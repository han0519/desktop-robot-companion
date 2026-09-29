/*
 * xiaozhi.c — 小智 AI 客户端 (官方 WebSocket 协议)
 *
 *  开机: HTTPS POST /xiaozhi/ota/  -> 取激活码 + websocket url/token
 *  连上: 发 hello, 之后按住说话 -> Opus 二进制帧双向流
 *  收到: llm.emotion -> 驱动 OLED 表情
 */
#include "xiaozhi.h"
#include "config.h"
#include "face.h"
#include "persona.h"
#include "ne_client.h"    /* ne_cookie_save_pending/now: 登录态落盘(本任务栈在内部 RAM) */
#include "mic_inmp441.h"
#include "speaker_max98357a.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/queue.h"
#include "opus.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "esp_timer.h"
#include "wake_word.h"
#include "esp_random.h"
#include "light.h"
#include "mcp.h"
#include "music.h"

static const char *TAG = "xiaozhi";

/* ======================= 参数 ======================= */
#define OTA_URL_DEFAULT   "https://api.tenclass.net/xiaozhi/ota/"
#define WS_URL_DEFAULT    ""                 /* 由 OTA 下发, 也可用 ai_client_set_server 覆盖 */
#define SAMPLE_RATE       16000
#define FRAME_MS          60
#define FRAME_SAMPLES     (SAMPLE_RATE / 1000 * FRAME_MS)   /* 960 (上行/麦克风) */

/* ---- 下行播放用 24kHz ----
 * 服务端下发的 Opus 是 24000Hz。解码器也建在 24000, 一帧 60ms 出 1440 个样本,
 * 正好对上喇叭 I2S 的 24kHz —— 中间不做任何重采样。
 * 若按 16kHz 解码, Opus 内部要把 24k 降到 16k, 高频上限只剩 8kHz,
 * 听感就是"糊/闷"。这也是喵伴(MIAOBAN)用 24kHz 输出的原因。 */
#define RX_SAMPLE_RATE    24000
#define RX_FRAME_SAMPLES  (RX_SAMPLE_RATE / 1000 * FRAME_MS)  /* 1440 */
#define OPUS_BITRATE      24000
#define OPUS_MAX_PKT      1024
#define RX_BUF_SIZE       4096

/* 免触摸对话(VAD)的可调参数集中在 components/board/include/config.h:
 * VAD_START_FRAMES / VAD_STOP_MS / VAD_MARGIN / VAD_MIN_RMS / VAD_COOLDOWN_MS */

/* ---- 下行抖动缓冲 (治"一卡一卡"的核心) ----
 * 官方 2.2.4 的 audio_service.h 里 MAX_DECODE_PACKETS_IN_QUEUE = 2400/60 = 40 包
 * (约 2.4 秒), 网络抖动全靠这个队列吸收。原来我们只有 8192 字节 ≈ 40 帧,
 * 而且是"收到一包就立刻解码+立刻写 I2S" —— 网络一抖缓冲就见底,
 * I2S 没数据可播就静音, 听感就是一句一顿地卡。
 * 这里: 队列放大到 ≈60 帧, 并且开播前先攒够 PREBUFFER 再播。 */
#define AUDIO_SB_SIZE     12288
/* 开播前先攒这么多字节再开始播(约 8 帧 ≈ 480ms)。
   攒够后播放速度(60ms/帧)会略慢于服务端的下发速度, 缓冲只会越来越深,
   之后即使网络突发抖动也不会断音。 */
/* 实测服务端下发偏慢, 攒到 2048 往往要等到超时(1.2 秒), 开播延迟明显。
   改成 ~5 帧(300ms 音频)就开播, 之后缓冲会随下发自然变深。 */
#define PREBUFFER_BYTES   1024
/* 攒不够也不能一直等 —— 超时就先播, 否则短句会明显延迟 */
#define PREBUFFER_TIMEOUT_MS 700

/* 服务端连续这么久没下发任何数据 → 认为连接已"假死", 主动重建。
   照搬官方 Protocol::IsTimeout() (protocol.cc:81-90, 120 秒)。
   官方没有 keep_alive / ping, 唯一的自愈手段就是这个超时重开。 */
#define RX_TIMEOUT_MS     120000
/* 空闲时水位有 22KB(free=22540/24576), 但真正播报 TTS 时
 * opus_decode + speaker_write 会吃掉 6~8KB —— 砍到 16KB 实测直接
 * "A stack overflow in task zx_audio", 所以必须保留 24KB */
#define AUDIO_TASK_STACK  28672
/* WebSocket 客户端任务的栈 —— 它要跑 wss:// 的 mbedTLS 握手,
 * 8KB 会栈溢出(实测触发 vApplicationStackOverflowHook 崩溃重启,
 * 并伴随 ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED). 24KB 是安全值.
 * 注意: 这是内部 RAM 里最大的一块连续分配, 必须给足总内存才建得出来 ——
 * 否则 esp_websocket_client 会报 "Error create websocket task" 而永不连接 */
#define ZX_TASK_STACK     16384
/* WebSocket 任务的栈。原来给 24KB 是怕 mbedTLS 握手爆栈, 但我们已经把
 * TLS 缓冲区挪到 PSRAM(CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC + DYNAMIC_BUFFER),
 * 握手对栈的需求大幅下降 —— 官方固件甚至不设这个值(用默认)。
 * ★ 10KB 而不是 16KB: 实测这个任务只用了约 3KB(free=13276/16384), 但内部 RAM
 *   碎片化后"最大连续块"常常只剩 12KB 左右, 16KB 的任务栈会直接创建失败
 *   ("Error create websocket task" → 永远连不上)。10240 既能容纳 TLS, 又容易分配成功。 */
/* ★ 这个值必须小到"即使跑久了内存碎片化也还能分配出连续块"。
   实测 WS 任务只用到约 3KB(free=13276 时栈是 16384), 而内部 RAM 的最大
   连续块会从启动时的 ~10.7KB 逐渐降到 ~7.6KB —— 8KB 的栈就会创建失败
   ("Error create websocket task" → 永远连不上)。6144 留了约 2 倍余量。 */
#define WS_TASK_STACK     5120

/* 连续对话: 一直没人说话多久后退出, 回到"喊唤醒词"模式。
   注意: 这台服务端回答完会立刻关闭会话, 所以这个窗口内如果没人说话,
   设备会停止聆听并等待重连 —— 重连后由 want_listen 自动重新开启。 */
#define CONT_IDLE_MS      12000
/* 音频任务一轮的耗时。必须等于 FRAME_MS: 每轮恰好消费一帧 60ms 的音频,
   循环周期也只有 60ms 才跟得上, 否则会丢样本(详见循环末尾的说明) */
#define AUDIO_LOOP_MS     FRAME_MS
/* 上行麦克风增益(倍数)。实测原始电平偏低导致识别率低, 模型只能"呃…"。
   INMP441 输出本身不大, 这里做数字增益并做饱和削波, 避免溢出破音。 */
#define TX_GAIN              3.0f

/* 播报态里连续没有新音频多久就强制收尾(兜底: 万一 tts stop 丢了,
   不至于永远卡在"说话中"导致麦克风不再被读取)。
   必须给足 —— 服务端是"一句一句"合成下发的, 句子之间实测会停顿 4 秒多,
   阈值太小会把正常的停顿误判成卡死, 反而把后面的音频冲掉。 */
#define SPEAK_IDLE_MS     8000

/* ======================= 状态 ======================= */
typedef enum { MODE_IDLE = 0, MODE_LISTEN, MODE_SPEAK } zx_mode_t;

static struct {
    bool        online;         /* STA 联网 */
    bool        ws_ready;       /* WebSocket 已建立 + hello 完成 */
    zx_mode_t   mode;
    char        session[64];
    char        device_id[20];
    char        client_id[40];
    char        ws_url[256];
    char        ws_token[256];
    char        ota_url[192];
    char        act_code[16];
    char        state[24];
    esp_websocket_client_handle_t ws;
    StreamBufferHandle_t sb;    /* 收到的 Opus 帧排队给音频任务 */
    QueueHandle_t txq;          /* 待发文本消息队列(详见 zx_enqueue_text 的说明) */
    TaskHandle_t audio_task;
    uint32_t    tx, rx;
    /* --- 唤醒词 --- */
    bool        server_hello;   /* 服务端 hello 已收到(协议握手完成) */
    uint32_t    ws_connect_ms;  /* WebSocket 建立时刻 */
    bool        ww_ready;       /* esp-sr 唤醒词引擎可用 */
    bool        wake_beep;      /* 唤醒时是否播应答音(排查音频问题时可关) */
    /* --- 音乐互斥: 播音乐时挂起整条 AI 音频链(详见 ai_client_set_audio_suspended) --- */
    volatile bool audio_suspended;
    int         suspend_log;    /* 挂起期间只打一次日志 */
    volatile bool ww_restart_req;  /* 请求重建唤醒词引擎(必须由音频任务执行, 见下) */
    volatile bool sb_flush_req;    /* 请求清空下行音频缓冲(必须由音频任务做:
                                      xStreamBufferReset 只允许在读者上下文调,
                                      别的任务 reset 会和 WS 写者竞态, 且读者
                                      正阻塞在 Receive 时 reset 会静默失败) */
    volatile bool mic_drain_req;   /* 请求丢弃麦克风积压(同理: 麦克风只能由
                                      音频任务读, 双消费者会互相挖洞) */
    /* --- VAD --- */
    bool        vad_on;
    bool        vad_session;    /* 当前这轮对话是 VAD 自动开始的 */
    float       mic_rms;        /* 最近一帧的麦克风能量(给网页标定用) */
    float       noise;          /* 自适应噪声底 */
    float       vad_thr;        /* 当前触发阈值 */
    int         voice_frames;
    int         silence_ms;
    int         cooldown_ms;
    int         vad_log_cnt;
    int         stuck;          /* WS 连续未连上的次数(看门狗式重建用) */
    /* --- 重连策略 ---
       force_reconnect = 软性: 服务端关了会话, 交给 esp_websocket_client 自带的
                         自动重连(复用同一个任务, 不需要新内存), 我们不重建。
       hard_reconnect  = 硬性: 连接"假死"(120 秒收不到任何数据), 自动重连救不了,
                         必须彻底销毁重建。 */
    volatile bool hard_reconnect;
    /* 播音乐期间把"重建 WebSocket"这件事挂起, 等音乐结束再做。
       实测: 播音乐时内部 RAM 只剩 7.4KB, 重建直接 "Error create websocket task";
       而每次失败的尝试都要分配/释放 8KB WS 缓冲 + TLS 上下文, 内存来回搅动,
       把音乐那条 TCP 也拖死(日志: 178482 重建失败 → 182742 音乐缓冲掉到 0
       → 184872 判"流断了")。而且播音乐时 AI 音频本来就挂起, 有连接也没用。 */
    volatile bool pending_rebuild;
    /* ★ 音乐模式: 播歌期间 AI 整个让路(音频链挂起 + WebSocket 关闭释放内存),
       停歌或摸头顶三下暂停后自动恢复。见 ai 任务里的 music_mode 分支。 */
    volatile bool music_mode;
    /* --- 播报收尾 --- */
    bool        tts_draining;   /* 服务端已说"发完", 等缓冲放空再真正结束 */
    uint32_t    tts_start_ms;   /* 本轮播报开始时刻(用于抖动缓冲攒帧计时) */
    int         speak_idle_ms;  /* 播报态里连续没有新音频的时长(兜底防卡死) */
    int         dec_fail;       /* 连续解码失败次数(用于重新同步) */
    uint32_t    last_rx_ms;     /* 最后一次收到服务端数据(文本或音频)的时刻 */
    /* --- 播报期间的情绪延后展示 ---
       llm 消息几乎和 tts start 同时到达, 若当场换脸就会把「说话」表情顶掉,
       用户看到的就是"说话表情只闪了不到 1 秒"。先记下来, 播完再展示。 */
    char        pending_emo[24];
    uint32_t    emo_show_until_ms;  /* 情绪展示截止时刻(此期间不切"聆听"脸) */
    /* --- 连续对话 --- */
    bool        want_listen;    /* 想开始聆听但 WS 没就绪 → 连上后自动补上 */
    bool        force_reconnect;/* 服务端关了会话 → 下一拍立即重建(不等超时) */
    bool        cont_mode;      /* 连续对话: 一轮答完自动重新聆听, 不用再喊唤醒词 */
    int         cont_wait_ms;   /* 播报结束后等待多久自动重开聆听 */
    int         cont_idle_ms;   /* 连续对话里"一直没人说话"的累计时长 */
    bool        heard_voice;    /* 本轮是否听到过人声 */
    bool        selftested;     /* 开机自检是否已跑过 */
} zx;

/* 自检只在"烧录后第一次开机"跑一次(标志存 NVS), 之后永久跳过。
   放在 zx 任务里跑(它有 20KB 栈), 不另建任务 —— 内部 RAM 已经不够建了。 */
static bool zx_selftest_should_run(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("stest", NVS_READWRITE, &h) != ESP_OK) return false;
    if (nvs_get_u8(h, "doneB", &v) == ESP_OK && v) { nvs_close(h); return false; }
    v = 1;
    nvs_set_u8(h, "doneB", v);
    nvs_commit(h);
    nvs_close(h);
    return true;
}

/* 一帧 PCM 的均方根 (int16 满量程 32767) */
static float rms_of(const int16_t *s, size_t n)
{
    int64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = s[i];
        acc += (int64_t)v * (int64_t)v;
    }
    if (n == 0) return 0.0f;
    return sqrtf((float)acc / (float)n);
}

/* ======================= 小工具 ======================= */
static size_t nvs_str_get(const char *key, char *out, size_t max)
{
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = max;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = '\0';
    nvs_close(h);
    return strlen(out);
}

static void nvs_str_set(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static void set_state(const char *s)
{
    strncpy(zx.state, s, sizeof(zx.state) - 1);
    zx.state[sizeof(zx.state) - 1] = '\0';
    ESP_LOGI(TAG, "state -> %s", s);
}

/* 小智服务端的 emotion 名 -> 我们的表情 ID */
static const char *map_emotion(const char *e)
{
    static const struct { const char *src, *dst; } M[] = {
        {"neutral",     "NORMAL"},
        {"happy",       "HAPPY"},
        {"laughing",    "LAUGH"},
        {"funny",       "SILLY"},
        {"sad",         "SAD"},
        {"angry",       "ANGRY"},
        {"crying",      "CRY"},
        {"loving",      "LOVE"},
        {"embarrassed", "SHY"},
        {"surprised",   "SURPRISED"},
        {"shocked",     "SHOCK"},
        {"thinking",    "THINKING"},
        {"winking",     "WINK"},
        {"cool",        "COOL"},
        {"relaxed",     "RELIEVED"},
        {"delicious",   "HUNGRY"},
        {"kissy",       "KISS"},
        {"kiss",        "KISS"},
        {"confident",   "SMUG"},
        {"sleepy",      "SLEEPY"},
        {"confused",    "CONFUSED"},
        {"silly",       "SILLY"},
    };
    for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); i++)
        if (strcasecmp(e, M[i].src) == 0) return M[i].dst;
    for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); i++)
        if (strcasecmp(e, M[i].dst) == 0) return M[i].dst;
    return NULL;
}

/* ======================= 设备身份 ======================= */
static void build_identity(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    /* 关键: 服务端要求 MAC 是「小写 + 冒号分隔」.
       大写、或不带冒号, 都会直接返回 400 {"error":"Invalid MAC address"} */
    snprintf(zx.device_id, sizeof(zx.device_id), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (nvs_str_get("client_id", zx.client_id, sizeof(zx.client_id)) == 0) {
        /* 生成一个稳定的 UUID 并存起来 */
        uint32_t r = esp_random();
        snprintf(zx.client_id, sizeof(zx.client_id),
                 "%02x%02x%02x%02x-%02x%02x-4%01x%02x-a%01x%02x-%02x%02x%02x%02x%02x%02x",
                 (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2], (unsigned)(r & 0xffu),
                 (unsigned)((r >> 8) & 0xffu), (unsigned)((r >> 16) & 0xffu),
                 (unsigned)((r >> 24) & 0xfu), (unsigned)(mac[3] & 0xffu),
                 (unsigned)(mac[4] & 0xfu), (unsigned)(mac[5] & 0xffu),
                 (unsigned)mac[0], (unsigned)mac[1], (unsigned)mac[2], (unsigned)mac[3],
                 (unsigned)mac[4], (unsigned)mac[5]);
        nvs_str_set("client_id", zx.client_id);
    }
    ESP_LOGI(TAG, "device-id=%s client-id=%s", zx.device_id, zx.client_id);
}

/* ======================= OTA / 激活 ======================= */
#define OTA_RESP_MAX 3072
static char  s_ota_resp[OTA_RESP_MAX];      /* 必须是实体数组: 之前写成 NULL 指针, 回调里判断失败, 响应体全丢了 */
static int   s_ota_len = 0;

static esp_err_t ota_http_event(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_DATA) {
        if (s_ota_len + ev->data_len < OTA_RESP_MAX) {
            memcpy(s_ota_resp + s_ota_len, ev->data, ev->data_len);
            s_ota_len += ev->data_len;
            s_ota_resp[s_ota_len] = '\0';
        }
    }
    return ESP_OK;
}

/* 返回 true = 拿到可用的 websocket 配置 */
static bool zx_ota_request(void)
{
    char body[512];
    snprintf(body, sizeof(body),
             "{\"version\":2,\"language\":\"zh-CN\",\"flash_size\":16384,"
             "\"mac_address\":\"%s\",\"uuid\":\"%s\",\"chip_model_name\":\"esp32s3\","
             "\"application\":{\"name\":\"robot_companion\",\"version\":\"1.0.0\"},"
             "\"board\":{\"type\":\"robot-companion\",\"name\":\"robot-companion\","
             "\"mac\":\"%s\"},"
             "\"ota\":{\"label\":\"robot_companion\"}}",
             zx.device_id, zx.client_id, zx.device_id);

    s_ota_len = 0;
    esp_http_client_config_t cfg = {
        .url = zx.ota_url[0] ? zx.ota_url : OTA_URL_DEFAULT,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 12000,
        .event_handler = ota_http_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return false;
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_header(h, "Device-Id", zx.device_id);
    esp_http_client_set_header(h, "Client-Id", zx.client_id);
    esp_http_client_set_header(h, "Activation-Version", "1");
    esp_http_client_set_post_field(h, body, strlen(body));

    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA 请求失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "OTA HTTP %d, %d bytes", status, s_ota_len);
    if (s_ota_len <= 0) return false;

    cJSON *root = cJSON_Parse(s_ota_resp);
    if (!root) { ESP_LOGW(TAG, "OTA 响应不是合法 JSON"); return false; }

    bool ok = false;
    cJSON *ws = cJSON_GetObjectItem(root, "websocket");
    if (ws) {
        cJSON *u = cJSON_GetObjectItem(ws, "url");
        cJSON *t = cJSON_GetObjectItem(ws, "token");
        if (u && cJSON_IsString(u) && u->valuestring[0]) {
            strncpy(zx.ws_url, u->valuestring, sizeof(zx.ws_url) - 1);
            nvs_str_set("ws_url", zx.ws_url);
        }
        if (t && cJSON_IsString(t)) {
            strncpy(zx.ws_token, t->valuestring, sizeof(zx.ws_token) - 1);
            nvs_str_set("ws_token", zx.ws_token);
        }
    }
    cJSON *act = cJSON_GetObjectItem(root, "activation");
    if (act && cJSON_IsObject(act)) {
        cJSON *code = cJSON_GetObjectItem(act, "code");
        cJSON *msg = cJSON_GetObjectItem(act, "message");
        if (code && cJSON_IsString(code)) {
            strncpy(zx.act_code, code->valuestring, sizeof(zx.act_code) - 1);
            ESP_LOGW(TAG, "==================================================");
            ESP_LOGW(TAG, " 设备未激活! 去 xiaozhi.me 添加设备, 验证码: %s", zx.act_code);
            if (msg && cJSON_IsString(msg)) ESP_LOGW(TAG, " 服务端提示: %s", msg->valuestring);
            ESP_LOGW(TAG, "==================================================");
        }
    }
    if (zx.ws_url[0]) ok = true;
    cJSON_Delete(root);
    return ok;
}

/* ======================= WebSocket ======================= */
/* 文本消息一律先入队, 由 zx_task 统一发出去。
 *
 * ★ 为什么不能直接在这里 send_text:
 *   mbedTLS 的 SSL 上下文不允许「一边读一边写」。而所有 JSON 处理
 *   (zx_handle_json) 都是在 WS 接收回调里跑的 —— 那一刻客户端正卡在
 *   mbedtls_ssl_read 里, 我们若直接 send_text, 就是并发写同一个 SSL
 *   上下文。实测 2065 字节的 MCP tools/list 回复会直接把连接写崩:
 *       esp_transport_write() returned -1
 *       transport_error=ESP_ERR_MBEDTLS_SSL_WRITE_FAILED, errno=119
 *   然后每 5 秒重连一次、对话反复掉线。
 *
 * 官方 2.2.4 也是这么处理的 —— MCP 回复必须 Schedule() 到主任务再发
 * (application.cc:1069-1076), 不在网络回调里直接写。 */
static bool zx_enqueue_text(const char *s)
{
    if (!zx.txq || !s || !s[0]) return false;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) return false;
    memcpy(p, s, n);
    if (xQueueSend(zx.txq, &p, 0) != pdTRUE) { free(p); return false; }
    return true;
}

static void zx_send_json(cJSON *obj)
{
    if (!zx.ws || !zx.ws_ready || obj == NULL) {
        if (obj) cJSON_Delete(obj);
        return;
    }
    char *s = cJSON_PrintUnformatted(obj);
    if (s) {
        if (!zx_enqueue_text(s)) ESP_LOGW(TAG, "发送队列满, 丢弃消息");
        free(s);
    }
    cJSON_Delete(obj);
}

static void zx_send_hello(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "hello");
    cJSON_AddNumberToObject(o, "version", 1);
    cJSON_AddStringToObject(o, "transport", "websocket");
    cJSON *ap = cJSON_AddObjectToObject(o, "audio_params");
    cJSON_AddStringToObject(ap, "format", "opus");
    cJSON_AddNumberToObject(ap, "sample_rate", SAMPLE_RATE);
    cJSON_AddNumberToObject(ap, "channels", 1);
    cJSON_AddNumberToObject(ap, "frame_duration", FRAME_MS);
    /* ★ 声明支持 MCP —— 服务端只有看到这个标志, 才会把工具调用(mcp 消息)下发下来,
       AI 才能"自己"开灯/调亮度/读温湿度。没有这一行服务端永远不发 mcp。 */
    cJSON *ft = cJSON_AddObjectToObject(o, "features");
    cJSON_AddBoolToObject(ft, "mcp", true);
    zx_send_json(o);
}

static void zx_send_listen(const char *state)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "listen");
    cJSON_AddStringToObject(o, "state", state);
    /* 官方默认用 "auto": 由服务端做 VAD 判断"说完了", 本地再补一发 stop 也无害。
       "manual" 要求设备自己判断结束, 服务端行为不同, 实测 auto 更稳。 */
    if (strcmp(state, "start") == 0) cJSON_AddStringToObject(o, "mode", "auto");
    if (zx.session[0]) cJSON_AddStringToObject(o, "session_id", zx.session);
    zx_send_json(o);
}

/* 唤醒词命中时先告知服务端 (官方 Protocol::SendWakeWordDetected 的做法) */
static void zx_send_wake_detected(const char *word)
{
    if (!zx.ws_ready) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "listen");
    cJSON_AddStringToObject(o, "state", "detect");
    cJSON_AddStringToObject(o, "text", (word && word[0]) ? word : "你好小智");
    if (zx.session[0]) cJSON_AddStringToObject(o, "session_id", zx.session);
    zx_send_json(o);
}

/* ★ 统一的"强制回 IDLE"(异常收尾路径专用: WS 断开 / abort / 音乐挂起)。
 * 这些路径以前只把 mode 拍成 IDLE, 有两个真实踩过的坑:
 *   ① speaker_set_hold(true) 是在 tts start 钉住的, 唯一正常释放点是
 *      zx_finish_speaking(); 异常路径绕过它 → 功放永久钉死, 静态底噪一直响。
 *   ② xStreamBufferReset 只允许在【读者】(音频任务)上下文调 —— 别的任务
 *      reset 会与 WS 写者竞态, 且音频任务正阻塞在 Receive 时 reset 静默失败。
 * 所以这里只放功放 + 拍状态, 清缓冲改成挂标志交给音频任务。 */
static void zx_force_idle(void)
{
    speaker_set_hold(false);
    zx.mode          = MODE_IDLE;
    zx.tts_draining  = false;
    zx.tts_start_ms  = 0;
    zx.speak_idle_ms = 0;
    zx.vad_session   = false;
    zx.sb_flush_req  = true;
}

/* 丢弃麦克风里积压的旧样本(应答音期间的"叮咚"等)。
 * ★ 只能在音频任务里调: 音频任务正阻塞在 mic_read 上, 从别的任务再读
 * 同一个 I2S 就是双消费者, 会把音频任务的帧挖出洞 —— 唤醒词的前提是
 * "一个样本都不能丢"。 */
static void mic_drain_backlog(void)
{
    static int16_t junk[256];
    for (int i = 0; i < 12; i++) {
        if (mic_read(junk, 256, 0) < 128) break;
    }
}

/* 一轮播报"真正"结束: 回 IDLE / 冷却 / 连续对话重新聆听。
   必须在缓冲放空之后才调用 —— 提前调用会把没播完的帧丢掉。 */
static void zx_finish_speaking(void)
{
    /* ★ 放开功放(播报期间是钉住的)。放在最前面且不判 mode ——
       否则某条异常收尾路径会让功放一直钉着, 静态底噪一直响。 */
    speaker_set_hold(false);
    if (zx.mode != MODE_SPEAK) return;
    zx.mode = MODE_IDLE;
    zx.tts_draining  = false;
    zx.tts_start_ms  = 0;
    zx.speak_idle_ms = 0;
    zx.vad_session   = false;
    zx.cooldown_ms   = VAD_COOLDOWN_MS;
    if (zx.sb) xStreamBufferReset(zx.sb);      /* 清掉可能错位的残留字节 */
    set_state(zx.cont_mode ? "连续对话" : "就绪");
    persona_event_dialog_end();       /* 人格: 完整聊了一轮, 心情略好 */

    /* ★ 本轮播报结束: 把播报期间攒下的 AI 情绪展示几秒。
       必须在这里做, 并且记录截止时刻 —— 否则 0.5 秒后 zx_begin_listen()
       就把脸换成"聆听", 情绪根本来不及看清。 */
    if (zx.pending_emo[0]) {
        const char *id = map_emotion(zx.pending_emo);
        ESP_LOGI(TAG, "播报结束 → 展示情绪「%s」", zx.pending_emo);
        if (id && face_set_emotion_by_name(id)) {
            face_set_emotion_hold(face_get_emotion(), 4000);
            zx.emo_show_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 4000;
        } else if (!zx.cont_mode) {
            face_set_emotion_hold(FACE_NORMAL, 3000);
        }
        zx.pending_emo[0] = '\0';
    } else if (!zx.cont_mode) {
        face_set_emotion_hold(FACE_NORMAL, 3000);
    }

    if (zx.cont_mode) {
        /* ★ 连续对话: 不等唤醒词, 稍候自动重新进入聆听。
           两条路一起上(双保险):
             · 音频任务里的 cont_wait_ms 倒计时 → 约 0.5 秒就重开(主路)
             · zx 任务里的 want_listen 兜底    → 最迟 5 秒内一定重开
           实测只靠主路时偶尔不触发, 用户就又得喊一遍唤醒词。 */
        zx.cont_wait_ms = 500;
        zx.want_listen  = true;
        zx.heard_voice  = false;
        zx.cont_idle_ms = 0;
    } else {
        face_set_emotion_hold(FACE_NORMAL, 3000);
    }
}

/* 开始一轮对话 (触摸触发与 VAD 触发共用) */
static size_t txlen;              /* 定义在音频任务区(与 txacc 一起), 这里前置声明 */
static void zx_begin_listen(void)
{
    /* 协议要求: 设备发 hello → 服务端回 hello(带 session_id) → 之后才能发 listen。
       这里做个门禁, 免得握手没完成就发音频被服务端忽略。
       留 3 秒兜底: 万一服务端不回 hello(例如未激活), 也不至于完全不能用。 */
    if (!zx.server_hello) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (zx.ws_connect_ms && (now - zx.ws_connect_ms) < 3000) {
            ESP_LOGW(TAG, "服务端还没回 hello, 等握手完成再开口");
            return;
        }
        ESP_LOGW(TAG, "服务端始终没回 hello —— 多半是设备未激活(去 xiaozhi.me 添加)");
    }

    zx.mode = MODE_LISTEN;
    zx.silence_ms = 0;
    if (xTaskGetCurrentTaskHandle() == zx.audio_task) {
        /* 音频任务自己调用(唤醒命中/VAD 触发的主路): 直接清 */
        mic_drain_backlog();
        txlen = 0;                    /* 上一轮没凑满的半帧旧音频, 别拼进这一轮 */
        if (zx.sb) xStreamBufferReset(zx.sb);
    } else {
        /* 从别的任务调用(zx 任务的 want_listen 兜底/自检): 绝不能在这里
           读麦克风或清 sb —— 双消费者会互相挖洞。挂标志, 音频任务下一拍做。 */
        zx.mic_drain_req = true;
        zx.sb_flush_req  = true;
    }
    zx_send_listen("start");
    zx.heard_voice = false;
    zx.cont_idle_ms = 0;
    zx.cont_wait_ms = 0;
    /* ★ 开始对话就把音乐暂停: 音乐和 AI 播报共用同一个喇叭,
       不暂停会互相插话。暂停(而不是停掉)保留流, 用户说"继续播放"能接上。 */
    if (music_is_active() && music_is_playing()) {
        music_pause();
        ESP_LOGI(TAG, "开始对话 → 暂停音乐");
    }

    set_state("在听");
    persona_event_dialog_start();     /* 人格: 有人陪我说话, 不无聊了 */
    /* ★ 若刚播完、AI 的情绪还在展示期内, 就先别换成"聆听"脸 ——
       否则情绪只亮 0.5 秒就被顶掉, 等于没展示。 */
    if ((uint32_t)(esp_timer_get_time() / 1000) < zx.emo_show_until_ms) {
        ESP_LOGI(TAG, "AI 情绪展示中 → 暂不切换聆听表情");
    } else {
        face_set_emotion_hold(FACE_LISTENING, 60000);
    }
}

static void zx_handle_json(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "JSON 解析失败: %.*s", len > 80 ? 80 : len, json);
        return;
    }
    cJSON *t = cJSON_GetObjectItem(root, "type");
    const char *type = (t && cJSON_IsString(t)) ? t->valuestring : "?";
    /* 用 ASCII 打一条, 方便串口里直接看到服务端到底回了什么 */
    ESP_LOGI(TAG, "RX json: type=%s len=%d", type, len);

    if (strcmp(type, "hello") == 0) {
        cJSON *sid = cJSON_GetObjectItem(root, "session_id");
        if (sid && cJSON_IsString(sid)) {
            strncpy(zx.session, sid->valuestring, sizeof(zx.session) - 1);
        }
        zx.ws_ready = true;
        zx.server_hello = true;     /* 握手完成, 可以发 listen 了 */
        set_state("就绪");
        /* 把服务端 hello 原样打出来: 里面有 audio_params(采样率/帧长), 出问题一眼可见 */
        ESP_LOGI(TAG, "服务端 hello: %.*s", len > 220 ? 220 : len, json);
        face_set_emotion_hold(FACE_HELLO, 1500);

    } else if (strcmp(type, "stt") == 0) {
        cJSON *tx = cJSON_GetObjectItem(root, "text");
        if (tx && cJSON_IsString(tx)) ESP_LOGI(TAG, "我听到: %s", tx->valuestring);

    } else if (strcmp(type, "llm") == 0) {
        cJSON *tx = cJSON_GetObjectItem(root, "text");
        cJSON *em = cJSON_GetObjectItem(root, "emotion");
        if (tx && cJSON_IsString(tx)) {
            ESP_LOGI(TAG, "小智: %s", tx->valuestring);
            /* ★ 从回复文本里识别"认同/不认同" → 点头/摇头。
               内部带节流(8 秒一次), 且只认强信号词, 不会每句都点头。 */
            persona_scan_reply(tx->valuestring);
        }
        if (em && cJSON_IsString(em) && em->valuestring[0]) {
            /* ★ 播报中不许换脸。
               llm 几乎和 tts start 同时到达, 原来在这里直接 face_set_emotion_hold(),
               于是刚设好的「说话」表情(60 秒 hold)立刻被顶掉 ——
               现象就是"AI 说话表情只闪了几秒就没了, 后面变成别的脸"。
               现在先存起来, 等这轮播报结束(缓冲放空)再展示。 */
            if (zx.mode == MODE_SPEAK) {
                strncpy(zx.pending_emo, em->valuestring, sizeof(zx.pending_emo) - 1);
                zx.pending_emo[sizeof(zx.pending_emo) - 1] = '\0';
                ESP_LOGI(TAG, "播报中 → 情绪「%s」延后到播完再展示", zx.pending_emo);
            } else {
                const char *id = map_emotion(em->valuestring);
                ESP_LOGI(TAG, "表情 <- %s -> %s", em->valuestring, id ? id : "?");
                if (id && face_set_emotion_by_name(id)) {
                    /* 用 hold 锁住 5 秒, 免得被自动轮换立刻冲掉 */
                    face_set_emotion_hold(face_get_emotion(), 5000);
                } else {
                    face_set_emotion_hold(FACE_HAPPY, 3000);
                }
            }
        }

    } else if (strcmp(type, "tts") == 0) {
        cJSON *st = cJSON_GetObjectItem(root, "state");
        const char *s = (st && cJSON_IsString(st)) ? st->valuestring : "";
        if (strcmp(s, "start") == 0) {
            /* 清掉缓冲里的残留字节, 保证从帧头开始解析(否则长度头会错位) */
            if (zx.sb) xStreamBufferReset(zx.sb);
            zx.mode = MODE_SPEAK;
            zx.tts_draining  = false;
            zx.tts_start_ms  = (uint32_t)(esp_timer_get_time() / 1000);
            zx.speak_idle_ms = 0;
            zx.dec_fail      = 0;
            zx.pending_emo[0] = '\0';     /* 新一轮, 清掉上一轮残留的情绪 */
            /* ★★★ 播报期间必须【钉住功放】★★★
             *
             * MAX98357A 的空闲关断判据是"距上次 speaker_write 超过 120ms"。
             * 而 TTS 帧是网络下发+解码的, 帧间偶有 >120ms 的空档, 空闲任务就会
             * 在说话途中把功放关掉、下一帧又打开 —— 结果是咔哒声、断字、发闷,
             * 用户听着就是"声音很糊"(实测日志: 刚解码 3 帧就出现"空闲 → 关断功放")。
             * 播音乐时用 speaker_set_hold(true) 解决过同样的问题, 语音这条路
             * 一直漏了, 现在补上(播报结束在 zx_finish_speaking 里放开)。 */
            speaker_set_hold(true);
            ESP_LOGI(TAG, ">>> TTS 开始播报 (先攒 %d 字节抖动缓冲再开播, 累计rx=%u)",
                     PREBUFFER_BYTES, (unsigned)zx.rx);
            set_state("说话中");
            /* 说话表情: 两眼 + 嘟噜噜电流嘴, hold 给大值, 播报期间一直保持 */
            face_set_emotion_hold(FACE_SPEAKING, 60000);
        } else if (strcmp(s, "stop") == 0) {
            /* ★ 不能立刻切 IDLE! 服务端说的是"我已经发完了", 但缓冲里通常
               还有几帧没播完(realtime 播放 vs 网络突发)。此刻切走的话,
               音频任务会把这些帧当"空闲残留"直接冲掉 —— 回复就只播了半截,
               正是"他回复一小句话就没音了"。改成标记收尾, 放空后再结束。 */
            zx.tts_draining = true;
            ESP_LOGI(TAG, "TTS 发送结束 → 等播完 (缓冲剩 %u 字节)",
                     (unsigned)(zx.sb ? xStreamBufferBytesAvailable(zx.sb) : 0));
            /* 极端情况: 缓冲本来就是空的, 音频任务可能一直读不到头,
               这里顺手补一次收尾判定, 避免卡在"说话中" */
            if (zx.sb && xStreamBufferBytesAvailable(zx.sb) == 0) {
                zx_finish_speaking();
            }
        } else if (strcmp(s, "sentence_start") == 0) {
            cJSON *tx = cJSON_GetObjectItem(root, "text");
            if (tx && cJSON_IsString(tx)) ESP_LOGI(TAG, "播报: %s", tx->valuestring);
        }

    } else if (strcmp(type, "goodbye") == 0) {
        /* ★★ 千万不要在这里清 cont_mode / cont_wait_ms! ★★
         *
         * 正常时序是  tts stop  →  goodbye  →  socket close。
         * 而 tts stop 的处理(见上面)刚刚把 cont_wait_ms=500 / want_listen=true
         * 设好, 准备 0.5 秒后自动重开聆听 —— goodbye 紧接着就把这两个值清零,
         * 连续对话于是在这一瞬间被彻底掐死。后面断线回调里的兜底
         * `if (zx.cont_mode) { want_listen = true; ... }` 也因为 cont_mode 已假
         * 而永远进不去。表现就是: 每轮回答完都得重新喊一次「你好小智」。
         *
         * 正确做法: 只结束"本轮", 保留连续对话意图 ——
         * 排队 want_listen, 并催一下尽快重连, 连上后自动接着听。
         * (参考喵伴 MIAOBAN: 它收到 goodbye 只改界面文案, 不动会话/聆听状态) */
        zx.session[0] = '\0';
        if (zx.cont_mode) {
            zx.want_listen     = true;
            zx.force_reconnect = true;
            ESP_LOGI(TAG, "服务端 goodbye → 保持连续对话: 重连后自动接着聆听");
            set_state("连续对话");
        } else {
            face_set_emotion_hold(FACE_BYE, 2500);
            set_state("就绪");
        }
        /* ★ 不要把 mode 直接拍成 IDLE: 那样音频任务会立刻停止播放,
           服务端刚下发的最后一句就被截断了(听感是"话说一半突然没了")。
           正在播报就只标记收尾, 让音频任务把缓冲放空后自己结束。 */
        if (zx.mode == MODE_SPEAK) {
            zx.tts_draining = true;
        } else {
            zx.mode = MODE_IDLE;
        }

    } else if (strcmp(type, "mcp") == 0) {
        /* ★ AI 的工具调用: payload 是 JSON-RPC 2.0 请求(initialize/tools/list/tools/call),
           交给 mcp.c 执行, 再把 JSON-RPC 响应原样回给服务端。 */
        cJSON *pl = cJSON_GetObjectItem(root, "payload");
        if (pl) {
            char *pls = cJSON_PrintUnformatted(pl);
            if (pls) {
                char *resp = mcp_handle_payload(pls, zx.session);
                free(pls);
                if (resp) {
                    /* ★ 不要再调 esp_websocket_client_is_connected(zx.ws) 做前置判断!
                       zx.ws 可能已经被 zx 任务摘掉并销毁, 这里再传进去就是野指针,
                       实测直接 LoadProhibited 崩溃(两次都是崩在这一行)。
                       esp_websocket_client_send_text() 内部自己会判断连接状态并返回
                       错误码, 不需要我们先查。 */
                    /* 不能在这里直接 send_text —— 我们正处在 WS 接收回调里,
                       并发写同一个 mbedTLS SSL 上下文会把连接写崩。
                       入队交给 zx_task 发(详见 zx_enqueue_text 的说明)。 */
                    if (zx_enqueue_text(resp))
                        ESP_LOGI(TAG, "MCP -> 已排队 %d 字节", (int)strlen(resp));
                    else
                        ESP_LOGW(TAG, "MCP -> 队列满, 回复丢弃");
                    free(resp);
                }
            }
        } else {
            ESP_LOGW(TAG, "mcp 消息没有 payload");
        }

    } else if (strcmp(type, "alert") == 0 || strcmp(type, "iot") == 0) {
        ESP_LOGI(TAG, "收到 %s 消息(暂未处理)", type);
    }
    cJSON_Delete(root);
}

/* ---------- 下行音频的分帧搬运 ----------
 * StreamBuffer 是纯「字节流」, 完全不保留写入时的消息边界:
 * 按帧 Send、消费端一次 Receive 读一大块, 会读到「半帧」或「两帧粘在一起」,
 * opus_decode 必然失败 —— 现象就是"叮咚响了, 但一句人声都没有"。
 * 所以自己在字节流里加 2 字节长度头, 消费端按头精确读取。 */
static void zx_push_audio(const uint8_t *data, int len)
{
    if (!zx.sb || len <= 0 || len > OPUS_MAX_PKT) return;
    /* 音乐互斥中: 直接丢掉下行音频帧。
       服务端可能还在推上一轮的残余 TTS, 不丢的话会在音乐里混出一段人声。 */
    if (zx.audio_suspended) return;
    size_t need = (size_t)len + 2;

    /* ★ 必须"确认装得下整帧"再写, 而且头+数据一起写。
       StreamBuffer 是字节流, 且 xStreamBufferSend 在空间不足时**只写入能容纳的部分**:
       原来先写 2 字节头、再写数据, 数据可能只写进去一半 —— 消费端就按错误长度读,
       帧头一旦错位, 之后所有帧全部解不出来。现象: "回复到一半突然没声"、"很卡"。
       先查空间再一次性写; 装不下就整帧丢掉(下一帧照样能对上), 绝不写半帧。 */
    if (xStreamBufferSpacesAvailable(zx.sb) < need) return;

    static uint8_t pkt[OPUS_MAX_PKT + 2];
    pkt[0] = (uint8_t)(len & 0xFF);
    pkt[1] = (uint8_t)((len >> 8) & 0xFF);
    memcpy(pkt + 2, data, (size_t)len);
    xStreamBufferSend(zx.sb, pkt, need, 0);
}

/* 精确读满 n 字节(读不满就一直等, 直到超时) */
static bool zx_read_exact(uint8_t *out, int n, TickType_t wait)
{
    int got = 0;
    while (got < n) {
        size_t r = xStreamBufferReceive(zx.sb, out + got, (size_t)(n - got), wait);
        if (r == 0) return false;
        got += (int)r;
    }
    return true;
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket 已连接");
        zx.server_hello  = false;                       /* 等这一轮的 hello */
        zx.ws_connect_ms = esp_timer_get_time() / 1000;

        /* ★ 必须先置 ws_ready 再发 hello —— zx_send_json() 会检查这个标志,
           放在后面的话 hello 会被静默丢弃, 服务端永远不回, 整个协议握不上手。
           之前"只有叮咚、没有对话"就是卡在这里。 */
        zx.ws_ready = true;
        zx_send_hello();
        ESP_LOGI(TAG, "已发 hello (等服务器回 session)");
        set_state(zx.act_code[0] ? "待激活" : "就绪");
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket 断开");
        zx.ws_ready = false;
        zx.server_hello = false;
        /* ★ 走 zx_force_idle: 这里以前直接 mode=IDLE —— 恰恰是代码在下面
           goodbye 分支里警告过"会把最后一句截断"的那种做法, 而且功放 hold
           永远放不开(静态底噪一直响)。服务端每轮答完关连接, 必经此路。 */
        zx_force_idle();
        zx.session[0] = '\0';
        /* 这台服务端每轮回答完会主动关掉会话。若还在连续对话模式,
           排队"重连后继续聆听", 这样用户不用每轮都喊唤醒词。 */
        if (zx.cont_mode) {
            zx.want_listen = true;
            zx.force_reconnect = true;      /* 立刻安排重连, 不等超时 */
            ESP_LOGI(TAG, "服务端结束会话 → 排队: 重连后自动继续聆听");
        }
        set_state("离线");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x08) {           /* close */
            ESP_LOGW(TAG, "服务端关闭连接");
            /* ★ 这台服务端每轮回答完就主动关会话。别干等看门狗超时,
               直接标记重建, 让 zx 任务下一拍(约 1 秒内)就重连,
               否则用户会觉得"每轮都要重新喊你好小智"。 */
            zx.force_reconnect = true;
            break;
        }
        if (d->op_code == 0x09 || d->op_code == 0x0A) break;   /* ping/pong */

        /* 记录"最后一次收到服务端数据的时刻", 供 120 秒假死检测用 */
        zx.last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);

        if (d->op_code == 0x01) {           /* 文本 JSON */
            static char jbuf[RX_BUF_SIZE];
            if (d->payload_offset == 0 && d->data_len == d->payload_len) {
                zx_handle_json(d->data_ptr, d->data_len);
            } else {
                int off = d->payload_offset;
                if (off + d->data_len <= (int)sizeof(jbuf)) {
                    memcpy(jbuf + off, d->data_ptr, d->data_len);
                    if (off + d->data_len >= d->payload_len) {
                        zx_handle_json(jbuf, d->payload_len);
                    }
                } else {
                    /* ★ 以前超长帧是静默丢弃的 —— 服务端下发 >4KB 的 JSON
                       (比如超长的 tools 清单)会无声消失, 表现为"AI 突然不执行
                       某功能", 极难排查。至少喊一声。 */
                    ESP_LOGW(TAG, "超长 JSON 帧丢弃: payload=%u > 缓冲 %u",
                             (unsigned)d->payload_len, (unsigned)sizeof(jbuf));
                }
            }
        } else if (d->op_code == 0x02) {    /* 二进制 = TTS 的 Opus 帧 */
            /* 兜底: 万一没收到/漏掉 tts start, 收到二进制音频也直接进播报态,
               否则这些帧会因为状态不对而被丢弃 —— 表现就是"一点声音都没有" */
            if (zx.mode != MODE_SPEAK) {
                if (zx.sb) xStreamBufferReset(zx.sb);
                zx.mode = MODE_SPEAK;
                zx.tts_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
                ESP_LOGI(TAG, "收到下行音频 → 自动进入播报态");
            }
            /* 一帧 Opus 可能被 TCP 拆成多个 WebSocket 分片, 必须重组后再交给解码器,
               否则半个包送进 opus_decode 会解出噪声甚至报错 */
            static uint8_t bbuf[2048];
            int off = d->payload_offset;
            if (off == 0 && d->data_len == d->payload_len) {
                if (d->data_len > 0 && d->data_len <= OPUS_MAX_PKT) {
                    zx_push_audio((const uint8_t *)d->data_ptr, d->data_len);
                    zx.rx++;
                }
            } else if (off + d->data_len <= (int)sizeof(bbuf)) {
                memcpy(bbuf + off, d->data_ptr, d->data_len);
                if (off + d->data_len >= d->payload_len) {
                    if (d->payload_len <= OPUS_MAX_PKT) {
                        zx_push_audio(bbuf, d->payload_len);
                        zx.rx++;
                    }
                }
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket 错误");
        break;
    default:
        break;
    }
}

/* ======================= 音频任务 ======================= */
/* ======================= 播报音色(设备端 DSP 变声) =======================
 * ★ xiaozhi.me 的"音色"是在它服务端的控制台里配的, 设备↔服务器协议里
 *   根本没有"切换音色"的消息 —— 设备端做不到让服务器换声。
 * 所以这里是设备端自己的【真·变声效果器】, 对播报解码后的 PCM 实时做:
 * 变调重采样 / 环形调制(金属) / 幅度调制(电流) / 带通(对讲机) /
 * 反馈回声(深空) / 软削波(失真)。全部一阶实现, 每样本十几次乘加,
 * 24kHz 下占 CPU 不到 3%。
 * 只影响 AI 播报, 不影响麦克风上行(不然上传的也变声, 识别会变差)。 */
typedef enum {
    FX_ORIGINAL = 0,   /* 原声 */
    FX_METAL,          /* 机械金属: 环形调制(乘双极性低频振荡) → 金属质感 */
    FX_DEEP,           /* 电子低沉: 变低 + 低通, 闷一点 */
    FX_ALIEN,          /* 赛博外星: 变高变快 + 较高频环调 */
    FX_ELEC,           /* 电流音: 50Hz 幅度调制("嗡——") */
    FX_STRONG,         /* 强电流: 深度调制 + 重失真 */
    FX_RADIO,          /* 对讲机: 300~3200Hz 带通 + 削波 */
    FX_SPACE,          /* 深空回声: 变低 + 220ms 反馈回声(越来越闷) */
    FX_ELEC2,          /* 电流音2: 110Hz 调制 + 6Hz 颤音 */
} tts_fx_t;

static const struct { const char *id; const char *cn; tts_fx_t fx; float pitch; } TTS_VOICES[] = {
    { "original", "原声",     FX_ORIGINAL, 1.00f },
    { "metal",    "机械金属", FX_METAL,    1.00f },
    { "deep",     "电子低沉", FX_DEEP,     0.76f },
    { "alien",    "赛博外星", FX_ALIEN,    1.22f },
    { "elec",     "电流音",   FX_ELEC,     1.00f },
    { "strong",   "强电流",   FX_STRONG,   0.95f },
    { "radio",    "对讲机",   FX_RADIO,    1.00f },
    { "space",    "深空回声", FX_SPACE,    0.90f },
    { "elec2",    "电流音2",  FX_ELEC2,    1.08f },
};
#define TTS_VOICE_N (sizeof(TTS_VOICES) / sizeof(TTS_VOICES[0]))
#define TTS_FS      ((float)RX_SAMPLE_RATE)      /* 播报采样率 24000 */
#ifndef PI_F
#define PI_F 3.14159265358979f
#endif
static uint8_t s_voice_idx = 0;              /* 当前音色下标(持久化用) */

/* DSP 跨帧状态: 一句话是几十个 60ms 帧连起来的, 相位/滤波/回声必须连续 */
static struct {
    float ph1, ph2;          /* 振荡器相位(环调/AM/颤音共用, 各自累加) */
    float lp1, lp2;          /* 一阶低通状态 */
    float echo_lp;           /* 回声支路低通 */
    int16_t *echo;           /* 深空回声环形缓冲(PSRAM, 首次用到才分配) */
    int      echo_len, echo_wr;
} s_dsp;

static void dsp_reset(void)
{
    s_dsp.ph1 = s_dsp.ph2 = 0;
    s_dsp.lp1 = s_dsp.lp2 = s_dsp.echo_lp = 0;
}

/* 软削波: 声音越大越"破", 但不会像硬切那样产生刺耳的爆音 */
static inline float dsp_sat(float v, float drive)
{
    float x = v * drive;
    if (x >  4.0f) x =  4.0f;
    if (x < -4.0f) x = -4.0f;
    return x / (1.0f + fabsf(x));
}

/* 对一帧 PCM 施加当前音色特效; 返回输出样本数(变调会变)。原声直通。 */
static size_t tts_apply(int16_t *buf, size_t n)
{
    if (s_voice_idx >= TTS_VOICE_N || s_voice_idx == 0) return n;

    const tts_fx_t fx    = TTS_VOICES[s_voice_idx].fx;
    const float    pitch = TTS_VOICES[s_voice_idx].pitch;

    /* ① 变调重采样(线性插值): 输出帧数 = n/pitch */
    static int16_t vbuf[RX_FRAME_SAMPLES + 16];
    size_t on;
    if (pitch != 1.0f) {
        on = (size_t)((float)n / pitch + 0.5f);
        if (on > RX_FRAME_SAMPLES) on = RX_FRAME_SAMPLES;
        float pos = 0.0f;
        for (size_t i = 0; i < on; i++) {
            size_t i0 = (size_t)pos;
            if (i0 >= n - 1) { vbuf[i] = buf[n - 1]; pos += pitch; continue; }
            float f = pos - (float)i0;
            vbuf[i] = (int16_t)((float)buf[i0] + ((float)buf[i0 + 1] - (float)buf[i0]) * f);
            pos += pitch;
        }
    } else {
        on = n;
        memcpy(vbuf, buf, n * sizeof(int16_t));
    }

    /* 深空回声的环形缓冲: 首次用到才分配(~10KB, 放 PSRAM, 内部 RAM 出不起) */
    if (fx == FX_SPACE && !s_dsp.echo) {
        s_dsp.echo_len = (int)(TTS_FS * 0.22f);              /* 220ms */
        s_dsp.echo = heap_caps_malloc((size_t)s_dsp.echo_len * sizeof(int16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_dsp.echo) memset(s_dsp.echo, 0, (size_t)s_dsp.echo_len * sizeof(int16_t));
        s_dsp.echo_wr = 0;
    }

    /* ② 逐样本特效 */
    for (size_t i = 0; i < on; i++) {
        float x = (float)vbuf[i] / 32768.0f;
        float y = x;
        switch (fx) {
        case FX_METAL: {                                     /* 机械金属 */
            float o = sinf(s_dsp.ph1);                       /* 55Hz 双极性环调 */
            y = dsp_sat(x * o * 0.9f, 1.6f);
            s_dsp.ph1 += 2.0f * PI_F * 55.0f / TTS_FS;
            break;
        }
        case FX_DEEP: {                                      /* 电子低沉 */
            s_dsp.lp1 += 0.20f * (x - s_dsp.lp1);            /* ~900Hz 低通 */
            y = s_dsp.lp1 * 1.15f;
            break;
        }
        case FX_ALIEN: {                                     /* 赛博外星 */
            float o = sinf(s_dsp.ph1);                       /* 120Hz 环调 */
            y = dsp_sat(x * 0.45f + x * o * 0.55f, 1.5f);
            s_dsp.ph1 += 2.0f * PI_F * 120.0f / TTS_FS;
            break;
        }
        case FX_ELEC: {                                      /* 电流音 */
            float o = 0.5f + 0.5f * sinf(s_dsp.ph1);         /* 50Hz 单极性 AM */
            y = dsp_sat(x * (0.55f + 0.45f * o), 1.2f);
            s_dsp.ph1 += 2.0f * PI_F * 50.0f / TTS_FS;
            break;
        }
        case FX_STRONG: {                                    /* 强电流 */
            float o = 0.5f + 0.5f * sinf(s_dsp.ph1);         /* 60Hz 深调制 */
            y = dsp_sat(x * (0.15f + 0.85f * o), 3.2f);      /* + 重失真 */
            s_dsp.ph1 += 2.0f * PI_F * 60.0f / TTS_FS;
            break;
        }
        case FX_RADIO: {                                     /* 对讲机 */
            s_dsp.lp1 += 0.077f * (x - s_dsp.lp1);           /* 高通 300Hz(原-低通) */
            float h = x - s_dsp.lp1;
            s_dsp.lp2 += 0.66f * (h - s_dsp.lp2);            /* 低通 ~3200Hz */
            y = dsp_sat(s_dsp.lp2 * 1.5f, 1.7f);
            break;
        }
        case FX_SPACE: {                                     /* 深空回声 */
            s_dsp.lp1 += 0.45f * (x - s_dsp.lp1);            /* 主体略闷 */
            float dry = s_dsp.lp1 * 0.72f;
            float wet = 0.0f;
            if (s_dsp.echo) {
                wet = (float)s_dsp.echo[s_dsp.echo_wr] / 32768.0f;
                s_dsp.echo_lp += 0.35f * (wet - s_dsp.echo_lp);  /* 回声越滚越闷 */
                wet = s_dsp.echo_lp;
                int16_t fb = (int16_t)(dsp_sat(dry + wet * 0.55f, 1.1f) * 30000.0f);
                s_dsp.echo[s_dsp.echo_wr] = fb;              /* 反馈回写 */
                if (++s_dsp.echo_wr >= s_dsp.echo_len) s_dsp.echo_wr = 0;
            }
            y = dsp_sat(dry + wet * 0.55f, 1.2f);
            break;
        }
        case FX_ELEC2: {                                     /* 电流音2 */
            float o  = 0.5f + 0.5f * sinf(s_dsp.ph1);        /* 110Hz 蜂鸣 */
            float tr = 0.85f + 0.15f * sinf(s_dsp.ph2);      /* 6Hz 颤音 */
            y = dsp_sat(x * (0.45f + 0.55f * o) * tr, 1.8f);
            s_dsp.ph1 += 2.0f * PI_F * 110.0f / TTS_FS;
            s_dsp.ph2 += 2.0f * PI_F * 6.0f   / TTS_FS;
            break;
        }
        default:
            y = x;
            break;
        }
        if (s_dsp.ph1 > 2.0f * PI_F) s_dsp.ph1 -= 2.0f * PI_F;   /* 防漂移 */
        if (s_dsp.ph2 > 2.0f * PI_F) s_dsp.ph2 -= 2.0f * PI_F;

        int32_t v = (int32_t)(y * 32767.0f);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        vbuf[i] = (int16_t)v;
    }

    memcpy(buf, vbuf, on * sizeof(int16_t));
    return on;
}

bool ai_client_set_tts_voice(const char *name)
{
    if (!name || !name[0]) return false;
    /* ★ 早期版本的音色名继续认(老对话记录/AI 记忆里的说法不至于失效) */
    static const struct { const char *old, *nw; } ALIAS[] = {
        { "normal", "original" }, { "cute", "alien" },
        { "uncle",  "deep" },     { "robot", "metal" },
        { "正常",   "原声" },     { "萝莉",  "赛博外星" },
        { "大叔",   "电子低沉" }, { "机器人", "机械金属" },
    };
    for (size_t a = 0; a < sizeof(ALIAS) / sizeof(ALIAS[0]); a++) {
        if (strcasecmp(name, ALIAS[a].old) == 0) { name = ALIAS[a].nw; break; }
    }

    for (size_t i = 0; i < TTS_VOICE_N; i++) {
        if (strcasecmp(name, TTS_VOICES[i].id) == 0 ||
            strstr(TTS_VOICES[i].cn, name) != NULL ||
            (name[1] == '\0' && name[0] >= '0' && name[0] < '0' + (char)TTS_VOICE_N)) {
            s_voice_idx = (uint8_t)i;
            dsp_reset();
            nvs_handle_t h;
            if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, "tts_voice", s_voice_idx);
                nvs_commit(h);
                nvs_close(h);
            }
            ESP_LOGI(TAG, "播报音色 -> %s", TTS_VOICES[i].cn);
            return true;
        }
    }
    return false;
}

const char *ai_client_tts_voice_cn(void)
{
    if (s_voice_idx >= TTS_VOICE_N) return "原声";
    return TTS_VOICES[s_voice_idx].cn;
}

/* 开机恢复上次选的音色。
   ★ 旧固件存过 0..3(原声/萝莉/大叔/机器人), 要映射到新表的下标, 否则
   "存的是萝莉, 恢复成了机械金属"这类错位。 */
static void tts_voice_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "tts_voice", &v);
        nvs_close(h);
    }
    /* 旧: 0原声 1萝莉(1.25) 2大叔(0.82) 3机器人(1.50)
       新: 0原声 1机械 2低沉 3外星 4电流 5强电流 6对讲机 7深空 8电流2 */
    static const uint8_t OLD_MAP[4] = { 0, 3, 2, 1 };
    if (v < 4)                v = OLD_MAP[v];
    if (v < TTS_VOICE_N)      s_voice_idx = v;
}

static void zx_audio_task(void *arg)
{
    int err = 0;
    /* 照搬官方 audio_service.h 的 AS_OPUS_ENC_CONFIG():
         application = AUDIO    (不是 VOIP)
         complexity  = 0        ← 最值钱的一条, 见下
         fec = false, dtx = true, vbr = true
       ★ complexity 默认是 9, Opus 编码是本任务里最重的一份 CPU 开销。
         降到 0 之后才有余力在同一个任务里实时解码下行音频 ——
         这是"一卡一卡"的另一半原因(编码抢了给解码的时间)。 */
    OpusEncoder *enc = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || !enc) {
        ESP_LOGE(TAG, "Opus 编码器创建失败: %d", err);
        vTaskDelete(NULL);
        return;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));      /* 省 CPU 给解码 */
    opus_encoder_ctl(enc, OPUS_SET_DTX(1));             /* 静音时不发包 */
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));

    /* 解码器建在 24kHz(不是 16k): 与服务端下发的采样率一致,
       输出 1440 样本/帧 正好对上 24kHz 的喇叭 I2S, 不做重采样 → 声音更清亮 */
    OpusDecoder *dec = opus_decoder_create(RX_SAMPLE_RATE, 1, &err);
    if (err != OPUS_OK || !dec) {
        ESP_LOGE(TAG, "Opus 解码器创建失败: %d", err);
        vTaskDelete(NULL);
        return;
    }

    static int16_t pcm[FRAME_SAMPLES * 2];
    static uint8_t pkt[OPUS_MAX_PKT];
    /* 上行编码用的整帧累积缓冲: mic_read 超时时会返回不足一帧的样本,
       而 opus_encode 只接受 2.5/5/10/20/40/60ms 的标准帧长 ——
       半帧会直接返回 OPUS_BAD_ARG, 那 60ms 就一个字节都发不出去。 */
    static int16_t txacc[FRAME_SAMPLES];
    static size_t  txlen = 0;
    int            speak_frames = 0;   /* 播报已解码帧数(诊断用) */
    size_t         rx_fill = 0;        /* 麦克风已攒下的样本数(凑满一整帧才处理) */
    int64_t        t_start = 0;        /* 本轮整帧采集的起始时刻 */
    ESP_LOGI(TAG, "音频任务就绪 (Opus 16kHz/%dms, 免触摸VAD=%d)", FRAME_MS, (int)zx.vad_on);

    while (1) {
        /* ★ 跨任务请求的清理: 只能在本任务(读者上下文)做, 见 zx_force_idle 注释。
           放在挂起检查之前 —— 挂起/断开路径设置的标志在挂起期间也要被消费。 */
        if (zx.sb_flush_req) {
            zx.sb_flush_req = false;
            if (zx.sb) xStreamBufferReset(zx.sb);
        }
        if (zx.mic_drain_req) {
            zx.mic_drain_req = false;
            mic_drain_backlog();
        }

        /* ---- ★ 音乐互斥: 挂起期间整条 AI 音频链停摆 ----
           不读麦克风(省 I2S/DMA 带宽)、不跑唤醒词(最重的 CPU 开销)、
           不上行编码、也不解码播放 TTS。音乐解码因此能独占这个核。
           这里只睡 50ms, 开销可忽略, 恢复时立刻就能接着跑。 */
        if (zx.audio_suspended) {
            if (zx.suspend_log == 0) {
                zx.suspend_log = 1;
                ESP_LOGI(TAG, "AI 音频已挂起(音乐播放中): 唤醒词/上行/播报全部暂停");
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* ★ 唤醒词引擎重建: 必须由本任务执行 —— 只有本任务会碰引擎,
           从别的任务 destroy 会和使用中的模型撞车(use-after-free 崩溃)。 */
        if (zx.ww_restart_req) {
            zx.ww_restart_req = false;
            if (zx.ww_ready) wake_word_restart();
        }

        /* ---- 播报中: 只解码播放. 不读麦克风, 否则扬声器的声音会把自己触发 ---- */
        if (zx.mode == MODE_SPEAK) {
            rx_fill = 0;   /* 播报期间不读麦克风, 残留的半帧丢掉, 免得拼进下一轮 */
            /* ★ 抖动缓冲(治卡顿的关键):
               开播前先攒够 PREBUFFER_BYTES。攒够之后再开始播, 而播放消耗
               60ms/帧、服务端也在持续下发, 缓冲只会越攒越深 —— 之后网络
               再怎么抖动都咬不到播放, 听感就是连贯的一整句而不是一顿一顿。
               攒不够也不能死等(短句会明显延迟), 超时就先播。 */
            if (zx.tts_start_ms) {
                size_t avail = zx.sb ? xStreamBufferBytesAvailable(zx.sb) : 0;
                uint32_t waited = (uint32_t)(esp_timer_get_time() / 1000) - zx.tts_start_ms;
                if (avail < PREBUFFER_BYTES && waited < (uint32_t)PREBUFFER_TIMEOUT_MS) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                ESP_LOGI(TAG, "抖动缓冲就绪 %u 字节(等待 %ums) → 开播",
                         (unsigned)avail, (unsigned)waited);
                zx.tts_start_ms = 0;
            }

            uint8_t hdr[2];
            if (!zx_read_exact(hdr, 2, pdMS_TO_TICKS(20))) {
                /* 没读到新帧 —— 三种情况要分开处理 */
                size_t avail = zx.sb ? xStreamBufferBytesAvailable(zx.sb) : 0;
                if (avail == 0 && zx.tts_draining) {
                    /* 服务端已说"发完" + 缓冲已放空 → 这一轮真正结束 */
                    ESP_LOGI(TAG, "播报完成: 共解 %d 帧", speak_frames);
                    zx_finish_speaking();
                } else if (avail == 0) {
                    /* 没在收尾却没数据: 可能是 tts stop 丢了。
                       累计静默, 超时就强制收尾 —— 否则会永远卡在"说话中"不读麦克风,
                       表现就是"我再怎么说都没反应"。 */
                    zx.speak_idle_ms += 20;
                    if (zx.speak_idle_ms >= SPEAK_IDLE_MS) {
                        ESP_LOGW(TAG, "播报态静默 %dms, 强制收尾(回聆听)",
                                 zx.speak_idle_ms);
                        zx_finish_speaking();
                    }
                }
                continue;
            }
            zx.speak_idle_ms = 0;

            int flen = hdr[0] | (hdr[1] << 8);
            if (flen <= 0 || flen > OPUS_MAX_PKT) {
                ESP_LOGW(TAG, "下行帧长异常 %d, 复位缓冲重新同步", flen);
                xStreamBufferReset(zx.sb);
                continue;
            }
            if (!zx_read_exact(pkt, flen, pdMS_TO_TICKS(20))) {
                /* 头读到了数据没跟上 —— 只可能是被写坏/被截断, 复位重新同步 */
                ESP_LOGW(TAG, "下行帧不完整, 复位缓冲");
                xStreamBufferReset(zx.sb);
                continue;
            }

            int samples = opus_decode(dec, pkt, flen, pcm, RX_FRAME_SAMPLES, 0);
            if (samples > 0) {
                zx.dec_fail = 0;
                /* ★ 音色特效(见 TTS_VOICES 说明): DSP 变声后写喇叭 */
                size_t ns = tts_apply(pcm, (size_t)samples);
                /* 前 3 帧打印样本数/帧长/电平, 一眼看出"解码成功但内容是空的"
                   还是"压根没收到音频" */
                size_t wr = speaker_write(pcm, ns, 500);
                if (speak_frames < 3) {
                    ESP_LOGI(TAG, "解码第%d帧: %d 样本(实际写入%u), 帧长=%d, 电平=%.0f",
                             speak_frames + 1, samples, (unsigned)wr, flen,
                             rms_of(pcm, (size_t)samples));
                }
                if (++speak_frames % 25 == 0)
                    ESP_LOGI(TAG, "播报中: 已解 %d 帧 (rx=%u)",
                             speak_frames, (unsigned)zx.rx);
            } else {
                ESP_LOGW(TAG, "opus_decode 失败 %d (帧长=%d)", samples, flen);
                /* 连续失败说明帧头已经错位, 复位重新同步, 否则后面全废 */
                if (++zx.dec_fail >= 3) {
                    ESP_LOGW(TAG, "连续解码失败, 复位缓冲重新同步");
                    zx.dec_fail = 0;
                    xStreamBufferReset(zx.sb);
                }
            }
            continue;
        }

        /* ---- 连续对话: 播报结束后等一小会, 自动重新进入聆听 ----
           这样用户不用每轮都喊"你好小智", 可以像聊天一样连着说。 */
        if (zx.cont_wait_ms > 0) {
            zx.cont_wait_ms -= AUDIO_LOOP_MS;
            if (!zx.cont_mode) {
                zx.cont_wait_ms = 0;                       /* 非连续模式, 不重开 */
            } else if (zx.mode == MODE_IDLE && zx.ws_ready && zx.cooldown_ms == 0) {
                zx.cont_wait_ms = 0;
                ESP_LOGI(TAG, "连续对话: 自动重新聆听 (不用再喊唤醒词)");
                zx.vad_session = true;
                zx_begin_listen();          /* 里面会重置 heard_voice/cont_idle */
            } else if (zx.cont_wait_ms <= 0) {
                /* ★ 条件还没满足时不能把倒计时一次性作废。
                   最常见的是冷却期(600ms)还没走完, 而播报后的等待只有 500ms ——
                   原写法倒计时一归零就再也不会重试, 只能等 zx 任务每秒一次的
                   兜底, 用户感觉"答完要愣一两秒才又能说话"。这里改成下一轮再试。 */
                zx.cont_wait_ms = AUDIO_LOOP_MS;
            }
        }

        /* ---- 采集: 攒满一整帧 60ms(960 样本)再往下走 ----
         * ★ 这里曾经是"唤醒词时灵时不灵"的真正元凶, 而且很隐蔽:
         *   i2s_channel_read() 一次往往只交付一个 DMA 描述符的量
         *   (dma_frame_num = 240 个样本), 也可能本次返回 0 —— 它并不保证
         *   一次性填满你要的 960。原代码写的是"不足半帧就 continue",
         *   于是把刚读到的 240 个样本【直接扔掉】。音频被切得七零八落,
         *   而 WakeNet 要求语音连续, 结果就是怎么喊都喊不出来、偶尔又能喊出来;
         *   上行语音同样残缺, 服务端识别自然时好时坏。
         *   正确做法: 分几次读, 攒够一整帧再处理, 一个样本都不丢。 */
        if (rx_fill == 0) t_start = esp_timer_get_time();
        size_t got = mic_read(pcm + rx_fill, FRAME_SAMPLES - rx_fill, 80);
        if (got == 0) {
            vTaskDelay(1);          /* 暂时没数据, 让出 CPU 免得空转 */
            continue;
        }
        rx_fill += got;
        if (rx_fill < FRAME_SAMPLES) continue;   /* 还没攒够, 接着读 */
        rx_fill = 0;
        size_t n = FRAME_SAMPLES;                /* 到这里手里必然是完整一帧 */

        /* 心跳: 每 ~3 秒报一次, 确认采集连续、循环跟得上音频产生速度 */
        static int hb_cnt = 0;
        if (++hb_cnt >= 50) {
            hb_cnt = 0;
            /* 带上内部 RAM 的剩余/最大连续块 —— 用来盯"内存是不是在慢慢漏"。
               连续块一旦掉到低于 WS_TASK_STACK, WebSocket 任务就建不出来
               (Error create websocket task), 设备再也连不上。 */
            ESP_LOGI(TAG, "音频心跳: mode=%d 整帧=%u/%d 循环=%dms (ww=%d ws=%d) 内部RAM=%u/%u",
                     (int)zx.mode, (unsigned)n, FRAME_SAMPLES,
                     (int)((esp_timer_get_time() - t_start) / 1000),
                     (int)zx.ww_ready, (int)zx.ws_ready,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
        /* 增益前的原始电平(诊断用):
           太低 → 麦克风/接线/供电有问题; 削波样本很多 → 增益过头, 失真同样会让唤醒变差 */
        float raw_rms = rms_of(pcm, n);

        /* ★ 麦克风增益放在这里(而不是只给上行): 唤醒词引擎和 VAD 用的都是
           同一份 pcm, 原始电平偏低会让"喊了听不见 / 识别成乱码 / 乱答呃…"。
           做数字增益 + 饱和削波, 抬高语音但不破音。 */
        int clipped = 0;
        for (size_t i = 0; i < n; i++) {
            int32_t v = (int32_t)((float)pcm[i] * TX_GAIN);
            if (v >  32767) { v =  32767; clipped++; }
            if (v < -32768) { v = -32768; clipped++; }
            pcm[i] = (int16_t)v;
        }
        float rms = rms_of(pcm, n);
        zx.mic_rms = rms;

        /* ★ 喂给「音乐频谱」灯效: 没放歌时对着麦克风说话/外放音乐,
           8 颗灯照样跳频谱。内部自判(放歌中或非频谱灯效直接返回), 无负担。 */
        light_mic_pcm(pcm, n);

        if (zx.mode == MODE_LISTEN) {
            /* 上行: 先攒够一整帧 960 样本, 再 Opus 编码发二进制帧 */
            if (n > 0) {
                size_t space = sizeof(txacc) - txlen;
                size_t cp = (n < space) ? n : space;
                /* 增益已在 mic_read 之后统一做过, 这里直接搬 */
                memcpy(txacc + txlen, pcm, cp * sizeof(int16_t));
                txlen += cp;
            }
            if (txlen >= FRAME_SAMPLES && zx.ws_ready &&
                esp_websocket_client_is_connected(zx.ws)) {
                int len = opus_encode(enc, txacc, FRAME_SAMPLES, pkt, sizeof(pkt));
                if (len > 0) {
                    UBaseType_t hw0 = uxTaskGetStackHighWaterMark(NULL);
                    esp_websocket_client_send_bin(zx.ws, (const char *)pkt, len,
                                                  pdMS_TO_TICKS(100));
                    if (zx.tx == 0) {   /* 只测第一帧, 看 TLS 发送实际吃多少栈 */
                        ESP_LOGI(TAG, "栈实测: 发送前剩余 %u 字, 发送后 %u 字",
                                 (unsigned)hw0, (unsigned)uxTaskGetStackHighWaterMark(NULL));
                    }
                    zx.tx++;
                    /* 每 20 帧(约 1.2 秒)打一条, 用来确认上行音频是否真的在发 */
                    if ((zx.tx % 20) == 1) {
                        ESP_LOGI(TAG, "上行中: tx=%u rx=%u 每帧 %d 字节",
                                 (unsigned)zx.tx, (unsigned)zx.rx, len);
                    }
                } else {
                    ESP_LOGW(TAG, "opus_encode 失败: %d", len);
                }
                txlen = 0;
            }
            /* --- 自动结束 / 连续对话的超时判定 ---
               ★ 阈值必须自适应, 不能用固定的 VAD_MIN_RMS * TX_GAIN。
               实测本机增益后的房间噪声底就在 1500~2600, 而固定阈值只有
               420*3 = 1260 —— 比噪声还低, 于是 voice 永远为真、silence_ms
               一直被清零, 结果就是"播完回答后一直挂在聆听里不停上传,
               直到服务端把会话踢掉"(实测连续上传了 37 秒、tx 到 1101)。
               这里改成跟免触摸 VAD 用同一套自适应噪声底:
               下降快(0.25)、上升慢(0.015), 说话不会把噪声底带跑。 */
            if (rms < zx.noise) zx.noise += (rms - zx.noise) * 0.25f;
            else                zx.noise += (rms - zx.noise) * 0.015f;
            float listen_thr = zx.noise * VAD_MARGIN;
            float floor_thr  = VAD_MIN_RMS * TX_GAIN;      /* 绝对下限, 防噪声底过低 */
            if (listen_thr < floor_thr) listen_thr = floor_thr;
            zx.vad_thr = listen_thr;

            bool voice = (rms > listen_thr);
            if (voice) {
                zx.heard_voice = true;
                zx.silence_ms = 0;
            } else if (zx.vad_session) {
                zx.silence_ms += FRAME_MS;
            }

            if (zx.vad_session && zx.heard_voice && zx.silence_ms >= VAD_STOP_MS) {
                ESP_LOGI(TAG, "VAD: 静音 %dms → 说完了", zx.silence_ms);
                ai_client_stop_listening();
            } else if (zx.vad_session && !zx.heard_voice) {
                /* 连续对话里开了聆听却一直没人说话 → 超时退出, 回到唤醒词模式 */
                zx.cont_idle_ms += FRAME_MS;
                if (zx.cont_idle_ms >= CONT_IDLE_MS) {
                    /* 只停掉这一轮聆听, 不退出连续对话模式 ——
                       会话一结束服务端会关连接, 重连后靠 want_listen 自动再开。 */
                    ESP_LOGI(TAG, "连续对话: %dms 没人说话 → 暂停聆听(等待重连)",
                             zx.cont_idle_ms);
                    ai_client_stop_listening();
                }
            }
        } else if (zx.ww_ready && !zx.vad_on) {
            /* ---- 唤醒词模式(首选): 只有听到「你好小智」才开口 ----
             *
             * ★ 这里刻意【不要求】ws_ready / 冷却结束就喂音频。
             *   这台服务端每 ~70 秒就会关掉一次会话, 重连还要几秒; 如果只在
             *   "联网 + 不在冷却"时才调用 wake_word_feed(), 那么这些窗口里
             *   唤醒词引擎根本没在跑, 用户感觉就是"有时候怎么喊都没反应"。
             *   而且引擎要求音频连续, 断断续续喂会让内部状态一直起不来。
             *   改成永续喂, 只把「命中后要不要动作」交给就绪/冷却判定。 */
            /* 每 ~2 秒报一次实时电平与循环耗时, 用于判断唤醒不灵的原因:
                 · rms 太低      → 麦克风/接线/供电问题
                 · 削波数很大    → 增益过头, 波形失真, 唤醒同样会变差
                 · 循环耗时 >>60ms → 本任务跟不上音频产生速度, 会丢样本 */
            static int ww_dbg = 0;
            if (++ww_dbg >= 33) {
                ww_dbg = 0;
                ESP_LOGI(TAG, "唤醒监听: 原始rms=%.0f 增益后rms=%.0f 削波=%d/%u 循环=%dms",
                         raw_rms, rms, clipped, (unsigned)n,
                         (int)((esp_timer_get_time() - t_start) / 1000));
            }

            const char *word = wake_word_feed(pcm, n);
            if (word != NULL && zx.cooldown_ms > 0) {
                /* 刚说完话的冷却期: 引擎继续跑, 但忽略这次命中 */
                ESP_LOGI(TAG, "唤醒命中但仍在冷却(%dms), 忽略", zx.cooldown_ms);
            } else if (word != NULL) {
                ESP_LOGI(TAG, "★ 唤醒词命中: 「%s」→ 开始对话", word);
                /* 应答音: 让你知道"它在听"; 先响再清引擎残留, 免把提示音当输入。
                   频率取低一些: Class-D 功放在低频段对供电纹波不敏感, 也不容易
                   和 WiFi 突发耦合出刺耳的兹拉声。 */
                if (zx.wake_beep) {
                    speaker_beep(784, 70);
                    speaker_beep(1046, 90);
                }
                wake_word_reset();
                if (!zx.ws_ready || !zx.ws) {
                    /* ★ 唤醒词命中了, 但 WebSocket 还没就绪 —— 这台服务端每轮回答完
                       会主动关闭会话, 设备正在重连, 这段窗口里发 listen 会被丢弃,
                       表现就是"喊了你好小智, 它也在听, 但怎么都没反应"。
                       这里改成排队: 连上后自动开始聆听, 不用再喊一遍。 */
                    ESP_LOGW(TAG, "唤醒命中但 WS 未就绪(%s) → 排队, 连上后自动开始聆听",
                             zx.ws ? "重连中" : "未创建");
                    zx.want_listen = true;
                    zx.cont_mode   = true;
                    if (zx.ws) zx.force_reconnect = true;   /* 催一下尽快重建 */
                } else {
                    zx_send_wake_detected(word);   /* 先把唤醒词告诉服务端 */
                    zx.vad_session = true;         /* 用静音检测自动结束 */
                    zx_begin_listen();
                }
            }
        } else if (zx.vad_on && zx.ws_ready && zx.cooldown_ms == 0) {
            /* 空闲: 本地 VAD(唤醒词不可用时的后备), 听到持续人声就自动开口 */
            if (rms < zx.noise) zx.noise += (rms - zx.noise) * 0.25f;   /* 下降快 */
            else                zx.noise += (rms - zx.noise) * 0.015f;  /* 上升慢 */
            float thr = zx.noise * VAD_MARGIN;
            if (thr < VAD_MIN_RMS) thr = VAD_MIN_RMS;
            zx.vad_thr = thr;

            if (rms > thr) {
                if (++zx.voice_frames >= VAD_START_FRAMES) {
                    zx.voice_frames = 0;
                    zx.vad_session  = true;
                    ESP_LOGI(TAG, "VAD: 检测到人声 (rms=%.0f 阈值=%.0f) → 自动开始对话",
                             rms, thr);
                    zx_begin_listen();
                }
            } else {
                zx.voice_frames = 0;
            }
            /* 每约 2 秒把能量/噪声底/阈值打到串口, 方便标定灵敏度 */
            if (++zx.vad_log_cnt >= 33) {
                zx.vad_log_cnt = 0;
                ESP_LOGI(TAG, "VAD: rms=%.0f 噪声底=%.0f 阈值=%.0f (说话时应明显超过阈值)",
                         rms, zx.noise, thr);
            }
        }

        if (zx.cooldown_ms > 0) {
            zx.cooldown_ms -= FRAME_MS;
            if (zx.cooldown_ms < 0) zx.cooldown_ms = 0;
        }

        /* ★ 节奏控制 —— 这里原本是个致命 bug, 是"唤醒词时灵时不灵"的根因:
         *
         *   本循环每轮通过 mic_read() 恰好消费 FRAME_MS(60ms) 的音频(960 样本)。
         *   原写法是"补齐到 50ms, 且最少睡 1 tick"。但 CONFIG_FREERTOS_HZ=100,
         *   1 tick = 10ms —— 于是每轮变成 ~70ms, 比消费掉的音频多 10ms。
         *   I2S 的 DMA 缓冲只有 dma_desc_num(6) x dma_frame_num(240) = 1440 样本
         *   (90ms), 约 0.6 秒就填满, 之后每 70ms 固定丢掉约 160 个样本 ——
         *   音频里每隔 70ms 就有一个 10ms 的洞。
         *
         *   WakeNet 要求音频连续, 音频被周期性打洞 → 唤醒率暴跌, 表现就是
         *   "怎么喊都喊不出来, 偶尔又能喊出来"; 上行语音同样被打洞,
         *   所以服务端识别也时好时坏、连续对话不顺畅。
         *
         *   修法: 不再"补齐到某个值"。正常路径下 mic_read() 自己就阻塞 60ms,
         *   恰好等于本轮消费掉的音频, 周期天然就是 60ms, 不必也不能再加延时 --
         *   加任何延时都会让循环慢于音频产生速度, 从而丢样本。
         *   只有在本轮明显偏快(读数不完整)时才补 1 tick, 避免自旋饿死 idle 任务。
         *   (mic_read 阻塞期间会让出 CPU, 所以看门狗也不会被触发) */
        int elapsed = (int)((esp_timer_get_time() - t_start) / 1000);
        if (elapsed < 10) vTaskDelay(1);
    }
}

/* ======================= 连接管理任务 ======================= */
static void zx_start_ws(void)
{
    if (zx.ws) {
        esp_websocket_client_stop(zx.ws);
        esp_websocket_client_destroy(zx.ws);
        zx.ws = NULL;
    }
    if (!zx.ws_url[0]) {
        ESP_LOGW(TAG, "没有 websocket 地址, 等激活/OTA");
        return;
    }

    static char headers[640];
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\n"
             "Protocol-Version: 1\r\n"
             "Device-Id: %s\r\n"
             "Client-Id: %s\r\n",
             zx.ws_token, zx.device_id, zx.client_id);
    /* esp_websocket_client 要求 \n 分隔, \r\n 也接受 */
    for (char *p = headers; *p; p++) if (*p == '\r') *p = ' ';

    /* ★ 这里【不再】提前判断"内存不够就先停音乐"。
     *
     * 原来那道判断留了 2KB 余量(WS_TASK_STACK+2048), 结果实测出现过
     *   连续块 5376 字节, 而任务栈其实只要 5120 —— 白白把音乐停掉,
     *   用户听到的就是"歌播到一半突然没了"。
     * 判据偏保守 + 停音乐代价太大, 所以改成:
     *   先直接建; 真的建不起来(见 zx_task 里 zx.ws == NULL 的分支)
     *   才停音乐腾内存, 然后重试一次。 */
    ESP_LOGI(TAG, "建 WS 前: 内部RAM最大连续块=%u 剩余=%u, PSRAM剩余=%u",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_websocket_client_config_t cfg = {
        .uri = zx.ws_url,
        .headers = headers,
        /* MCP tools/list 的回复实测有 2065 字节, 而 Kconfig 默认
           CONFIG_WS_BUFFER_SIZE 只有 1024 —— 缓冲不足时发送会失败并把
           连接写崩(esp_transport_write ... SSL_WRITE_FAILED)。
           4096 够用且省内部 RAM: 这两个缓冲(rx+tx)在内部 RAM 里, 给到 8192
           的那阵子正好撞上网易云拉歌单, WS 任务就建不起来了(见 sdkconfig
           里 SPIRAM_MALLOC_ALWAYSINTERNAL 的注释)。 */
        .buffer_size = 4096,
        .task_stack = WS_TASK_STACK,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = false,
    };
    zx.ws = esp_websocket_client_init(&cfg);
    if (!zx.ws) { ESP_LOGE(TAG, "WebSocket 初始化失败"); return; }
    esp_websocket_register_events(zx.ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(zx.ws);
    ESP_LOGI(TAG, "正在连接 %s", zx.ws_url);
    set_state("连接中");
}

static void zx_task(void *arg)
{
    /* 启动时先把上次存下来的服务器信息读出来 */
    nvs_str_get("ws_url", zx.ws_url, sizeof(zx.ws_url));
    nvs_str_get("ws_token", zx.ws_token, sizeof(zx.ws_token));
    if (zx.ws_url[0]) ESP_LOGI(TAG, "上次的服务器: %s", zx.ws_url);

    bool ota_done = false;
    int  offline_cnt = 0;

    while (1) {
        /* 排空待发文本队列(hello / listen / abort / mcp 回复)。
           必须在这个任务里发, 不能在 WS 接收回调里发 —— 详见 zx_enqueue_text。
           先阻塞 100ms 等一等, 保证 MCP 回复的低延迟。 */
        if (zx.txq) {
            char *m = NULL;
            if (xQueueReceive(zx.txq, &m, pdMS_TO_TICKS(100)) == pdTRUE && m) {
                do {
                    if (zx.ws) {
                        /* 注意: esp_websocket_client_send_text() 返回的是
                           「已发送字节数」(int), 不是 esp_err_t。
                           之前拿它跟 ESP_OK(0) 比较, 于是把每一次成功发送
                           都误报成"发送失败: ERROR", 白白吓人。 */
                        int sent = esp_websocket_client_send_text(
                            zx.ws, m, strlen(m), pdMS_TO_TICKS(500));
                        if (sent < 0)
                            ESP_LOGW(TAG, "文本发送失败 (len=%d)", (int)strlen(m));
                    }
                    free(m);
                    m = NULL;
                } while (xQueueReceive(zx.txq, &m, 0) == pdTRUE && m);
            }
        }

        if (!zx.online) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (++offline_cnt == 5) {
                offline_cnt = 0;
                set_state("未联网");
                ESP_LOGW(TAG, "STA 未联网, 去 http://192.168.4.1 配 WiFi");
            }
            continue;
        }
        offline_cnt = 0;

        /* 每 5 分钟或首次联网时做一次 OTA(拿激活码 + 最新服务器地址) */
        if (!ota_done) {
            set_state("激活中");
            if (zx_ota_request()) ota_done = true;
            if (zx.act_code[0]) set_state("待激活");
        }

        /* WebSocket 只创建一次, 重连交给 esp_websocket_client 自己的
           auto-reconnect(reconnect_timeout_ms=5s)。
         *
         * 千万不要写成 if (!is_connected()) zx_start_ws() ——
         * is_connected() 在 TLS/WS 握手完成前一直是 false, 于是每 5 秒就
         * new 一个 client 而旧的不销毁: 泄漏 client 及其 24KB 内部 RAM 任务栈
         * (实测连续块从 28672 掉到 11264), 还会出现两个 client 同时连着、
         * 各收一份服务端消息, 状态彻底混乱。
         * 只有确认卡死(约 60 秒连不上)才销毁重建。 */
        /* 排队中的"开始聆听" —— WS 一就绪就补上(唤醒命中时连接还没好, 或
           服务端刚结束上一轮会话)。这就是"连续对话"能跨重连生效的关键。 */
        if (zx.want_listen && zx.ws_ready && zx.mode == MODE_IDLE &&
            zx.cooldown_ms == 0) {
            zx.want_listen = false;
            ESP_LOGI(TAG, "WS 已就绪 → 自动开始聆听(不用再喊唤醒词)");
            zx.vad_session = true;
            zx_begin_listen();
        }

        /* 烧录后第一次开机: WS 就绪后自动跑一遍唤醒路径自检
           (提示音 → 上报唤醒词 → 上行 8 秒 → 静音结束), 验证整条链路 */
        if (zx.ws_ready && !zx.selftested) {
            zx.selftested = true;
            if (zx_selftest_should_run()) {
                ESP_LOGI(TAG, "==== 开机自检: 唤醒路径(应听到叮咚) ====");
                ai_client_test_wake("你好小智");
                ESP_LOGI(TAG, "==== 开机自检: MCP 工具执行链路(应看到爆闪+亮度变化) ====");
                ai_client_test_mcp();
                ESP_LOGI(TAG, "==== 开机自检结束 ====");
            }
        }

        /* ★ 照搬官方 Protocol::IsTimeout() (protocol.cc:81-90):
           120 秒没收到服务端任何数据 → 判定连接"假死"。
           官方没有 ping / keep_alive, 这是它唯一的自愈手段。
           我们的现象是"能连上、但说啥都没反应" —— TCP 还活着但服务端那侧
           已经把会话丢了, 不重建就再也回不来。 */
        if (zx.ws_ready && zx.last_rx_ms) {
            uint32_t silence_ms = (uint32_t)(esp_timer_get_time() / 1000) - zx.last_rx_ms;
            if (silence_ms > RX_TIMEOUT_MS) {
                ESP_LOGW(TAG, "已 %ums 未收到服务端数据 → 判定假死, 彻底重建连接",
                         (unsigned)silence_ms);
                /* 假死是"TCP 还活着但服务端已经丢了会话", 自动重连救不了 —— 
                   只有这种才用硬重建(见下面 zx.hard_reconnect 的说明)。 */
                zx.hard_reconnect = true;
                zx.last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
            }
        }

        /* ★★ 断线不再"一断就销毁重建" —— 这是"歌播一半突然没了"的真凶 ★★
         *
         * esp_websocket_client 已经配了 reconnect_timeout_ms=5000 且
         * disable_auto_reconnect=false: 服务端关掉会话后, 它会在【同一个任务】
         * 里自动重连, 完全不需要新的内部 RAM。
         *
         * 而我们原来每次断线都 destroy + init + start, 新建任务要 7168 字节
         * 【连续】内部 RAM。播音乐时内部 RAM 被音乐模块占掉一部分, 最大连续块
         * 只剩 5KB —— 于是下面那段"内存不够就先停掉音乐"的兜底逻辑被触发,
         * 音乐被无辜杀掉。用户听到的就是"播到一半突然没了, 接着换下一首"。
         *
         * 现在只有两种情况才彻底重建(都很罕见):
         *   1) hard_reconnect: 连接假死(120 秒收不到任何数据), 自动重连救不了
         *   2) 连续 45 秒以上完全连不上 —— 说明自动重连也没救回来
         * 注意 age: 刚创建的连接还在握手, 别急着掐掉。 */
        uint32_t age_ms = (uint32_t)(esp_timer_get_time() / 1000) - zx.ws_connect_ms;
        if (zx.ws && !esp_websocket_client_is_connected(zx.ws)) {
            if (++zx.stuck == 12)
                ESP_LOGW(TAG, "12 秒未连上(自动重连没生效) → 即将彻底重建");
        } else {
            zx.stuck = 0;
        }
        /* ★ 12 秒就重建, 不要等 45 秒 —— 实测网络中断超过约 30 秒时,
           音乐那条 TCP 也会被 LWIP 重传超时判死。断网窗口越短, 音乐越安全。 */
        /* ★★★ 音乐模式: 播歌时 AI 对话全部让路, 摸头顶三下暂停即恢复 ★★★
         *
         * 让路的是【音频链】(唤醒词/上行/播报全停, 见 ai_client_set_audio_suspended),
         * 而 WebSocket 必须【保持连接】—— 这里踩过一个坑, 记下来:
         *   曾试过"音乐期间把 WS 也关掉省内存", 省下的只有 1.7~5.9KB, 但代价是
         *   暂停后 WS 建不回来(音乐会话还活着, 内部 RAM 最大连续块只剩 1.7KB,
         *   而 WS 任务要 5~7KB 连续) → 用户摸三下暂停后喊小智【完全没反应】。
         *   而音乐本身很稳(缓冲 62/64KB、见底 0 次), 根本不需要省这点内存。
         * 结论: 让路 = 让出 CPU 和喇叭, 不是让出连接。 */
        /* ★ 顺手把待落盘的网易云登录态写进 NVS。
           为什么放在这个任务里: 写 flash 会关 cache, 而 zx 任务的栈在内部 RAM
           (PSRAM 栈的任务做这件事会触发
            esp_task_stack_is_sane_cache_disabled 断言重启 —— 踩过)。
           也不另外开任务: 内部 RAM 太紧, 建落盘任务会失败(也踩过),
           结果就是"登录了但一重启就丢"。 */
        if (ne_cookie_save_pending()) ne_cookie_save_now();

        /* ★ 用 music_owns_audio() 而不是 music_is_playing()/music_is_active():
           它精确表示"音乐正在出声、占着喇叭"。暂停后它是 false,
           于是 AI 立刻恢复(用户摸三下暂停就是为了喊小智)。 */
        bool mp = music_owns_audio();
        if (mp && !zx.music_mode) {
            zx.music_mode = true;
            ESP_LOGW(TAG, "★ 进入音乐模式: AI 音频链让路(唤醒词/上行/播报全停) [内部RAM=%u]",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            /* ★★ 音乐模式里把 WS 关掉, 内存全让给音乐 ★★
             * 音频已挂起, 这条连接收不到也发不出任何东西, 却占着 ~40KB 内部
             * RAM(TLS 上下文 + 收发缓冲 + 任务栈)。实测: 挂着 WS 时内部 RAM
             * 只剩 251 字节, 拉播放地址直接 "Failed to create socket" →
             * 整轮播放报废。关掉后退出音乐模式那一拍(下面)会自动重建。
             * (以前"保持连接"的顾虑是重建会内存搅动 —— 现在是【音乐开始前
             *  还有余量时】主动关, 时机完全不同, 且销毁流程复用下面的老路。) */
            if (zx.ws) {
                void *old = zx.ws;
                zx.ws = NULL;
                zx.ws_ready = false;
                /* 等在途回调走完再销毁(同"彻底重建"的时序, 防 LoadProhibited) */
                vTaskDelay(pdMS_TO_TICKS(1200));
                esp_websocket_client_destroy(old);
                ESP_LOGI(TAG, "音乐模式: WebSocket 已关闭释放内存 [内部RAM=%u]",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            }
        } else if (!mp && zx.music_mode) {
            zx.music_mode = false;
            ESP_LOGI(TAG, "★ 退出音乐模式 → AI 立刻恢复(暂停/放完/摸头顶 3 下) [内部RAM=%u]",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }

        bool need = zx.hard_reconnect || zx.pending_rebuild ||
                    (zx.ws && zx.stuck >= 12 && age_ms > 15000);

        /* ★★ 播音乐期间【绝不重建】WebSocket ★★
         * 音乐播放时内部 RAM 只剩 7KB 量级, 重建必然 "Error create websocket
         * task"; 而每次失败尝试都要分配/释放 8KB WS 缓冲 + TLS 上下文, 内存
         * 来回搅动会把音乐那条 TCP 一起拖死 —— 实测日志:
         *   178482 建 WS 前: 内部RAM最大连续块=4096 剩余=7375
         *   178482 websocket_client: Error create websocket task
         *   182742 music: 缓冲=0/24576      ← 音乐被拖空了
         *   184872 music: 流断了
         * 而且播音乐时 AI 音频本来就挂起(用户要的互斥), 这时有连接也没用。
         * 所以只把"要重建"记下来, 等音乐停了下一拍再建。 */
        /* ★ 闸门同样用 music_owns_audio(): 暂停期间 AI 要能重连
           (之前用 music_is_active(), 暂停时也不让重建 → 喊小智没反应) */
        if (need && music_owns_audio()) {
            if (!zx.pending_rebuild) {
                zx.pending_rebuild = true;
                ESP_LOGW(TAG, "音乐播放中 → WebSocket 暂不重建(避免内存搅动拖死音乐)");
            }
            need = false;
        } else if (need) {
            zx.pending_rebuild = false;
        }

        /* ★ 这两个标志都要无条件清掉(不管走不走重建)。
           之前写成 `if (need && zx.ws)` 才清, 那么当这一步进来时句柄已经是 NULL
           (刚销毁完)的话, 标志会一直为真 → 之后每拍都判定"需要重建"
           → 连接刚建起来就被销毁, 陷入死循环, 表现就是反复"连接中"却永远连不上。
           另外: force_reconnect(服务端关会话)只用于"尽快恢复聆听", 不再触发重建。 */
        zx.hard_reconnect  = false;
        zx.force_reconnect = false;

        if (need && zx.ws) {
            zx.stuck = 0;
            ESP_LOGW(TAG, "彻底重建 WebSocket (age=%ums, 假死/长时间未连上)",
                     (unsigned)age_ms);
            /* ★ 顺序很重要: 必须先摘掉句柄再销毁。
               如果直接 destroy(zx.ws) 然后才置 NULL, 那么在这两步之间,
               正在 WS 事件回调里跑的 zx_handle_json()(例如 MCP 分支里的
               esp_websocket_client_is_connected(zx.ws)) 会拿到已经释放的
               句柄 → LoadProhibited 崩溃。
               摘掉句柄后回调那边 `zx.ws &&` 直接为假, 再等一会儿让可能
               正在执行的回调(最多含一次 500ms 的发送)走完, 才真正销毁。 */
            void *old = zx.ws;
            zx.ws = NULL;
            zx.ws_ready = false;
            /* 等久一点更保险: 在途的回调最多含一次 500ms 的发送 */
            vTaskDelay(pdMS_TO_TICKS(1200));
            esp_websocket_client_destroy(old);
        }

        /* ★ 音乐模式下不建 WS(内存留给音乐); 等退出音乐模式那一拍再建 */
        if (zx.ws == NULL && !zx.music_mode) {
            zx.stuck = 0;
            zx_start_ws();
            /* ★★ 自愈: 内部堆【碎片化】后 WS 任务栈(5120)可能永远建不起来 ——
             *    总剩余看着够(11KB+)但最大连续块不够(4KB), 重试一万次也一样。
             *    实测症状: 日志反复 "Error create websocket task", 网页也失联,
             *    只能手动断电。连续 5 次失败 → 自动重启, 10 秒满血复活。 */
            static int ws_fail_cnt;
            if (zx.ws == NULL) {
                if (++ws_fail_cnt >= 5) {
                    ESP_LOGE(TAG, "WS 连续 %d 次建不起来(内部堆碎片化) → 自动重启自愈",
                             ws_fail_cnt);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    esp_restart();
                }
            } else {
                ws_fail_cnt = 0;
            }
            /* ★ 只有"真的建不起来"才动音乐 —— 这是最后手段, 很少走到。
               (zx_start_ws 建失败时 zx.ws 仍为 NULL 并已打过错误日志) */
            if (zx.ws == NULL && music_is_active()) {
                ESP_LOGW(TAG, "WS 建不起来(内部RAM连续块不足) → 停音乐腾内存后重试");
                music_stop();
                for (int i = 0; i < 40 && music_is_active(); i++) vTaskDelay(pdMS_TO_TICKS(50));
                vTaskDelay(pdMS_TO_TICKS(300));    /* 等解码器/Http 客户端彻底释放 */
                zx_start_ws();
            }
        } else if (esp_websocket_client_is_connected(zx.ws)) {
            zx.stuck = 0;
        }
        /* 1 秒一拍: 重连/续听的兜底以前要等 5 秒, 用户会觉得"迟钝"。
           这里不是忙循环(有延时), 不会额外占 CPU。 */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ======================= 公开 API ======================= */
void ai_client_init(void)
{
    memset(&zx, 0, sizeof(zx));
    zx.sb  = xStreamBufferCreate(AUDIO_SB_SIZE, 1);
    zx.txq = xQueueCreate(16, sizeof(char *));
    /* 噪声底初值取增益后的经验值(原始 ~500 × TX_GAIN)。
       给高点没关系: 自适应是"下降快(0.25)/上升慢(0.015)",
       安静环境下 0.5 秒内就会掉到真实水平; 给太低则要十几秒才爬上来,
       这期间会把房间噪声误判成"一直有人在说话", 导致不会自动停止聆听。 */
    zx.noise  = 1500.0f;
    build_identity();
    tts_voice_load();               /* 恢复上次选的播报音色 */

    /* 语音触发方式: 优先用 esp-sr 唤醒词「你好小智」(模型在 flash 的 model 分区);
     * 拿不到模型就回退到能量 VAD(任何说话都会触发, 灵敏度见 config.h 的 VAD_*) */
    zx.ww_ready  = wake_word_init();
    zx.vad_on    = !zx.ww_ready;
    zx.wake_beep = true;
    zx.cont_mode = true;           /* 默认连续对话: 答完自动重新聆听 */
    if (zx.ww_ready) {
        ESP_LOGI(TAG, "语音触发 = 唤醒词「你好小智」(%s), 说这四个字就开始说话",
                 wake_word_name());
    } else {
        ESP_LOGW(TAG, "唤醒词不可用 → 回退能量 VAD(免触摸, 但环境嘈杂时会误触发)");
    }
    strncpy(zx.ota_url, OTA_URL_DEFAULT, sizeof(zx.ota_url) - 1);
    /* 优先级 6 > 表情任务的 5: 这是语音设备, 音频(唤醒词/播报)必须优先,
       否则屏幕渲染一忙就会把音频挤到一边, 表现就是唤醒迟钝、播报卡顿。 */
    /* ★★★ 音频任务的栈放 PSRAM —— 内部 RAM 要留给 WebSocket 任务的栈 ★★★
     *
     * 这是整个系统里最大的一块内部 RAM 占用(28KB), 挪走之后内部 RAM 的
     * 最大连续块从 ~4KB 恢复到 28KB 量级, WebSocket 的 5KB 栈就永远能建出来。
     *
     * 为什么安全(PSRAM 栈的唯一禁忌是"任务自己触发 flash 写"):
     *   · zx_audio_task(869~1309 行)里【没有】任何 nvs 或 spi_flash 调用 —— 已逐行核对
     *   · 它只做: I2S 读写 / 唤醒词推理 / Opus 编解码 / 环形缓冲搬运 / 日志
     *   · 唤醒词模型是从 flash 只读映射的, 只读不算 flash 写, 不关 cache
     *   · 别的任务(如 httpd 写 NVS)做 flash 写时, IDF 会把本核挂起,
     *     不会让 PSRAM 栈的任务在 cache 关闭窗口里跑 —— 项目里 face 任务
     *     (8192) 和音乐读任务(4096) 都是这么放的, 已经长期稳定运行。
     * 真不放心的话: 创建失败会自动回退到内部 RAM(只是 WS 又可能建不出来)。
     */
    if (xTaskCreatePinnedToCoreWithCaps(zx_audio_task, "zx_audio", AUDIO_TASK_STACK,
                                        NULL, 6, &zx.audio_task, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "音频任务: PSRAM 栈创建失败, 回退内部 RAM(WS 可能建不起来)");
        xTaskCreatePinnedToCore(zx_audio_task, "zx_audio", AUDIO_TASK_STACK, NULL, 6,
                                &zx.audio_task, 1);
    } else {
        ESP_LOGI(TAG, "音频任务栈: PSRAM (腾出 28KB 内部 RAM 给 WebSocket)");
    }
    /* 这个任务里要跑 HTTPS + TLS(mbedTLS 握手约需 8~16KB 栈), 6144 会栈溢出崩溃 */
    /* 栈必须放内部 RAM —— 不能放 PSRAM:
       本任务会读写 NVS(flash 操作), flash 写入期间 cache 被关闭,
       此时访问 PSRAM 栈会立即 panic(实测 A1 落在 0x3c... 反复重启)。
       内部 RAM 不足的问题改由精简其他任务栈来解决。 */
    xTaskCreatePinnedToCore(zx_task, "zx", ZX_TASK_STACK, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "小智客户端已启动");
}

void ai_client_notify_online(bool online)
{
    if (online != zx.online) ESP_LOGI(TAG, "联网状态 -> %d", (int)online);
    zx.online = online;
}

bool ai_client_online(void) { return zx.online; }

void ai_client_start_listening(void)
{
    if (!zx.ws_ready) {
        ESP_LOGW(TAG, "还没连上小智服务, 无法说话 (state=%s)", zx.state);
        face_set_emotion_hold(FACE_ERROR, 1200);
        return;
    }
    /* 触摸触发: 由松手结束, 不走静音自动结束 */
    zx.vad_session = false;
    zx_begin_listen();
}

void ai_client_stop_listening(void)
{
    if (zx.mode != MODE_LISTEN) return;
    zx_send_listen("stop");
    zx.mode = MODE_IDLE;
    zx.vad_session = false;
    zx.silence_ms  = 0;
    set_state("思考中");
    face_set_emotion_hold(FACE_THINKING, 4000);
}

/* ---------- 下行链路自检 ----------
 * 自己用 24kHz 编码一段 440Hz 正弦(和真实服务端下行的参数一致),
 * 再经 zx_push_audio() 走「完全相同的」下行通路:
 *     分帧 → 播报态 → 按长度头取帧 → opus_decode → speaker_write
 * 听到 440Hz 提示音 + 日志里电平非 0 = 下行链路整条是通的,
 * 那么"没声音"就只可能出在上行/服务端。 */
void ai_client_selftest_tts(int seconds)
{
    int err = 0;
    OpusEncoder *e = opus_encoder_create(24000, 1, OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || e == NULL) {
        ESP_LOGE(TAG, "自检: 24kHz 编码器创建失败 %d", err);
        return;
    }
    const int N = 24000 * FRAME_MS / 1000;      /* 60ms @24kHz = 1440 样本 */
    static int16_t sbuf[1440];
    static uint8_t sout[OPUS_MAX_PKT];

    if (zx.sb) xStreamBufferReset(zx.sb);
    zx.mode = MODE_SPEAK;

    int frames = seconds * 1000 / FRAME_MS;
    int pushed = 0;
    for (int i = 0; i < frames; i++) {
        for (int k = 0; k < N; k++) {
            float t = (float)(i * N + k) / 24000.0f;
            sbuf[k] = (int16_t)(12000.0f * sinf(2.0f * 3.14159265f * 440.0f * t));
        }
        int len = opus_encode(e, sbuf, N, sout, sizeof(sout));
        if (len > 0) { zx_push_audio(sout, len); pushed++; }
    }
    opus_encoder_destroy(e);
    ESP_LOGI(TAG, "自检: 已推入 %d 帧 24kHz Opus(共 %dms) 到下行链路, "
                  "听到 440Hz 提示音即正常", pushed, seconds * 1000);
}

/* ---------- 唤醒路径自检 ----------
 * 完整走一遍「唤醒命中 → 提示音 → 上报唤醒词 → 开启上行 → 静音自动结束」,
 * 其中 wake_word_reset() 正是之前崩溃的那一步(会走到 esp-sr 的 model_clean
 * 并对空指针解引用)。这个自检用于确认该崩溃已彻底消除, 同时验证上行通路。 */
void ai_client_test_wake(const char *word)
{
    if (!word) word = "你好小智";
    ESP_LOGI(TAG, "自检: 模拟唤醒 —— 先调用曾经崩溃的 wake_word_reset()");
    wake_word_reset();
    ESP_LOGI(TAG, "自检: wake_word_reset() 正常返回, 未崩溃 ✓");

    if (zx.wake_beep) { speaker_beep(784, 70); speaker_beep(1046, 90); }
    ESP_LOGI(TAG, "自检: 提示音已播放");

    if (!zx.ws_ready) { ESP_LOGW(TAG, "自检: WS 未就绪, 跳过上行"); return; }
    zx_send_wake_detected(word);
    zx.vad_session = true;
    zx_begin_listen();
    uint32_t tx0 = ai_client_tx_frames();
    ESP_LOGI(TAG, "自检: 上行已开启(8 秒), 现在说话就会被服务端识别");
    vTaskDelay(pdMS_TO_TICKS(8000));
    ai_client_stop_listening();
    ESP_LOGI(TAG, "自检: 唤醒→上行→结束 全流程通过 ✓ (8 秒内发出上行音频 %u 帧)",
             (unsigned)(ai_client_tx_frames() - tx0));
}

bool ai_client_ws_ready(void) { return zx.ws_ready; }

/* ---------- MCP 工具执行链路自检 ----------
 * 用三条伪造的 tools/call 直接喂给 mcp_handle_payload(), 完全绕开服务端,
 * 专门验证「JSON-RPC 解析 → 工具分发 → 灯效/传感器动作」这一段是否真的能跑。
 * 若这段没问题, 那"AI 调不动灯"就只可能是服务端没把 tools/call 下发下来
 * (例如上行音频没送到, 模型压根没有输入)。 */
void ai_client_test_mcp(void)
{
    static const char *cases[] = {
        /* 1. 切灯效: 警车爆闪 —— 这一条应该能明显看到灯变 */
        "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":901,\"params\":"
        "{\"name\":\"self.light.set_effect\",\"arguments\":{\"effect\":\"警车爆闪\"}}}",
        /* 2. 调亮度 */
        "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":902,\"params\":"
        "{\"name\":\"self.light.set_brightness\",\"arguments\":{\"brightness\":80}}}",
        /* 3. 读温湿度 */
        "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":903,\"params\":"
        "{\"name\":\"self.sensor.get_temperature_humidity\",\"arguments\":{}}}",
        /* 4. 不存在的工具 —— 应回 JSON-RPC error */
        "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":904,\"params\":"
        "{\"name\":\"self.nope\",\"arguments\":{}}}",
    };
    for (int i = 0; i < 4; i++) {
        char *r = mcp_handle_payload(cases[i], "selftest");
        ESP_LOGI(TAG, "自检 MCP[%d] -> %s", i + 1, r ? r : "(无需回复)");
        free(r);
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
    /* 自检完恢复初始状态: 关灯(音量保持默认 70%, 别压成听不见) */
    light_set_effect(LIGHT_OFF);
    speaker_set_volume(70);
    ESP_LOGI(TAG, "自检结束, 已恢复: %s, 音量 %d%%",
             light_effect_cn(light_get_effect()), speaker_get_volume());
}

/* ---------- 免触摸 VAD 开关与标定 ---------- */
float ai_client_mic_rms(void) { return zx.mic_rms; }
bool  ai_client_get_vad(void) { return zx.vad_on; }

bool        ai_client_wake_ready(void) { return zx.ww_ready; }
const char *ai_client_wake_name(void)  { return zx.ww_ready ? wake_word_name() : "-"; }
bool        ai_client_get_wake_beep(void) { return zx.wake_beep; }
void        ai_client_set_wake_beep(bool on)
{
    zx.wake_beep = on;
    ESP_LOGI(TAG, "唤醒应答音 -> %s", on ? "开" : "关(排查杂音用)");
}

void ai_client_set_vad(bool on)
{
    zx.vad_on = on;
    zx.voice_frames = 0;
    ESP_LOGI(TAG, "免触摸对话(VAD) -> %s", on ? "开" : "关");
}

void ai_client_abort(void)
{
    if (!zx.ws_ready) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "abort");
    if (zx.session[0]) cJSON_AddStringToObject(o, "session_id", zx.session);
    zx_send_json(o);
    zx_force_idle();            /* 功放 hold 必须放; sb 清理由音频任务做 */
    set_state("就绪");
}

const char *ai_client_state_str(void) { return zx.state[0] ? zx.state : "启动中"; }
bool ai_client_is_busy(void) { return zx.mode == MODE_LISTEN || zx.mode == MODE_SPEAK; }

/* ======================= 音乐互斥 =======================
 * 播在线音乐时把整条 AI 音频链挂起, 音乐结束/暂停时恢复。
 * 详见 xiaozhi.h 里这个函数的说明。 */
void ai_client_set_audio_suspended(bool suspended)
{
    if (suspended == zx.audio_suspended) return;
    zx.audio_suspended = suspended;

    static int64_t s_susp_start_us;

    if (suspended) {
        s_susp_start_us = esp_timer_get_time();
        zx.suspend_log = 0;
        /* 停掉正在进行的聆听/播报, 清空缓冲 —— 否则恢复时会播上一轮的残渣。
           走 zx_force_idle: 放功放 hold + 挂清缓冲标志(本任务不是音频任务)。 */
        zx_force_idle();
        zx.cooldown_ms  = 0;
        zx.cont_wait_ms = 0;
        zx.want_listen  = false;
        ESP_LOGW(TAG, "AI 音频挂起 (播放音乐)");
    } else {
        /* ★★★ 挂起后的唤醒词引擎恢复: 看挂了多久 ★★★
         *
         * 短挂起(几百毫秒~几秒, 比如一次网易云请求): 清一下累积缓冲就行。
         * 长挂起(播歌几十秒): 只 reset 是【不够】的 —— 模型内部的特征队列还停在
         * 暂停前那一刻的旧音频上, 恢复后喂进去的声音在它看来是跳变, 唤醒率掉到
         * 几乎为 0。用户的感受就是【暂停音乐后, 怎么喊「你好小智」都没反应】
         * (实测踩到: 仪表全正常 —— 引擎在跑、麦克风有电平、循环 45ms,
         *  就是不命中)。所以超过 3 秒的长挂起直接重建引擎。 */
        if (zx.ww_ready) {
            int64_t gap_us = esp_timer_get_time() - s_susp_start_us;
            if (gap_us > 3 * 1000 * 1000) {
                /* ★★ 绝不能在这里直接 wake_word_restart() ★★
                 * 本函数是从音乐任务 / 网易云任务里调的, 而【音频任务】此刻可能正
                 * 在 wake_word_feed() 里跑模型。destroy 释放 s_data 的同时那边还在用
                 * → use-after-free, 实测直接崩:
                 *   dl_convq_queue_pop ← model_detect_samples ← wake_word_feed
                 *   Guru Meditation (LoadProhibited) → 设备重启
                 * 所以只打一个标记, 交给音频任务自己(唯一会碰引擎的任务)重建。 */
                ESP_LOGW(TAG, "挂起持续 %lldms → 请音频任务重建唤醒词引擎", gap_us / 1000);
                zx.ww_restart_req = true;
            } else {
                wake_word_reset();
            }
        }
        zx.audio_suspended = false;
        zx.suspend_log     = 0;
        zx.noise           = 1500.0f;   /* 噪声底重新估计 */
        zx.cooldown_ms     = 800;       /* 稍等一拍再开始听, 避开音乐的尾音 */
        ESP_LOGW(TAG, "AI 音频已恢复 (音乐结束)");
    }
}

bool ai_client_get_audio_suspended(void) { return zx.audio_suspended; }
bool ai_client_is_speaking(void) { return zx.mode == MODE_SPEAK; }
const char *ai_client_activation_code(void) { return zx.act_code; }
uint32_t ai_client_tx_frames(void) { return zx.tx; }
uint32_t ai_client_rx_frames(void) { return zx.rx; }

void ai_client_set_server(const char *ota_url, const char *ws_url, const char *token)
{
    if (ota_url) { strncpy(zx.ota_url, ota_url, sizeof(zx.ota_url) - 1); nvs_str_set("ota_url", ota_url); }
    if (ws_url)  { strncpy(zx.ws_url, ws_url, sizeof(zx.ws_url) - 1);  nvs_str_set("ws_url", ws_url); }
    if (token)   { strncpy(zx.ws_token, token, sizeof(zx.ws_token) - 1); nvs_str_set("ws_token", token); }
    ESP_LOGI(TAG, "服务器已改为: ota=%s ws=%s", zx.ota_url, zx.ws_url);
}
