/*
 * music.c — 网易云在线音乐播放 (自建代理方案)
 *
 * 与喵伴 MIAOBAN 的 ncm_proxy_v11.js 使用完全相同的三个接口:
 *   GET {base}/api/search?keyword=<urlenc>&limit=10&lean=1
 *   GET {base}/api/stream?id=<songId>&br=128000
 *   GET {base}/api/lyric?id=<songId>
 *
 * 为什么必须走代理: 网易 CDN 直链带防盗链, 设备端既没有 Cookie 也带不了
 * 正确的 Referer, 直连一律 403("You don't have permission..."). 所以由
 * 服务器取流再转发 —— 设备只管播。
 *
 * 播放链路:
 *   esp_http_client 分块读 → esp_audio_simple_dec(嗅探 MP3/AAC/FLAC)
 *   → 立体声降单声道 → speaker_write(I2S, 采样率随音乐自动切到 44.1kHz)
 */
#include "music.h"
#include "speaker_max98357a.h"
#include "xiaozhi.h"      /* 音乐互斥: ai_client_set_audio_suspended() */
#include "face.h"         /* 播音乐时切到「听歌」表情 FACE_MUSIC */
#include "light.h"        /* 音乐律动: light_music_start/level/stop */
#include "persona.h"      /* 放歌也影响心情(心情+5) */

/* 播音乐期间按住「听歌」表情。
   给一个足够长的时长(10 分钟, 比任何一首歌都长), 并在播放循环里周期性补按,
   这样整首歌期间表情都不会被自动情绪循环顶掉。 */
#define MUSIC_FACE_HOLD_MS   (10 * 60 * 1000)

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"      /* 必须在 stream_buffer.h 之前 */
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"

static const char *TAG = "music";

/* ======================= 参数 ======================= */
/* ★ 默认【不预置】任何代理地址 —— 开源项目里不能带作者的账号/服务器。
 * 使用方法:
 *   1. 用本仓库 server/music_proxy.js 自建一个代理(见 docs/MUSIC_PROXY.md)
 *   2. 浏览器打开代理地址扫码登录网易云音乐
 *   3. 在网页控制台「音乐」页填入代理地址并保存(存 NVS, 掉电不丢)
 * 填好之前音乐功能会提示"未设置音乐代理地址"。 */
#define MUS_DEFAULT_BASE  ""
#define MUS_TASK_STACK    10240
/* ★ 优先级 5 + 钉在核 1:
   核 0 上已经挤了 WiFi、舵机(prio6, 50Hz)、灯效、传感器、协议任务,
   音乐任务放核 0 抢不到 CPU 就会一卡一卡。核 1 只有音频(prio6)和表情(prio5),
   本任务 prio5 排在音频之后, 互不干扰。 */
/* ★ 必须高于表情任务(face 是 5)。实测数据说话:
   解码只要 4ms、写 I2S 16ms, 合计 20ms, 但墙钟一轮要 43ms ——
   剩下 23ms 全是在和同优先级的 face 任务抢 CPU(它渲染一帧要算 8192 像素的
   SDF, 还要用 200kHz 的 I2C 发一整帧, 一帧就是 50ms 量级)。
   AI 音频任务也是 6, 但播音乐时它已被挂起, 不冲突。 */
#define MUS_TASK_PRIO     6
#define MUS_TASK_CORE     1
/* ★ 读缓冲: 越大越抗抖动, 但"压缩搬运"的代价也线性增长。
   实测 48KB + 3/4 水位时: 每轮要搬 36KB PSRAM 数据, 单这一步就 10~21ms,
   占了整整一轮(43ms)的一半 —— 播放速度掉到 60%, 这才是卡顿的元凶。
   折中取 32KB(约 1 秒 128kbps 音频)+ 半水位:
   搬运量降到 16KB、且每消耗 16KB 才发生一次, 分摊下来只剩零点几毫秒;
   1 秒的缓冲对家庭 WiFi 也足够(实测"见底=0 次", 网络从未供不上)。 */
#define MUS_READ_BUF      (32 * 1024)
#define MUS_READ_HIWAT    (MUS_READ_BUF / 2)
/* ★ 单次向 HTTP 请求的最大字节数。
   必须给得"小": esp_http_client_read() 要凑满请求长度才返回, 而 ESP 的
   TCP 接收窗口只有几 KB —— 一次要 16KB 就会阻塞几百毫秒(实测每轮摊 20ms,
   是音乐卡顿的元凶); 要 2KB 则基本立刻返回。 */
#define MUS_READ_CHUNK    2048
#define MUS_DEC_OUT_MAX   (24 * 1024)
#define MUS_BODY_MAX      (16 * 1024)
/* 搜索结果/歌单里最多放多少首。
   原来只有 5 —— 现在网页要浏览"我的歌单"(动辄上百首), 5 首太少了。
   30 首 × 152 字节 = 4.5KB(静态 BSS), 完全放得下。 */
#define MUS_MAX_RESULTS   30
/* 我的歌单最多列出多少个(实测这个账号有 50 个) */
#define MUS_MAX_PLAYLISTS 64
#define MUS_ID_LEN        24
#define MUS_NAME_LEN      64

/* ======================= 状态 ======================= */
typedef struct { char id[MUS_ID_LEN]; char name[MUS_NAME_LEN]; char artist[MUS_NAME_LEN]; } mus_song_t;

/* MUS_SKIP = 这首放不了(多为 VIP/版权受限), 但【不是】致命错误:
   外层会自动跳去试搜索结果的下一首。原来直接判成 MUS_ERROR 就整轮放弃,
   表现就是"AI 说正在播放, 但一点声音都没有"。 */
/* MUS_SKIP  = 这首放不了(多为 VIP/版权受限), 但【不是】致命错误:
               外层会自动跳去试搜索结果的下一首。原来直接判成 MUS_ERROR
               就整轮放弃, 表现就是"AI 说正在播放, 但一点声音都没有"。
   MUS_RESUME= 网络断了导致流中断, 但歌还没放完 —— 外层【不换歌】,
               从断点继续(代理支持 HTTP Range, 实测返回 206)。
               原来这里直接跳到下一首, 用户听到的就是"歌播一半就没了"。 */
typedef enum { MUS_IDLE = 0, MUS_SEARCHING, MUS_PLAYING, MUS_PAUSED,
               MUS_SKIP, MUS_RESUME, MUS_ERROR } mus_state_t;

static struct {
    mus_state_t  state;
    char         base[128];
    mus_song_t   res[MUS_MAX_RESULTS];
    int          res_cnt;
    int          idx;
    int          rate;          /* 当前解码采样率 */
    uint32_t     played_bytes;  /* 已送出的 PCM 字节(诊断) */
    char         err[160];
    TaskHandle_t task;
    bool         running;       /* 任务在跑播放循环 */
    bool         skip_search;   /* 已给定 id, 跳过搜索直接播 */
    char         pending_kw[MUS_NAME_LEN];   /* 待搜索的歌名 */
    volatile bool stop_req;
    volatile bool pause_req;
} mus;

/* 网页"只搜索不播放"的请求标志(见 music_search_only) */
static volatile bool s_search_only = false;

/* ================= 我的歌单(网易云账号的歌单) =================
 * 代理实测支持:
 *   GET {base}/api/user/playlists          → 歌单列表 {id,name,count,cover,creator}
 *   GET {base}/api/playlist/songs?id=&limit=&offset=  → 歌单里的歌
 *        (注意: 分页要用 limit+offset; count 参数代理不认)
 *
 * 这两个请求都【交给音乐任务去做】, 不在 httpd 里做 —— httpd 任务栈只有
 * 4KB, 而这里要跑 HTTP + cJSON 解析几 KB 的 JSON, 有栈溢出风险。
 * 网页这边用"请求 + 轮询"的方式等结果。 */
typedef struct {
    char id[MUS_ID_LEN];
    char name[MUS_NAME_LEN];
    int  count;
} mus_pl_t;

static mus_pl_t          s_pls[MUS_MAX_PLAYLISTS];
static volatile int      s_pl_cnt   = 0;
static volatile bool     s_pl_ready = false;   /* 歌单列表已拉到 */
static volatile bool     s_pl_req   = false;   /* 请求拉歌单列表 */
static volatile bool     s_pls_req  = false;   /* 请求拉某个歌单的歌 */
static volatile bool     s_pls_ready = false;  /* 某个歌单的歌已拉到 */
static char              s_pl_want_id[MUS_ID_LEN];   /* 想拉哪个歌单 */
static char              s_pl_cur_name[MUS_NAME_LEN]; /* 当前装进歌单的那个歌单名 */

/* 复位成语音用的采样率(否则对话会变调) */
static void restore_voice_rate(void)
{
    if (mus.rate != 24000) {
        speaker_set_sample_rate(24000);
        mus.rate = 24000;
    }
}

/* ======================= 小工具 ======================= */

/* URL 百分号编码(按字节, 支持 UTF-8 中文) */
static void url_encode(const char *s, char *out, int max)
{
    static const char *hex = "0123456789ABCDEF";
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o < max - 4; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else if (*p == ' ') {
            out[o++] = '+';                     /* 空格用 + , 部分服务端更宽容 */
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0x0F];
        }
    }
    out[o] = '\0';
}

static esp_http_client_handle_t http_open_range(const char *url, int timeout_ms, uint32_t from)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* https 才用得上, http 无害 */
        .keep_alive_enable = false,
        /* 默认 512 字节偏小, 给到 1024 就够(音频流靠的是我们自己那 24KB 环形缓冲,
           不靠这一层)。★ 别给大: 它是内部 RAM, 而内部 RAM 还要留给
           WebSocket 任务的栈(必须连续分配) —— 给 4KB 时实测 WS 直接建不出来。 */
        .buffer_size       = 1024,
        .buffer_size_tx    = 512,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    /* ★ 断点续传: 必须在 open() 之前设头(open 会立刻把请求发出去)。
       代理实测支持 Range(返回 206 + Content-Range), 所以网络断掉之后可以
       从断点接着放 —— 不重头、也不跳歌。 */
    char rng[48];
    if (from > 0) {
        snprintf(rng, sizeof(rng), "bytes=%u-", (unsigned)from);
        esp_http_client_set_header(c, "Range", rng);
    }
    if (esp_http_client_open(c, 0) != ESP_OK) {
        ESP_LOGE(TAG, "打不开 %s", url);
        esp_http_client_cleanup(c);
        return NULL;
    }
    return c;
}

static esp_http_client_handle_t http_open(const char *url, int timeout_ms)
{
    return http_open_range(url, timeout_ms, 0);
}

/* 读一个较小的文本响应(搜索/歌词用) */
static int http_get_text(const char *url, char *out, int out_max)
{
    esp_http_client_handle_t c = http_open(url, 12000);
    if (!c) return -1;

    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d <- %s", status, url);
        /* 把状态码带回去 —— 之前一律写"连不上代理", 而实际多半是
           代理自己连不上网易云(返回 500/ENOBUFS) 或没登录(301/250),
           报文完全不同, 不写清楚根本没法排查。 */
        char tail[300] = {0};
        int r = esp_http_client_read(c, tail, sizeof(tail) - 1);
        if (r > 0) tail[r] = '\0';
        esp_http_client_cleanup(c);
        snprintf(mus.err, sizeof(mus.err), "代理返回 HTTP %d %.80s", status, tail);
        return -2;
    }
    int total = 0;
    while (total < out_max - 1) {
        int r = esp_http_client_read(c, out + total, out_max - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    out[total] = '\0';
    esp_http_client_cleanup(c);
    return total;
}

/* 按文件头猜音频格式(simple_dec 需要明确类型) */
static esp_audio_simple_dec_type_t sniff_type(const uint8_t *b, int n)
{
    if (n >= 4) {
        if (!memcmp(b, "fLaC", 4))   return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
        if (!memcmp(b, "RIFF", 4))   return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
        if (!memcmp(b, "OggS", 4))   return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
        if (!memcmp(b + 4, "ftyp", 4)) return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
        if (!memcmp(b, "ID3", 3))    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        /* MP3 帧同步: 11 个 1 */
        if (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        /* ADTS AAC: syncword 0xFFF + layer 0 */
        if (b[0] == 0xFF && (b[1] & 0xF6) == 0xF0) return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;   /* 代理默认回 MP3 */
}

/* ======================= 搜索 ======================= */
static bool mus_search(const char *kw)
{
    mus.res_cnt = 0;
    mus.idx     = 0;
    /* 按歌名搜索时清掉"当前歌单名", 免得网页上还挂着上一次浏览的歌单名 */
    s_pl_cur_name[0] = '\0';

    if (!mus.base[0]) {
        snprintf(mus.err, sizeof(mus.err),
                 "未设置音乐代理地址 —— 网页「音乐」页填入, 部署见 docs/MUSIC_PROXY.md");
        return false;
    }

    char enc[200];
    url_encode(kw, enc, sizeof(enc));

    char url[400];
    snprintf(url, sizeof(url), "%s/api/search?keyword=%s&limit=%d&lean=1",
             mus.base, enc, MUS_MAX_RESULTS);

    char *body = heap_caps_malloc(MUS_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (!body) { snprintf(mus.err, sizeof(mus.err), "内存不足"); return false; }

    int n = http_get_text(url, body, MUS_BODY_MAX);
    ESP_LOGI(TAG, "搜索「%s」-> %d 字节: %.160s", kw, n, n > 0 ? body : "");

    if (n <= 0) {
        /* n == -2 时 http_get_text 已经把"HTTP 状态码 + 代理返回的报文"
           写进 mus.err 了, 不要再覆盖掉 —— 那句才是排查的关键
           (例如代理回 {"code":500,"message":"code=ENOBUFS"})。 */
        if (n == -1)
            snprintf(mus.err, sizeof(mus.err), "连不上代理 %s", mus.base);
        free(body);
        return false;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { snprintf(mus.err, sizeof(mus.err), "返回不是合法 JSON"); return false; }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code && cJSON_IsNumber(code) && code->valueint != 200) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        snprintf(mus.err, sizeof(mus.err), "代理报错 %d: %s",
                 code->valueint, (msg && cJSON_IsString(msg)) ? msg->valuestring : "?");
        cJSON_Delete(root);
        return false;
    }

    /* 兼容几种返回形态: data[] / data.records[] / result.songs[] */
    cJSON *arr = cJSON_GetObjectItem(root, "data");
    if (arr && cJSON_IsObject(arr)) {
        cJSON *rec = cJSON_GetObjectItem(arr, "records");
        if (rec && cJSON_IsArray(rec)) arr = rec;
        else {
            cJSON *s = cJSON_GetObjectItem(arr, "songs");
            arr = (s && cJSON_IsArray(s)) ? s : NULL;
        }
    }
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON *r = cJSON_GetObjectItem(root, "result");
        cJSON *s = r ? cJSON_GetObjectItem(r, "songs") : NULL;
        arr = (s && cJSON_IsArray(s)) ? s : NULL;
    }
    if (!arr) { snprintf(mus.err, sizeof(mus.err), "返回里没有歌曲列表"); cJSON_Delete(root); return false; }

    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr) {
        if (mus.res_cnt >= MUS_MAX_RESULTS) break;
        cJSON *jid  = cJSON_GetObjectItem(it, "id");
        cJSON *jnm  = cJSON_GetObjectItem(it, "name");
        cJSON *jar  = cJSON_GetObjectItem(it, "artist");
        if (!jid) continue;

        mus_song_t *d = &mus.res[mus.res_cnt];
        /* id 可能是数字也可能是字符串 */
        if (cJSON_IsString(jid)) snprintf(d->id, sizeof(d->id), "%s", jid->valuestring);
        else                     snprintf(d->id, sizeof(d->id), "%d", jid->valueint);

        if (jnm && cJSON_IsString(jnm)) snprintf(d->name, sizeof(d->name), "%s", jnm->valuestring);
        else                            snprintf(d->name, sizeof(d->name), "%s", kw);

        if (jar && cJSON_IsString(jar)) snprintf(d->artist, sizeof(d->artist), "%s", jar->valuestring);
        else {
            /* 兼容 artists[].name 形态 */
            cJSON *arts = cJSON_GetObjectItem(it, "artists");
            d->artist[0] = '\0';
            if (arts && cJSON_IsArray(arts)) {
                cJSON *a0 = cJSON_GetArrayItem(arts, 0);
                cJSON *an = a0 ? cJSON_GetObjectItem(a0, "name") : NULL;
                if (an && cJSON_IsString(an)) snprintf(d->artist, sizeof(d->artist), "%s", an->valuestring);
            }
        }
        ESP_LOGI(TAG, "  结果%d: id=%s  %s - %s", mus.res_cnt + 1, d->id, d->name, d->artist);
        mus.res_cnt++;
    }
    cJSON_Delete(root);

    if (mus.res_cnt == 0) { snprintf(mus.err, sizeof(mus.err), "没搜到「%s」", kw); return false; }
    return true;
}

/* ======================= 播放一首 ======================= */
/* ==================== 网络读取任务 (与播放彻底解耦) ====================
 *
 * ★★★ 这是"音乐一卡一卡"的根治方案 ★★★
 *
 * 原来的做法是播放循环自己调 esp_http_client_read() 去补数据。但这个函数会
 * 停在"socket 缓冲区被读空"的那一刻, 要等下一批数据到达才返回 —— 而且 TCP
 * 窗口一关, 代理就暂停推送, 我们再读时它才重新开始, 这个来回是 100ms 量级。
 * 实测每轮摊到 6~20ms, 全部加载在 26ms 的实时预算上, 播放速度因此只有
 * 实时的 60%~90%, I2S 时不时没数据 → 一直断续。
 *
 * 关键点: 这个等待【没法】和播放重叠 —— 因为播放循环自己在等。
 * 拆成独立任务后就完全不一样了:
 *   读任务:  HTTP → 环形缓冲 (等数据的时间全在这里, 不关播放的事)
 *   播放任务: 环形缓冲 → 解码 → 写 I2S
 * 播放任务的实际工作只剩 解码(3ms) + 写I2S(16ms) = 19ms < 26ms,
 * 于是无论网络怎么抖, 播放速度都稳定在实时。
 * 官方 gmf 流水线也是这么分层的(读/解码/输出各自独立 stage)。
 */
/* 读任务 → 解码 的环形缓冲。
 * ★ 它就是"能顶住多久网络抖动"的余量: 24KB ≈ 1.5 秒的 128kbps 音频,
 *   实测网络每隔几分钟会停 2 秒左右, 1.5 秒的余量必然见底 → 判"流断了"
 *   → 虽然能从断点接上, 但那几秒是静音的, 用户能听出来。
 *   加到 64KB ≈ 4 秒: 4 秒以内的停顿被完全吃掉, 一点都听不出来。
 *   代价只是 64KB PSRAM(还剩 7.8MB), 完全不心疼。 */
#define MUS_NET_RING      (64 * 1024)
#define MUS_NET_CHUNK     2048          /* 单次 HTTP 读取量(小才能及时返回) */
#define MUS_DEC_IN        4096          /* 解码器输入暂存窗口(必须连续) */

static StreamBufferHandle_t s_net;          /* 环形缓冲 */
static StaticStreamBuffer_t s_net_sb;       /* 控制块(内部 RAM, 很小) */
static uint8_t             *s_net_store;    /* 存储区(必须自己给 PSRAM!) */
static esp_http_client_handle_t volatile s_net_http;  /* 本首歌的 HTTP 句柄 */
static volatile bool s_net_eof;             /* 流已结束 */
static volatile int  s_net_err;             /* 读出错码(0=正常) */
/* ★★ 读任务改成【常驻】: 开机创建一次, 之后每首歌只是"换数据源" ★★
 *
 * 原来每首歌都要 xTaskCreatePinnedToCore(mus_net_task, "mus_net", 4096, ...)
 * 重新建一个任务 —— 而播音乐时内部 RAM 已经被占得很碎, 4096 字节【连续】
 * 内部 RAM 根本拿不到。实测日志里 "读任务: 创建失败" 出现了 45 次,
 * 后果是一条完整的错误链:
 *   读任务建不起 → 环形缓冲永远是空的 → 播放循环 2 秒拿不到数据判"流断了"
 *   → 断点续传重试 4 次(每次都还建不起) → 放弃 → 换下一首 → 周而复始
 * 用户看到的就是"总是自动切换, 一首歌都放不完"(实测 9 首歌 0 首正常播完)。
 *
 * 常驻任务彻底消除这个问题, 顺带也消除了"上一首的读任务还没退出、新任务
 * 就没被创建"的那个竞态。 */
static volatile TaskHandle_t s_net_task;    /* 常驻读任务句柄(创建后长期存在) */
static volatile bool s_net_active;          /* 是否在服务当前这首歌 */
static volatile bool s_net_busy;            /* 正在 socket 上读 → 不能关 HTTP 句柄 */

/* ---- 断点续传 ----
 * 网络断掉(实测: WS 断开后整条约 30 秒不通)会把音乐的 TCP 连接也拖死。
 * 原来这时候就直接跳到下一首, 用户听到的就是"歌播一半就没了"。
 * 代理支持 HTTP Range(实测 206), 所以这里记住"读到第几个字节",
 * 流断了就从那个位置接着放 —— 不重头、也不跳歌。 */
static volatile uint32_t s_stream_pos  = 0;   /* 本首累计已读的 MP3 字节数 */
static uint32_t          s_resume_off  = 0;   /* >0 表示本次是续传, 从这继续 */
static int               s_resume_try  = 0;   /* 本首已续传次数(防死循环) */
static int               s_last_dtype  = 0;   /* 上次嗅探到的格式(续传时跳过嗅探要用) */
#define MUS_RESUME_MAX  4                     /* 最多续传 4 次, 超过就认输换歌 */

static void mus_net_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(MUS_NET_CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "读任务: 缓冲内存不足, 任务退出");
        s_net_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "读任务已就绪(常驻, 不再每首歌重建)");

    for (;;) {
        /* 空闲: 这台设备正在听歌之外的时候, 这个任务什么都不做 */
        if (!s_net_active || s_net_eof || mus.stop_req) {
            s_net_busy = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        esp_http_client_handle_t h = s_net_http;
        if (!h) { s_net_busy = false; vTaskDelay(pdMS_TO_TICKS(20)); continue; }

        /* s_net_busy 期间绝不能被 cleanup 关掉句柄(会 use-after-free) */
        s_net_busy = true;
        int rs = esp_http_client_read(h, (char *)buf, MUS_NET_CHUNK);
        s_net_busy = false;

        if (rs > 0) {
            s_stream_pos += (uint32_t)rs;      /* 记录进度, 供断点续传用 */
            size_t off = 0;
            while (off < (size_t)rs && s_net_active && !mus.stop_req) {
                /* 环满时阻塞 200ms, 让播放任务先消费 */
                size_t n = xStreamBufferSend(s_net, buf + off, (size_t)rs - off,
                                             pdMS_TO_TICKS(200));
                if (n == 0) break;
                off += n;
            }
        } else if (rs == 0) {
            s_net_eof = true;               /* 流正常读完 */
        } else {
            s_net_err = rs;
            s_net_eof = true;
        }
    }
}

/* 开机创建常驻读任务(在 music_init 里调一次)。
   这时候内存最宽裕, 一定能建起来 —— 这正是改成常驻的核心动机。 */
static void mus_net_init(void)
{
    if (!s_net) {
        s_net_store = heap_caps_malloc(MUS_NET_RING, MALLOC_CAP_SPIRAM);
        if (s_net_store)
            s_net = xStreamBufferCreateStatic(MUS_NET_RING, 1, s_net_store, &s_net_sb);
    }
    if (!s_net) { ESP_LOGE(TAG, "读任务: 环形缓冲创建失败(PSRAM 不足)"); return; }
    if (s_net_task) return;

    /* ★★ 栈必须放 PSRAM —— 这是"小智叫不出来"的根治点 ★★
     *
     * 读任务现在是【常驻】的(开机建好、一辈子不退出), 所以它的栈是【永久】
     * 占用内部 RAM 的。而 WebSocket 任务的栈(5KB+)必须能【连续】分配到内部
     * RAM, 否则就是那句致命的:
     *     E websocket_client: Error create websocket task
     * → WS 永远建不起来 → 唤醒词照常工作(ww=1), 但连不上服务器, 表现就是
     *   "怎么叫小智都没反应"。
     * 实测改常驻之后: 内部RAM 最大连续块只剩 2304 字节, WS 必然失败。
     *
     * 放 PSRAM 是安全的: 这个任务只做 esp_http_client_read(代理是 http,
     * 连 TLS 都没有) + xStreamBufferSend + heap_caps_malloc, 全程不碰 flash
     * —— 而"PSRAM 栈不安全"只在任务自己触发 flash 操作(cache 被关)时才成立。
     * 项目里 face 任务(8192)早就这么做了, 这里沿用同一套办法。
     */
    if (xTaskCreatePinnedToCoreWithCaps(mus_net_task, "mus_net", 4096, NULL, 6,
                                        (TaskHandle_t *)&s_net_task, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "读任务: PSRAM 栈创建失败, 回退到内部 RAM");
        if (xTaskCreatePinnedToCore(mus_net_task, "mus_net", 4096, NULL, 6,
                                    (TaskHandle_t *)&s_net_task, 1) != pdPASS) {
            ESP_LOGE(TAG, "读任务: 创建失败(内部 RAM 不足)");
            s_net_task = NULL;
        }
    } else {
        ESP_LOGI(TAG, "读任务栈: PSRAM (内部 RAM 留给 WebSocket)");
    }
}

/* 换数据源(每首歌 / 每次断点续传调用)。
   不再创建任务 —— 只是"把活派给那个常驻的读任务"。
   sniff 时已经读进来的字节也要塞进环里, 否则开头会丢。 */
static void mus_net_start(esp_http_client_handle_t c, const uint8_t *pre, size_t pre_len)
{
    mus_net_init();                     /* 兜底: 万一开机那次没建成 */
    if (!s_net) { ESP_LOGE(TAG, "读任务: 环形缓冲不可用"); return; }

    /* ① 先让读任务把手从【旧】socket 上拿开。
          不然后面 cleanup 关掉旧句柄时它还在读 → use-after-free 崩溃。 */
    s_net_active = false;
    for (int i = 0; i < 150 && s_net_busy; i++) vTaskDelay(pdMS_TO_TICKS(20));  /* 最多 3 秒 */
    if (s_net_busy) ESP_LOGW(TAG, "读任务仍卡在旧 socket 上, 强行换源");

    /* ② 清环 + 复位状态, 换上新句柄 */
    xStreamBufferReset(s_net);
    s_net_eof  = false;
    s_net_err  = 0;
    /* ★ 注意: 这里【不能】清 s_stream_pos —— 它由 mus_play_one 管理,
       嗅探读进来的那几个字节也算进度(断点续传要用)。清零会导致续传位置偏移。 */
    if (pre_len) xStreamBufferSend(s_net, pre, pre_len, pdMS_TO_TICKS(200));
    s_net_http = c;

    /* ③ 放行 */
    s_net_active = true;
}

/* 让读任务松开 HTTP 句柄 —— 之后调用者才能安全地 esp_http_client_cleanup()。
   超时给得宽(8 秒)是因为卡在 socket 上时只能等它自己回来;
   配合把音乐 HTTP 的超时降到 5 秒, 实际不会等这么久。 */
static void mus_net_stop(void)
{
    s_net_active = false;
    for (int i = 0; i < 400 && s_net_busy; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_net_busy) ESP_LOGW(TAG, "读任务 8 秒仍卡在 socket 上");
    s_net_http = NULL;
    s_net_eof  = true;
}

/* ---------- 拉"我的歌单"列表 ---------- */
static void mus_fetch_playlists(void)
{
    if (!mus.base[0]) { snprintf(mus.err, sizeof(mus.err), "未设置音乐代理地址"); s_pl_ready = true; return; }

    char url[220];
    snprintf(url, sizeof(url), "%s/api/user/playlists", mus.base);

    char *body = heap_caps_malloc(MUS_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (!body) { ESP_LOGE(TAG, "歌单: 内存不足"); s_pl_ready = true; return; }

    int n = http_get_text(url, body, MUS_BODY_MAX);
    if (n <= 0) {
        ESP_LOGW(TAG, "我的歌单: 拉取失败(%d) %s", n, mus.err);
        free(body); s_pl_ready = true; return;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { ESP_LOGW(TAG, "我的歌单: JSON 解析失败"); s_pl_ready = true; return; }

    s_pl_cnt = 0;
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsArray(data)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, data) {
            if (s_pl_cnt >= MUS_MAX_PLAYLISTS) break;
            cJSON *id = cJSON_GetObjectItem(it, "id");
            cJSON *nm = cJSON_GetObjectItem(it, "name");
            cJSON *ct = cJSON_GetObjectItem(it, "count");
            if (!cJSON_IsString(id) || !id->valuestring[0]) continue;
            mus_pl_t *p = &s_pls[s_pl_cnt];
            snprintf(p->id,   sizeof(p->id),   "%s", id->valuestring);
            snprintf(p->name, sizeof(p->name), "%s",
                     cJSON_IsString(nm) ? nm->valuestring : "(未命名)");
            p->count = cJSON_IsNumber(ct) ? ct->valueint : 0;
            s_pl_cnt++;
        }
    }
    cJSON_Delete(root);
    s_pl_ready = true;
    ESP_LOGI(TAG, "我的歌单: 拉到 %d 个", s_pl_cnt);
}

/* ---------- 把某个歌单的歌装进 mus.res[] ----------
   装进去之后就复用了"搜索结果/点播/下一首"那一整套 UI 与逻辑, 不用另写一套。 */
static void mus_fetch_playlist_songs(void)
{
    if (!mus.base[0]) { snprintf(mus.err, sizeof(mus.err), "未设置音乐代理地址"); s_pls_ready = true; return; }

    char url[300];
    /* ★ 分页必须用 limit + offset —— 实测代理不认 count 参数(会返回全部) */
    snprintf(url, sizeof(url), "%s/api/playlist/songs?id=%s&limit=%d&offset=0",
             mus.base, s_pl_want_id, MUS_MAX_RESULTS);

    s_pl_cur_name[0] = '\0';
    mus.res_cnt = 0;
    mus.idx     = 0;

    char *body = heap_caps_malloc(MUS_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (!body) { ESP_LOGE(TAG, "歌单: 内存不足"); s_pls_ready = true; return; }

    int n = http_get_text(url, body, MUS_BODY_MAX);
    if (n <= 0) {
        ESP_LOGW(TAG, "歌单 %s: 拉歌曲失败(%d)", s_pl_want_id, n);
        free(body); s_pls_ready = true; return;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { ESP_LOGW(TAG, "歌单: JSON 解析失败"); s_pls_ready = true; return; }

    cJSON *nm = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(nm))
        snprintf(s_pl_cur_name, sizeof(s_pl_cur_name), "%s", nm->valuestring);

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsArray(data)) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, data) {
            if (mus.res_cnt >= MUS_MAX_RESULTS) break;
            cJSON *id = cJSON_GetObjectItem(it, "id");
            cJSON *sn = cJSON_GetObjectItem(it, "name");
            cJSON *ar = cJSON_GetObjectItem(it, "artist");
            if (!cJSON_IsString(id) || !id->valuestring[0]) continue;
            mus_song_t *d = &mus.res[mus.res_cnt++];
            snprintf(d->id,     sizeof(d->id),     "%s", id->valuestring);
            snprintf(d->name,   sizeof(d->name),   "%s",
                     cJSON_IsString(sn) ? sn->valuestring : "(未知)");
            snprintf(d->artist, sizeof(d->artist), "%s",
                     cJSON_IsString(ar) ? ar->valuestring : "");
        }
    }
    cJSON_Delete(root);
    s_pls_ready = true;
    ESP_LOGI(TAG, "歌单「%s」: 装进 %d 首, 等网页点选", s_pl_cur_name, mus.res_cnt);
}

/* 音频电平(0..1): 取这一帧的峰值。
 * 峰值比均方根更"跟鼓点" —— 鼓一响立刻顶到 1.0, 灯环的律动才跳得起来。
 * 每帧 1152 个样本, 开销可忽略。 */
static float pcm_peak(const int16_t *p, int n)
{
    int32_t pk = 0;
    for (int i = 0; i < n; i++) {
        int32_t a = p[i];
        if (a < 0) a = -a;
        if (a > pk) pk = a;
    }
    return (float)pk / 32768.0f;
}

static void mus_play_one(void)
{
    const mus_song_t *s = &mus.res[mus.idx];

    if (!mus.base[0]) {
        snprintf(mus.err, sizeof(mus.err), "未设置音乐代理地址");
        mus.state = MUS_ERROR;
        return;
    }

    /* ★ 是"新的一首"还是"断点续传"? (见 s_resume_off 的说明) */
    bool resuming = (s_resume_off > 0);
    if (!resuming) { s_stream_pos = 0; s_resume_try = 0; }

    char url[300];
    snprintf(url, sizeof(url), "%s/api/stream?id=%s&br=128000", mus.base, s->id);

    /* ★ 超时给 5 秒(原来 15 秒)。
       读任务卡在 socket 上时, cleanup 只能干等它自己回来 —— 15 秒太久,
       会把"换歌/续传"整体拖住; 而每次只读 2KB, 5 秒足够任何正常网络返回。 */
    esp_http_client_handle_t c = http_open_range(url, 5000, resuming ? s_resume_off : 0);
    if (!c) { snprintf(mus.err, sizeof(mus.err), "打不开音频流"); mus.state = MUS_ERROR; return; }

    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    int clen   = esp_http_client_get_content_length(c);
    /* 代理取流失败时回的是 JSON(404); 网易的"错误页"也很短。
       200 = 正常整段; 206 = 我们要的断点续传, 同样算成功。 */
    if (status != 200 && status != 206) {
        /* ★ 代理对 VIP/版权受限的歌会回 404(JSON 里写明"无可用播放链接")。
           这不是我们的 bug, 也不该整轮放弃 —— 标记为 SKIP, 让外层去试
           搜索结果里的下一首(翻唱/其他版本往往可以放)。 */
        snprintf(mus.err, sizeof(mus.err),
                 "HTTP %d (多为 VIP/版权受限, 换下一个结果)", status);
        ESP_LOGW(TAG, "「%s」取流失败: %s → 自动跳过", s->name, mus.err);
        esp_http_client_cleanup(c);
        mus.state = MUS_SKIP;
        return;
    }
    if (resuming)
        ESP_LOGW(TAG, "断点续传「%s」: 从第 %u 字节继续 (HTTP %d, 剩余 %d 字节)",
                 s->name, (unsigned)s_resume_off, status, clen);
    else
        ESP_LOGI(TAG, "开始播放: %s - %s  (id=%s, HTTP %d, 长度 %d)",
                 s->name, s->artist, s->id, status, clen);

    uint8_t *inbuf  = heap_caps_malloc(MUS_READ_BUF, MALLOC_CAP_SPIRAM);
    uint8_t *outbuf = heap_caps_malloc(MUS_DEC_OUT_MAX, MALLOC_CAP_SPIRAM);
    int16_t *mono   = heap_caps_malloc(MUS_DEC_OUT_MAX / 2, MALLOC_CAP_SPIRAM);
    if (!inbuf || !outbuf || !mono) {
        /* ★ 必须打日志! 这条路径原来是完全静默的: 直接 goto cleanup 返回,
           而 mus.state 还停在 MUS_SEARCHING, 外层循环判定"不是 ERROR"就
           idx++ 播下一首 —— 表现就是"每首歌一秒就跳过去, 什么错都不报"。 */
        snprintf(mus.err, sizeof(mus.err), "内存不足(in/out/mono)");
        ESP_LOGE(TAG, "%s", mus.err);
        mus.state = MUS_ERROR;
        goto cleanup;
    }

    /* 先读一小段用于嗅探格式。
       ★ 断点续传时【跳过嗅探】: 格式和刚才那一段完全一样(记在 s_last_dtype),
         而且这 1KB 是从断点开始读的 —— 读它就等于白白跳过 1KB 的音频。 */
    int got = 0;
    if (!resuming) {
        while (got < 1024) {
            int r = esp_http_client_read(c, (char *)(inbuf + got), MUS_READ_BUF - got);
            if (r <= 0) break;
            got += r;
        }
        if (got <= 0) {
            snprintf(mus.err, sizeof(mus.err), "流是空的");
            ESP_LOGW(TAG, "「%s」%s → 自动跳过", s->name, mus.err);
            mus.state = MUS_SKIP;   /* 这首拿不到数据, 试下一个结果 */
            goto cleanup;
        }
        s_stream_pos = (uint32_t)got;                  /* 进度从这算起 */
        s_last_dtype = (int)sniff_type(inbuf, got);
        ESP_LOGI(TAG, "嗅探到音频格式 type=%d", s_last_dtype);
    }
    esp_audio_simple_dec_type_t dtype = (esp_audio_simple_dec_type_t)s_last_dtype;

    esp_audio_simple_dec_cfg_t dcfg = {
        .dec_type      = dtype,
        .dec_cfg       = NULL,
        .cfg_size      = 0,
        .use_frame_dec = false,     /* 让解析器自己切帧 */
    };
    esp_audio_simple_dec_handle_t dec = NULL;
    esp_audio_err_t dret = esp_audio_simple_dec_open(&dcfg, &dec);
    if (dret != ESP_AUDIO_ERR_OK || !dec) {
        /* ★ 这条路径原来是静默的(只写 mus.err 没打日志), 结果"每首一秒跳过去"
           却什么都不报, 排查了很久。以后所有提前返回都必须打日志。 */
        snprintf(mus.err, sizeof(mus.err), "解码器打开失败 (er=%d, 格式=%d)",
                 (int)dret, (int)dtype);
        ESP_LOGE(TAG, "%s", mus.err);
        mus.state = MUS_ERROR;
        goto cleanup;
    }

    mus.state        = MUS_PLAYING;
    mus.played_bytes = 0;
    int rate_set = 0;

    /* ★ 启动独立的网络读任务: 嗅探时已经读进来的 got 字节也交给它, 后面的
       读取全在它自己的任务里完成 —— 播放循环从此不再碰网络, 也就不会再被
       "等 socket 数据"卡住。详见 mus_net_task 上面的说明。
       此时 inbuf 只当前 MUS_DEC_IN 字节的"连续窗口"用, 不再当大缓冲。 */
    mus_net_start(c, inbuf, (size_t)got);

    uint32_t have = 0;      /* 窗口内有效字节数 */
    uint32_t off  = 0;      /* 已解码偏移 */

    int iter      = 0;
    int stall     = 0;
    int starve    = 0;      /* 缓冲空了但流没结束的连续次数(超过 2 秒才放弃) */
    bool died     = false;  /* 是"流被网络掐断"而不是正常播完 → 待会儿断点续传 */
    int underruns = 0;      /* 缓冲见底次数 —— "卡"的直接来源, 统计出来便于定位 */
    int64_t t_loop0  = esp_timer_get_time();
    int64_t t_dec_sum  = 0;     /* 解码累计耗时 */
    int64_t t_wr_sum   = 0;     /* 写 I2S 累计耗时 */
    int64_t t_fill_sum = 0;     /* 挪数据 + 补网络数据 累计耗时 */
    int64_t t_move_sum = 0;     /* 其中: memmove 搬运 */
    int64_t t_read_sum = 0;     /* 其中: 读网络 */
    int64_t t_dmx_sum  = 0;     /* 降混 累计耗时 */
    while (!mus.stop_req) {
        iter++;
        /* ====== 从环形缓冲补数据(非阻塞) —— 音乐卡顿的根治点 ======
         * 网络读取已经由 mus_net_task 独立完成, 这里只是把它的产物搬进
         * "连续窗口"(inbuf 前 MUS_DEC_IN 字节)给解码器用。
         * 接收超时给 0: 有数据就拿走, 没有就立刻返回继续解码 ——
         * 播放循环里【绝不等待网络】, 这正是卡顿的根治点。 */
        if ((have - off) < MUS_DEC_IN / 2) {
            if (off > 0) {
                memmove(inbuf, inbuf + off, have - off);
                have -= off; off = 0;
            }
            size_t want = (size_t)MUS_DEC_IN - have;
            if (want > 0 && s_net) {        /* s_net 为 NULL 时当作没数据, 靠 starve 兜底 */
                int64_t r0 = esp_timer_get_time();
                size_t n = xStreamBufferReceive(s_net, inbuf + have, want, 0);
                t_read_sum += esp_timer_get_time() - r0;
                have += (size_t)n;
            }
        }

        /* --- 暂停: 停在这里不动, 连接和缓冲都保持 ---
           暂停期间把 AI 音频放开: 音乐不响了, 没有抢 CPU 和喇叭的理由,
           用户可以正常喊小智。继续播放时再挂起。 */
        if (mus.pause_req) {
            mus.state = MUS_PAUSED;
            ai_client_set_audio_suspended(false);
            while (mus.pause_req && !mus.stop_req) {
                mus.state = MUS_PAUSED;
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (!mus.stop_req) {
                ai_client_set_audio_suspended(true);
                ESP_LOGI(TAG, "继续播放 → 重新挂起 AI 音频");
            }
        }
        if (mus.stop_req) break;
        mus.state = MUS_PLAYING;

        /* 环形缓冲见底 = 读任务供不上, 就是"卡"的直接来源。统计出来。 */
        size_t ringlv = s_net ? xStreamBufferBytesAvailable(s_net) : 0;
        if (ringlv < 2048) underruns++;

        /* 每 ~10 秒报一次缓冲水位和见底次数:
             · 缓冲一直很深、见底 0 次  → 播放是流畅的
             · 缓冲长期很浅、见底很多次 → 网络供不上, 要再放大缓冲
             · 缓冲很深但循环很慢       → CPU 抢不过别人, 要继续降对手的优先级 */
        /* 表情若被别的东西顶掉了(比如服务端下发的情绪), 补按一次 ——
           保证整首歌期间都稳定停在「听歌」表情上。 */
        if (iter % 150 == 0 && face_get_emotion() != FACE_MUSIC)
            face_set_emotion_hold(FACE_MUSIC, MUSIC_FACE_HOLD_MS);

        if (iter % 150 == 0)
            /* 单位都是【毫秒】: 解码+写入是实际干活时间, 与"均速"的差额
               就是被别的任务抢占/阻塞掉的时间 —— 这个差额大就说明要提优先级 */
            {
                int64_t it = iter ? iter : 1;
                int64_t avg = (esp_timer_get_time() - t_loop0) / 1000 / it;
                int64_t dec = t_dec_sum / it / 1000, wr = t_wr_sum / it / 1000;
                int64_t mv = t_move_sum / it / 1000, rd = t_read_sum / it / 1000;
                int64_t fl  = mv + rd, dm = t_dmx_sum / it / 1000;
                ESP_LOGI(TAG, "音乐播放中: 缓冲=%u/%u 见底=%d 均速=%dms/轮 "
                              "[解码=%d 写入=%d 搬运=%d 读网=%d 降混=%d 未计=%d] 已送=%u",
                         (unsigned)ringlv, MUS_NET_RING,
                         underruns, (int)avg,
                         (int)dec, (int)wr, (int)mv, (int)rd, (int)dm,
                         (int)(avg - dec - wr - fl - dm), (unsigned)mus.played_bytes);
            }

        if (have - off == 0) {
            if (s_net_eof && (!s_net || xStreamBufferBytesAvailable(s_net) == 0)) {
                ESP_LOGI(TAG, "播放结束(流读完): %s (共送 %u 字节 PCM)",
                         s->name, (unsigned)mus.played_bytes);
                break;
            }
            /* 环形缓冲空了但流还没结束 = 网络暂时没跟上。
               这里只是短暂等一拍, 数据一到就继续 —— 不会误判成"流结束"。 */
            if (++starve >= 100) {          /* 100 x 20ms = 2 秒 */
                ESP_LOGW(TAG, "流断了 (2 秒没数据, err=%d), 已播 %u 字节",
                         s_net_err, (unsigned)mus.played_bytes);
                died = true;                /* 不是正常播完 → 走断点续传, 不换歌 */
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        starve = 0;

        esp_audio_simple_dec_raw_t raw = {
            .buffer = inbuf + off,
            .len    = have - off,
            .eos    = false,
        };
        esp_audio_simple_dec_out_t out = {
            .buffer = outbuf,
            .len    = MUS_DEC_OUT_MAX,
        };
        int64_t t_d0 = esp_timer_get_time();
        esp_audio_err_t er = esp_audio_simple_dec_process(dec, &raw, &out);
        t_dec_sum += esp_timer_get_time() - t_d0;
        if (raw.consumed) off += raw.consumed;

        if (er == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > MUS_DEC_OUT_MAX) {
            ESP_LOGW(TAG, "解码缓冲不够(需要 %u), 跳过这段", (unsigned)out.needed_size);
            continue;
        }
        if (er != ESP_AUDIO_ERR_OK || out.decoded_size == 0) {
            /* 防死循环: 解码器既没吃数据也没吐数据, 再转下去就是空转 */
            if (raw.consumed == 0) {
                if (++stall >= 300) {
                    ESP_LOGW(TAG, "解码器卡住 (er=%d, 剩余 %u 字节), 放弃这首",
                             (int)er, (unsigned)(have - off));
                    break;
                }
                vTaskDelay(1);
            }
            continue;
        }
        stall = 0;

        /* 第一次解出数据时拿到真实采样率, 把 I2S 切过去 */
        if (!rate_set) {
            esp_audio_simple_dec_info_t info = {0};
            if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK && info.sample_rate) {
                ESP_LOGI(TAG, "解码参数: %uHz %uch %ubit",
                         (unsigned)info.sample_rate, info.channel, info.bits_per_sample);
                speaker_set_sample_rate((int)info.sample_rate);
                mus.rate = (int)info.sample_rate;
            } else {
                speaker_set_sample_rate(44100);
                mus.rate = 44100;
            }
            rate_set = 1;
        }

        if (mus.played_bytes == 0)
            ESP_LOGI(TAG, "首帧解码成功: PCM %u 字节, I2S 采样率 %d",
                     (unsigned)out.decoded_size, mus.rate);

        /* --- 立体声降单声道 (我们的 I2S 是 MONO) --- */
        int64_t t_x0 = esp_timer_get_time();
        const int16_t *pcm = (const int16_t *)out.buffer;
        int frames = (int)(out.decoded_size / sizeof(int16_t));
        /* 从 get_info 拿声道数; 拿不到就按 2 */
        esp_audio_simple_dec_info_t info2 = {0};
        int ch = (esp_audio_simple_dec_get_info(dec, &info2) == ESP_AUDIO_ERR_OK && info2.channel)
                 ? info2.channel : 2;
        if (ch >= 2) {
            int n = frames / ch;
            for (int i = 0; i < n; i++) {
                int32_t a = pcm[i * ch], b = pcm[i * ch + 1];
                mono[i] = (int16_t)((a + b) / 2);
            }
            t_dmx_sum += esp_timer_get_time() - t_x0;
            /* ★ 音乐律动: 把这一帧的峰值电平喂给灯环(0..1)。
               取峰值而不是均方根, 因为峰值更"跟鼓点", 灯看起来是在跳动。 */
            light_music_level(pcm_peak(mono, n));
            int64_t t_w0 = esp_timer_get_time();
            speaker_write(mono, (size_t)n, 1000);
            t_wr_sum += esp_timer_get_time() - t_w0;
            mus.played_bytes += (uint32_t)(n * 2);
        } else {
            t_dmx_sum += esp_timer_get_time() - t_x0;
            light_music_level(pcm_peak(pcm, frames));
            int64_t t_w0 = esp_timer_get_time();
            speaker_write(pcm, (size_t)frames, 1000);
            t_wr_sum += esp_timer_get_time() - t_w0;
            mus.played_bytes += out.decoded_size;
        }
    }

    ESP_LOGI(TAG, "退出播放循环: %s | 迭代 %d 次 | 已送 %u 字节 PCM%s",
             mus.stop_req ? "收到停止请求" : "流结束/出错",
             iter, (unsigned)mus.played_bytes, died ? " [网络断流]" : "");

    esp_audio_simple_dec_close(dec);

    /* ★★ 网络断了但歌还没放完 → 从断点续传, 【不要】跳到下一首 ★★
       代理支持 HTTP Range(实测返回 206), 所以能真的接上, 用户几乎听不出断过。
       原来这里直接 idx++ 换歌, 用户听到的就是"歌播一半就没了"。
       只在这首确实播过内容、且续传次数没超限时才做, 避免死循环。 */
    if (!mus.stop_req && died && s_stream_pos > 0 && s_resume_try < MUS_RESUME_MAX) {
        s_resume_try++;
        s_resume_off = s_stream_pos;
        mus.state    = MUS_RESUME;
        ESP_LOGW(TAG, "歌还没放完就断了 → 从第 %u 字节续传 (第 %d/%d 次)",
                 (unsigned)s_resume_off, s_resume_try, MUS_RESUME_MAX);
    } else {
        s_resume_off = 0;               /* 这首结束(或放弃), 下一首从头发起 */
    }

cleanup:
    /* ★ 必须先停掉读任务再关 HTTP: 否则读任务会拿着已经释放的句柄去读, 直接崩。
       mus_net_stop() 会等它真正退出(最多 2 秒)。 */
    mus_net_stop();
    if (inbuf)  free(inbuf);
    if (outbuf) free(outbuf);
    if (mono)   free(mono);
    esp_http_client_cleanup(c);

    /* 收尾: 采样率还给语音, 否则下一次对话声音会变调 */
    restore_voice_rate();
}

/* ======================= 任务 ======================= */
static void music_task(void *arg)
{
    while (1) {
        /* 空闲时: 顺手处理网页发来的"拉歌单"请求。
           ★ 放在这个任务里做(而不是 httpd 里): httpd 任务栈只有 4KB,
             而这两个请求要跑 HTTP + cJSON 解析几 KB 的 JSON, 会栈溢出。
           注意顺序: 先处理请求再 sleep, 否则网页要白等 100ms。 */
        if (!mus.running) {
            if (s_pl_req)  { s_pl_req  = false; mus_fetch_playlists();       continue; }
            if (s_pls_req) { s_pls_req = false; mus_fetch_playlist_songs();  continue; }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        mus.stop_req  = false;
        mus.pause_req = false;
        mus.state     = MUS_SEARCHING;
        mus.err[0]    = '\0';

        if (mus.skip_search) {
            mus.skip_search = false;      /* 已给定 id, 直接播 */
        } else if (!mus_search(mus.pending_kw)) {
            ESP_LOGW(TAG, "搜索失败: %s", mus.err);
            mus.state   = MUS_ERROR;
            mus.running = false;
            vTaskDelay(pdMS_TO_TICKS(400));
            continue;
        }

        /* ★ 音乐互斥: 进入播放前把整条 AI 音频链挂起。
           唤醒词引擎 + Opus 编码每 60ms 就要吃一大块 CPU, 和音乐解码抢同一个核,
           不挂起音乐必卡; 而且两者共用同一个功放, 会互相插话。
           放在这里(搜完、真要出声之前)而不是 music_play() 里 ——
           搜索阶段很快, 没必要让 AI 哑着。 */
        ai_client_set_audio_suspended(true);
        /* ★ 钉住功放: 播音乐时 speaker_write 会阻塞几百毫秒, 空闲任务会
           误判成"空闲"而中途关断功放 → 声音断一块(实测日志里 840ms 就关了)。 */
        speaker_set_hold(true);
        /* ★ 灯环进入音乐律动(记住放歌前的灯效, 歌停自动恢复) */
        light_music_start();
        persona_event_music(true);
        /* ★ 进入「听歌」表情, 并一直保持到整段音乐结束(见 MUSIC_FACE_HOLD_MS) */
        face_set_emotion_hold(FACE_MUSIC, MUSIC_FACE_HOLD_MS);

        /* ★★ 网页"只搜索不播放": 搜完就收工, 歌单留给网页点选 ★★
           必须放在【播放循环之前】—— 放在后面会把搜索结果的第 1 首顺带播出去
           (实测: 网页只是点"搜索", 喇叭里已经响起来了)。 */
        if (s_search_only) {
            s_search_only = false;
            mus.state   = MUS_IDLE;
            mus.running = false;
            ESP_LOGI(TAG, "只搜索不播放: 共 %d 个结果, 等网页点选", mus.res_cnt);
            continue;
        }

        /* 从当前下标开始连续播, 直到 stop。
           ★ 如果某首【放不了】(MUS_SKIP, 多为 VIP/版权受限), 会自动 idx++ 去
             试搜索结果里的下一首(翻唱/其他版本往往可以放); 只有 MUS_ERROR
             这种真正的故障才整轮放弃。 */
        int nskip = 0, nstart = 0;
        while (!mus.stop_req && mus.idx < mus.res_cnt) {
            mus_play_one();
            if (mus.stop_req) break;
            if (mus.state == MUS_ERROR) break;
            /* ★ 网络断流: 不换歌, 同一首重进(内部会用 Range 从断点继续) */
            if (mus.state == MUS_RESUME) continue;
            if (mus.state == MUS_SKIP) nskip++; else nstart++;
            mus.idx++;      /* 一首完了自动下一首 */
        }
        /* 全部结果都放不了 → 必须说清楚, 否则用户只看到"AI 说在播、却没声音" */
        if (!mus.stop_req && nstart == 0 && nskip > 0) {
            snprintf(mus.err, sizeof(mus.err),
                     "「%s」的 %d 个搜索结果都无可用播放链接(VIP/版权受限)",
                     mus.pending_kw, nskip);
            ESP_LOGE(TAG, "%s —— 没播出任何声音", mus.err);
            mus.state = MUS_ERROR;
        }

        mus.state   = MUS_IDLE;
        mus.running = false;
        restore_voice_rate();
        /* ★ 音乐结束 → 放开功放、立刻恢复 AI 音频, 用户可以马上喊小智 */
        speaker_set_hold(false);
        ai_client_set_audio_suspended(false);
        /* ★ 退出音乐律动, 灯环恢复放歌前的灯效 */
        light_music_stop();
        /* 表情回到平静, 让自动情绪循环接管 */
        if (!mus.pause_req) face_set_emotion(FACE_NORMAL);
        ESP_LOGI(TAG, "播放结束: 已恢复语音采样率, AI 音频已恢复");
    }
}

/* ======================= 公开 API ======================= */
static void load_base(void)
{
    nvs_handle_t h;
    mus.base[0] = '\0';
    if (nvs_open("music", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(mus.base);
        if (nvs_get_str(h, "base", mus.base, &n) != ESP_OK) mus.base[0] = '\0';
        nvs_close(h);
    }
    if (!mus.base[0]) snprintf(mus.base, sizeof(mus.base), "%s", MUS_DEFAULT_BASE);
}

void music_init(void)
{
    memset(&mus, 0, sizeof(mus));
    load_base();

    /* ★ 开机就把常驻读任务建好(这时内部 RAM 最宽裕)。
       以前是每首歌现建, 播音乐时内存已碎, 4096 字节连续内部 RAM 拿不到,
       实测 "读任务: 创建失败" 出现 45 次 —— 那是"总是自动切歌"的根因。 */
    mus_net_init();

    /* ★ 必须注册【两组】解码器, 少一个 MP3 就打不开:
       · esp_audio_dec_register_default()        裸解码器: MP3/AAC/FLAC/OPUS...
       · esp_audio_simple_dec_register_default() 简易解码器: 只含 WAV/M4A/TS/OGG
       只调后者的话, esp_audio_simple_dec_open(MP3) 会直接失败,
       而失败路径当时又是静默的 —— 表现就是"每首歌 1 秒就跳过去, 什么错都不报"。 */
    esp_audio_err_t r1 = esp_audio_dec_register_default();
    esp_audio_err_t r2 = esp_audio_simple_dec_register_default();
    ESP_LOGI(TAG, "解码器注册: 裸解码=%d 简易解码=%d", (int)r1, (int)r2);

    /* ★ 栈必须放 PSRAM —— 实测放内部 RAM 会把内部 RAM 吃到只剩
       2304 字节连续块(内部RAM=11547/2304), 结果 I2C 传输开始失败、
       表情刷不进屏幕(屏幕看着像没亮)。
       本任务不写 flash(NVS 只在 music_set_base() 里写, 那是从 HTTP/MCP
       回调调的, 不在这个任务里), 所以 PSRAM 栈是安全的。 */
    xTaskCreatePinnedToCoreWithCaps(music_task, "music", MUS_TASK_STACK, NULL,
                                    MUS_TASK_PRIO, &mus.task, MUS_TASK_CORE, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "音乐模块就绪, 代理 = %s", mus.base);
    /* 确认 CPU 频率真的生效了 —— MP3 解码是纯算力活,
       build 里的 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 若没被采纳, 这里会露馅 */
    ESP_LOGI(TAG, "CPU 频率配置 = %d MHz", (int)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
}

bool music_play(const char *song)
{
    if (!song || !song[0]) return false;
    /* 已经在放 -> 先停, 再换歌 */
    if (mus.running) { mus.stop_req = true; for (int i = 0; i < 60 && mus.running; i++) vTaskDelay(pdMS_TO_TICKS(50)); }
    snprintf(mus.pending_kw, sizeof(mus.pending_kw), "%s", song);
    mus.running = true;
    return true;
}

bool music_play_id(const char *song_id)
{
    if (!song_id || !song_id[0]) return false;
    /* 等待上一轮真正停下来(和 music_play 同样的做法) */
    if (mus.running) {
        mus.stop_req = true;
        for (int i = 0; i < 60 && mus.running; i++) vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* ★ 如果这首就在当前搜索结果里(网页点选的正是这种), 【不要清空列表】——
       把 idx 指过去就行。这样"歌单"还在, "下一首"能顺着播,
       而且不用再搜一次。原来这里会 res_cnt=1 把列表冲掉, 点一首歌单就没了。 */
    for (int i = 0; i < mus.res_cnt; i++) {
        if (strcmp(mus.res[i].id, song_id) == 0) {
            mus.idx         = i;
            mus.running     = true;
            mus.skip_search = true;      /* 已给定 id, 跳过搜索直接播 */
            ESP_LOGI(TAG, "网页点播第 %d 首: %s - %s", i + 1,
                     mus.res[i].name, mus.res[i].artist);
            return true;
        }
    }

    /* 不在当前列表里: 退化成单曲播放(列表被替换成这一首) */
    mus.res_cnt = 1;
    mus.idx     = 0;
    snprintf(mus.res[0].id, sizeof(mus.res[0].id), "%s", song_id);
    snprintf(mus.res[0].name, sizeof(mus.res[0].name), "(指定 id)");
    mus.res[0].artist[0] = '\0';
    mus.running     = true;
    mus.skip_search = true;
    return true;
}

/* ================= 网页专用: 只搜索不出声 / 导出歌单 ================= */

/* 只搜索、不播放。搜索结果留在 mus.res[] 里, 网页再从 music_results_json 取。
   为什么不做成同步搜索: 搜索要走一次 HTTP(几百毫秒), 放在 httpd 任务里会
   把整个网页卡住, 而且 cJSON 解析对 4KB 的 httpd 栈有风险。
   所以这里只是"投递请求", 真正的搜索仍然在音乐任务里做。 */
bool music_search_only(const char *kw)
{
    if (!kw || !kw[0]) return false;

    /* ★★ 必须先把当前这首停掉并等任务真正闲下来 ★★
     *
     * 任务在播放时会一直待在 mus_play_one 的播放循环里, 根本不会回到
     * "开始新会话"那一步。所以只设 pending_kw/s_search_only 的话:
     *   · 搜索请求会被无限期搁置;
     *   · 而 s_search_only 会一直残留, 直到下一次会话才被处理 ——
     *     那时 pending_kw 早被别的请求覆盖, 状态全乱。
     * 实测症状: 网页搜"晴天", 结果播出来的还是上一次"孤勇者"的第 3 首。
     * 做法和 music_play / music_play_id 保持一致: 设 stop_req 再等它停。 */
    if (mus.running) {
        mus.stop_req = true;
        for (int i = 0; i < 60 && mus.running; i++) vTaskDelay(pdMS_TO_TICKS(50));
    }

    snprintf(mus.pending_kw, sizeof(mus.pending_kw), "%s", kw);
    /* ★ 必须显式清掉 skip_search: 上一次 music_play_id 点播留下的 true 会让
       任务"跳过搜索直接播旧歌单", 搜索就静悄悄地不生效了。 */
    mus.skip_search = false;
    s_search_only   = true;
    mus.running     = true;          /* 叫醒音乐任务去做这次搜索 */
    return true;
}

bool music_search_pending(void) { return s_search_only; }

/* 把当前歌单(搜索结果)导成 JSON 给网页。
   歌名/歌手里的双引号会破坏 JSON, 统一换成单引号。 */
static void json_safe(const char *src, char *dst, size_t n)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < n; p++) {
        char c = *p;
        if (c == '"' || c == '\\') c = '\'';
        if ((unsigned char)c < 0x20) c = ' ';
        dst[o++] = c;
    }
    dst[o] = '\0';
}

/* ---- 我的歌单 ---- */
void music_playlists_request(void)
{
    s_pl_ready = false;
    s_pl_req   = true;      /* 音乐任务空闲时会处理 */
}
void music_playlist_songs_request(const char *playlist_id)
{
    if (!playlist_id || !playlist_id[0]) return;
    snprintf(s_pl_want_id, sizeof(s_pl_want_id), "%s", playlist_id);
    s_pls_ready = false;
    s_pls_req   = true;
}
bool music_playlists_ready(void)  { return s_pl_ready; }
bool music_playlist_ready(void)   { return s_pls_ready; }

int music_playlists_json(char *out, size_t n)
{
    int o = snprintf(out, n, "{\"ready\":%s,\"count\":%d,\"items\":[",
                     s_pl_ready ? "true" : "false", s_pl_cnt);
    if (o < 0 || (size_t)o >= n) return 0;
    for (int i = 0; i < s_pl_cnt; i++) {
        char nm[MUS_NAME_LEN];
        json_safe(s_pls[i].name, nm, sizeof(nm));
        int w = snprintf(out + o, n - (size_t)o,
                         "%s{\"id\":\"%s\",\"name\":\"%s\",\"count\":%d}",
                         i ? "," : "", s_pls[i].id, nm, s_pls[i].count);
        if (w < 0 || (size_t)(o + w) >= n - 4) break;
        o += w;
    }
    o += snprintf(out + o, n - (size_t)o, "]}");
    return o;
}

int music_results_json(char *out, size_t n)
{
    int o = snprintf(out, n,
                     "{\"count\":%d,\"idx\":%d,\"playing\":%s,\"busy\":%s,\"src\":\"%s\",\"items\":[",
                     mus.res_cnt, mus.idx,
                     music_is_playing() ? "true" : "false",
                     mus.running ? "true" : "false",
                     s_pl_cur_name[0] ? s_pl_cur_name : (mus.pending_kw[0] ? mus.pending_kw : ""));
    if (o < 0 || (size_t)o >= n) return 0;

    for (int i = 0; i < mus.res_cnt; i++) {
        char nm[MUS_NAME_LEN], ar[MUS_NAME_LEN];
        json_safe(mus.res[i].name,   nm, sizeof(nm));
        json_safe(mus.res[i].artist, ar, sizeof(ar));
        int w = snprintf(out + o, n - (size_t)o,
                         "%s{\"id\":\"%s\",\"name\":\"%s\",\"artist\":\"%s\"}",
                         i ? "," : "", mus.res[i].id, nm, ar);
        if (w < 0 || (size_t)(o + w) >= n - 4) break;   /* 放不下就截断 */
        o += w;
    }
    o += snprintf(out + o, n - (size_t)o, "]}");
    return o;
}

void music_pause(void)  { if (mus.running) mus.pause_req = true; }
void music_resume(void) { mus.pause_req = false; }
void music_stop(void)   { mus.stop_req = true; mus.pause_req = false; }
void music_next(void)
{
    if (!mus.running) return;
    mus.idx++;              /* 跳过当前这首 */
    mus.stop_req = true;    /* 打断当前播放, 任务会从新的 idx 继续 */
}

bool music_is_active(void)  { return mus.running; }
bool music_is_playing(void) { return mus.state == MUS_PLAYING; }

const char *music_status_str(void)
{
    static char buf[288];
    const char *st = "空闲";
    switch (mus.state) {
    case MUS_SEARCHING: st = "搜索中"; break;
    case MUS_PLAYING:   st = "播放中"; break;
    case MUS_PAUSED:    st = "已暂停"; break;
    case MUS_SKIP:      st = "跳过(无版权/VIP)"; break;
    case MUS_ERROR:     st = "出错";   break;
    default:            st = "空闲";   break;
    }
    if ((mus.state == MUS_ERROR || mus.state == MUS_SKIP) && mus.err[0])
        snprintf(buf, sizeof(buf), "%s: %.160s", st, mus.err);
    else if (!mus.base[0])
        snprintf(buf, sizeof(buf), "未设置代理地址(网页「音乐」页填入)");
    else if (mus.idx < mus.res_cnt && mus.res[mus.idx].name[0])
        snprintf(buf, sizeof(buf), "%s: %s - %s", st,
                 mus.res[mus.idx].name, mus.res[mus.idx].artist);
    else
        snprintf(buf, sizeof(buf), "%s", st);
    return buf;
}

void music_set_base(const char *url)
{
    if (!url || !url[0]) return;
    snprintf(mus.base, sizeof(mus.base), "%s", url);
    nvs_handle_t h;
    if (nvs_open("music", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "base", mus.base);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "代理地址 -> %s", mus.base);
}

const char *music_get_base(void) { return mus.base; }
