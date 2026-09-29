/*
 * ne_client.c — 网易云音乐直连客户端实现
 *
 * 加密算法/常量与网页端一致(公开知识), 已在设备上实测通过:
 *   weapi = base64(AES128CBC(base64(AES128CBC(json,预设密钥)), 随机密钥))
 *           + encSecKey = hex(RSA_raw(反序(随机密钥)左补零到128字节, E=65537))
 *   eapi  = hex(AES128ECB(url-36cd479b6b5-json-36cd479b6b5-md5(nobody url use json md5forencrypt)))
 */
#include "ne_client.h"
#include "xiaozhi.h"        /* 音乐互斥相关(灯效/挂起状态查询) */
#include "music.h"          /* music_is_playing(): 播歌时不要打网络(会互相拖死) */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/md5.h"
#include "mbedtls/bignum.h"
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ne";

static const char    NE_PRESET_KEY[] = "0CoJUm6Qyw8W8jud";
static const uint8_t NE_IV[16] = {'0','1','0','2','0','3','0','4','0','5','0','6','0','7','0','8'};
static const char    NE_BASE62[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
static const char    NE_EAPI_KEY[] = "e82ckenh8dichen8";
static const uint8_t NE_RSA_N[128] = {
    0xe0, 0xb5, 0x09, 0xf6, 0x25, 0x9d, 0xf8, 0x64, 0x2d, 0xbc, 0x35, 0x66,
    0x29, 0x01, 0x47, 0x7d, 0xf2, 0x26, 0x77, 0xec, 0x15, 0x2b, 0x5f, 0xf6,
    0x8a, 0xce, 0x61, 0x5b, 0xb7, 0xb7, 0x25, 0x15, 0x2b, 0x3a, 0xb1, 0x7a,
    0x87, 0x6a, 0xea, 0x8a, 0x5a, 0xa7, 0x6d, 0x2e, 0x41, 0x76, 0x29, 0xec,
    0x4e, 0xe3, 0x41, 0xf5, 0x61, 0x35, 0xfc, 0xcf, 0x69, 0x52, 0x80, 0x10,
    0x4e, 0x03, 0x12, 0xec, 0xbd, 0xa9, 0x25, 0x57, 0xc9, 0x38, 0x70, 0x11,
    0x4a, 0xf6, 0xc9, 0xd0, 0x5c, 0x4f, 0x7f, 0x0c, 0x36, 0x85, 0xb7, 0xa4,
    0x6b, 0xee, 0x25, 0x59, 0x32, 0x57, 0x5c, 0xce, 0x10, 0xb4, 0x24, 0xd8,
    0x13, 0xcf, 0xe4, 0x87, 0x5d, 0x3e, 0x82, 0x04, 0x7b, 0x97, 0xdd, 0xef,
    0x52, 0x74, 0x1d, 0x54, 0x6b, 0x8e, 0x28, 0x9d, 0xc6, 0x93, 0x5b, 0x3e,
    0xce, 0x04, 0x62, 0xdb, 0x0a, 0x22, 0xb8, 0xe7,
};
static const char NE_BASE_COOKIE[] =
    "os=pc; appver=2.9.7; channel=netease; osver=Microsoft-Windows-10--build-22631-64bit";
static const char NE_UA[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36";

/* ================= 加密实现 ================= */
static int aes_b64(const uint8_t *in, size_t in_len, const uint8_t *key,
                   int cbc, char *out, size_t out_sz)
{
    uint8_t *dst = heap_caps_malloc(in_len + 32, MALLOC_CAP_SPIRAM);
    if (!dst) return -1;
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int r;
    if (cbc) {
        size_t pad = 16 - (in_len % 16);
        size_t tot = in_len + pad;
        uint8_t *src = heap_caps_malloc(tot, MALLOC_CAP_SPIRAM);
        if (!src) { free(dst); mbedtls_aes_free(&aes); return -1; }
        memcpy(src, in, in_len);
        memset(src + in_len, (int)pad, pad);
        uint8_t iv[16];
        memcpy(iv, NE_IV, 16);
        r = mbedtls_aes_setkey_enc(&aes, key, 128);
        if (r == 0) r = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, tot, iv, src, dst);
        free(src);
        in_len = tot;
    } else {
        r = mbedtls_aes_setkey_enc(&aes, key, 128);
        for (size_t i = 0; i + 16 <= in_len && r == 0; i += 16)
            r = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in + i, dst + i);
    }
    mbedtls_aes_free(&aes);
    if (r != 0) { free(dst); return -2; }
    size_t olen = 0;
    r = mbedtls_base64_encode((unsigned char *)out, out_sz, &olen, dst, in_len);
    free(dst);
    return (r == 0) ? (int)olen : -3;
}

/* RSA 裸加密(无填充): out = in^65537 mod N */
static bool rsa_raw_hex(const uint8_t in[128], char out[257])
{
    mbedtls_mpi N, E, M, C;
    mbedtls_mpi_init(&N); mbedtls_mpi_init(&E);
    mbedtls_mpi_init(&M); mbedtls_mpi_init(&C);
    int r = mbedtls_mpi_read_binary(&N, NE_RSA_N, 128);
    if (r == 0) r = mbedtls_mpi_lset(&E, 65537);
    if (r == 0) r = mbedtls_mpi_read_binary(&M, in, 128);
    if (r == 0) r = mbedtls_mpi_exp_mod(&C, &M, &E, &N, NULL);
    uint8_t bin[128];
    if (r == 0) r = mbedtls_mpi_write_binary(&C, bin, 128);
    mbedtls_mpi_free(&N); mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&M); mbedtls_mpi_free(&C);
    if (r != 0) return false;
    static const char HX[] = "0123456789abcdef";
    for (int i = 0; i < 128; i++) {
        out[i * 2]     = HX[bin[i] >> 4];
        out[i * 2 + 1] = HX[bin[i] & 0xF];
    }
    out[256] = '\0';
    return true;
}

static void ne_urlenc(const char *s, char *out, size_t out_sz)
{
    size_t o = 0;
    static const char H[] = "0123456789ABCDEF";
    for (; *s && o + 4 < out_sz; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = H[c >> 4]; out[o++] = H[c & 0xF]; }
    }
    out[o] = '\0';
}

static bool weapi_body(const char *json, char *body, size_t body_sz)
{
    uint8_t sk[16];
    for (int i = 0; i < 16; i++) sk[i] = (uint8_t)NE_BASE62[esp_random() % 62];
    char step1[1024], params[1024];
    if (aes_b64((const uint8_t *)json, strlen(json), (const uint8_t *)NE_PRESET_KEY, 1, step1, sizeof(step1)) < 0) return false;
    if (aes_b64((const uint8_t *)step1, strlen(step1), sk, 1, params, sizeof(params)) < 0) return false;
    uint8_t rev[128] = {0};
    for (int i = 0; i < 16; i++) rev[112 + i] = sk[15 - i];
    char enc[257];
    if (!rsa_raw_hex(rev, enc)) return false;
    char ep[1400], ee[600];
    ne_urlenc(params, ep, sizeof(ep));
    ne_urlenc(enc, ee, sizeof(ee));
    snprintf(body, body_sz, "params=%s&encSecKey=%s", ep, ee);
    return true;
}

/* eapi 表单体(AES-ECB, 取播放地址用) */
static bool eapi_body(const char *path, const char *json, char *body, size_t body_sz)
{
    char msg[1500], digest[33];
    snprintf(msg, sizeof(msg), "nobody%suse%smd5forencrypt", path, json);
    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    mbedtls_md5_starts(&md5);
    mbedtls_md5_update(&md5, (const unsigned char *)msg, strlen(msg));
    unsigned char o16[16];
    mbedtls_md5_finish(&md5, o16);
    mbedtls_md5_free(&md5);
    for (int i = 0; i < 16; i++) sprintf(digest + i * 2, "%02x", o16[i]);

    int need = (int)strlen(path) + (int)strlen(json) + 96;
    char *data = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!data) return false;
    int n = snprintf(data, need, "%s-36cd479b6b5-%s-36cd479b6b5-%s", path, json, digest);
    /* ★★★ AES-ECB 必须用 PKCS7 填充, 不是补 0! ★★★
     *
     * 网易云的 eapi 用 PKCS7 —— 每个填充字节的值等于"填充了几个字节"
     * (补 5 字节就是 0x05 0x05 0x05 0x05 0x05; 正好整块时补满 16 个 0x10)。
     * 以前这里补的是 0x00, 网易云解开后尾部是乱码 → 回
     *     {"msg":"参数错误","code":400}
     * 表现: 搜索正常、登录正常, 但【每首歌都取不到播放地址】。 */
    int pad = 16 - (n % 16);
    memset(data + n, pad, pad);
    n += pad;

    uint8_t *enc = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!enc) { free(data); return false; }
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int r = mbedtls_aes_setkey_enc(&aes, (const uint8_t *)NE_EAPI_KEY, 128);
    for (int i = 0; i + 16 <= n && r == 0; i += 16)
        r = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, (const uint8_t *)data + i, enc + i);
    mbedtls_aes_free(&aes);
    free(data);
    if (r != 0) { free(enc); return false; }
    static const char HX[] = "0123456789ABCDEF";
    char *p = body;
    memcpy(p, "params=", 7);
    p += 7;
    for (int i = 0; i < n; i++) {
        *p++ = HX[enc[i] >> 4];
        *p++ = HX[enc[i] & 0xF];
    }
    *p = 0;
    free(enc);
    return true;
}

/* ================= Cookie(登录态) =================
 *
 * ★★★ 这里必须【整条原样保存】, 不能拆成"键=值"再限量 ★★★
 *
 * 曾经的写法是 `struct { char k[28]; char v[224]; } s_ck[16]`, 并且
 *     if (n <= 1 || n >= 250) return;      // 单条超过 250 字符 → 直接丢弃
 * —— 而网易云的 MUSIC_U 值有【1030 个字符】! 于是它每次都被静默丢掉:
 * 扫码明明授权成功了(803), 设备却始终显示未登录, 因为判断登录的依据
 * ne_client_logged_in() 找不到 MUSIC_U。粘贴整条 Cookie 也一样会丢。
 * (想通这点后就能解释"手机说登录成功、设备还显示等待扫码"这类怪象。)
 *
 * 现在: 把整条 Cookie 当成一个字符串存(它本来就是个 HTTP 请求头),
 * 只限制总长度 NE_CK_STR。 */
#define NE_CK_STR 3072
static char s_ck_raw[NE_CK_STR];
static char s_nick[48];

/* Set-Cookie 里的这些是属性, 不是 cookie 值, 不要带进请求头 */
static bool ck_is_attr(const char *s, size_t n)
{
    static const char *ATTR[] = { "Path", "Domain", "Expires", "Max-Age",
                                  "Secure", "HttpOnly", "SameSite", "Version", "Comment" };
    for (int i = 0; i < 9; i++) {
        size_t L = strlen(ATTR[i]);
        if (n == L && strncmp(s, ATTR[i], L) == 0) return true;
        if (n > L && strncmp(s, ATTR[i], L) == 0 && s[L] == '=') return true;
    }
    return false;
}

/* 加入一条 "名=值"(同名会替换)。line 可以是整行 Set-Cookie, 也可以是 "a=1"
 * ★ 全程在临界区里: 音乐任务(搜索响应)和登录轮询任务(扫码响应)都可能带着
 *   Set-Cookie 并发进来, static tmp 和 s_ck_raw 会被交叉写坏 —— 而 Cookie
 *   正是登录判据, 坏了就"明明登录了却当未登录"。全是纯内存操作, 关中断极短。 */
static portMUX_TYPE s_ck_mux = portMUX_INITIALIZER_UNLOCKED;

static void ck_set_locked(const char *line)
{
    while (*line == ' ') line++;
    const char *semi = strchr(line, ';');
    size_t n = semi ? (size_t)(semi - line) : strlen(line);
    while (n && line[n - 1] == ' ') n--;
    if (n < 3) return;
    const char *eq = memchr(line, '=', n);
    if (!eq || eq == line) return;
    size_t klen = (size_t)(eq - line);
    if (klen > 40) return;
    if (ck_is_attr(line, klen)) return;

    /* 重建一遍: 跳过同名旧字段, 再追加新的 —— 这样重复登录/重复粘贴不会堆积 */
    static char tmp[NE_CK_STR];
    size_t tl = 0;
    const char *p = s_ck_raw;
    while (*p) {
        const char *s2 = strchr(p, ';');
        size_t L = s2 ? (size_t)(s2 - p) : strlen(p);
        const char *e2 = memchr(p, '=', L);
        bool same = e2 && (size_t)(e2 - p) == klen && strncmp(p, line, klen) == 0;
        if (!same && L) {
            if (tl + L + 3 < sizeof(tmp)) {
                if (tl) { tmp[tl++] = ';'; tmp[tl++] = ' '; }
                memcpy(tmp + tl, p, L); tl += L;
            }
        }
        p = s2 ? s2 + 1 : p + L;
        while (*p == ' ') p++;
    }
    tmp[tl] = 0;

    if (tl + n + 3 >= NE_CK_STR) {
        ESP_LOGW(TAG, "Cookie 串已满(%d/%d), 丢弃字段 %.20s", (int)tl, NE_CK_STR, line);
        return;
    }
    char *dst = s_ck_raw;
    memcpy(dst, tmp, tl);
    if (tl) { dst[tl++] = ';'; dst[tl++] = ' '; }
    memcpy(dst + tl, line, n);
    dst[tl + n] = 0;
}

static void ck_set(const char *line)
{
    /* 包裹式加锁: 锁内函数有多个提前 return, 绝不能把 ENTER/EXIT 放进它
       本体(某个 return 漏了 EXIT 就死锁 —— 差点自己踩)。 */
    taskENTER_CRITICAL(&s_ck_mux);
    ck_set_locked(line);
    taskEXIT_CRITICAL(&s_ck_mux);
}

static void ck_build(char *out, size_t sz)
{
    snprintf(out, sz, "%s%s%s", NE_BASE_COOKIE, s_ck_raw[0] ? "; " : "", s_ck_raw);
}

/* ★★★ 落盘必须交给"内部 RAM 栈"的任务去做 ★★★
 *
 * 写 NVS 属于 flash 操作, 而 flash 操作会【关闭 cache】; 栈在 PSRAM 的任务在
 * cache 关闭期间没法访问自己的栈, 于是 IDF 直接断言重启:
 *     assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
 *                 (esp_task_stack_is_sane_cache_disabled())
 *     rst:0xc (RTC_SW_CPU_RST)          ← 实测反复重启
 * 而登录轮询任务(栈在 PSRAM, 为了省内部 RAM)正好会在开机校验和扫码成功后落盘,
 * 所以这里改成: 只打个标记, 由 ck_writer_task(内部 RAM 栈, 4KB)去写。
 * (这也顺便解释了"扫码授权成功却没登录上"—— 以前写 NVS 会把它重启掉。) */
static volatile bool s_ck_need_save;

/* ★★★ 落盘必须由【已经存在的、栈在内部 RAM 的任务】来做 ★★★
 *
 * 踩过两个坑:
 *   ① 常驻一个 4KB 内部栈的落盘任务 → 永久占掉 4KB 内部 RAM, 直接把
 *      WebSocket 挤死(WS 要 7KB 连续块): "Error create websocket task"。
 *   ② 改成"用时才建"的一次性任务 → 建的时候内部 RAM 又不够:
 *         E ne: Cookie 落盘任务创建失败(内部 RAM 不足)
 *      于是 Cookie 根本没存下来, 【一重启就丢登录态】。
 * 现在: 只打一个标记, 由 AI 主任务 zx(栈在内部 RAM, 每秒循环一次)看到标记后
 * 顺手写 NVS —— 不额外占一个字节, 也不会失败。 */
bool ne_cookie_save_pending(void) { return s_ck_need_save; }

/* ★ 只能由"栈在内部 RAM"的任务调用(写 flash 会关 cache, PSRAM 栈的任务
   在关 cache 期间没法访问自己的栈 → 断言重启: esp_task_stack_is_sane_cache_disabled) */
void ne_cookie_save_now(void)
{
    if (!s_ck_need_save) return;
    s_ck_need_save = false;
    nvs_handle_t h;
    if (nvs_open("nemusic", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "cookie", s_ck_raw);
        nvs_set_str(h, "nick", s_nick);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "登录态已保存到 NVS (%d 字节, 重开也不丢)", (int)strlen(s_ck_raw));
    }
}

static void ck_save(void)
{
    s_ck_need_save = true;      /* 真正写由 zx 任务去做(见上) */
}

/* 前向声明: 校验函数要用它, 但它的定义在下面 */
static bool call_weapi(const char *path, const char *json, char *out, size_t out_sz, int *out_len);

bool ne_client_logged_in(void)
{
    /* ★ 用整串查找, 不再依赖"键值表"(那套会丢掉超长的 MUSIC_U) */
    return strstr(s_ck_raw, "MUSIC_U=") != NULL;
}

/* ---- Cookie 有效性(去网易云问, 不猜) ----
 * -1 = 还没验过, 0 = 无效/已过期, 1 = 有效
 *
 * 为什么必须验: 以前"登录=字符串里有 MUSIC_U", 于是【一张过期的 Cookie 也让
 * 网页显示"已登录"】, 然后搜歌、取播放地址全部失败, 用户完全摸不着头脑
 * (实测就踩了这个: 网页写着"已登录: 网易云用户", 实际网易云那边是未登录)。 */
static volatile int s_ck_ok = -1;
static TickType_t s_last_verify;          /* 校验节流(网络不通时别死循环重试) */
int ne_client_cookie_ok(void) { return s_ck_ok; }

bool ne_client_verify(char *nick_out, size_t nick_sz)
{
    if (!ne_client_logged_in()) { s_ck_ok = 0; return false; }
    char *resp = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!resp) return false;
    int n = 0;
    bool ok = call_weapi("/weapi/nuser/account/get", "{}", resp, 4096, &n);
    if (!ok) {
        /* ★ 网络没通(例如开机时 WiFi 还没连上, 实测会报 getaddrinfo() returns 202)
           → 不能判定 Cookie 失效! 保持"未知(-1)", 过一会儿再验。 */
        free(resp);
        ESP_LOGW(TAG, "Cookie 校验: 网络不通, 稍后重试(暂不改变登录状态)");
        return false;
    }
    bool good = false;
    {
        cJSON *root = cJSON_Parse(resp);
        if (root) {
            cJSON *acc = cJSON_GetObjectItem(root, "account");
            cJSON *pf  = cJSON_GetObjectItem(root, "profile");
            cJSON *nm  = pf ? cJSON_GetObjectItem(pf, "nickname") : NULL;
            if (acc && !cJSON_IsNull(acc)) {
                good = true;
                if (cJSON_IsString(nm)) snprintf(s_nick, sizeof(s_nick), "%s", nm->valuestring);
            }
            cJSON_Delete(root);
        }
    }
    free(resp);
    s_ck_ok = good ? 1 : 0;
    if (good) {
        ck_save();               /* 顺手把昵称一起存下来 */
        ESP_LOGW(TAG, "★ Cookie 校验通过: 昵称=%s", s_nick);
    } else {
        ESP_LOGW(TAG, "★ Cookie 校验失败 —— 已过期或无效, 请重新登录");
    }
    if (nick_out && nick_sz) snprintf(nick_out, nick_sz, "%s", s_nick);
    return good;
}

const char *ne_client_nick(void) { return s_nick; }

void ne_login_logout(void)
{
    s_ck_raw[0] = 0;
    s_nick[0] = 0;
    s_ck_ok = -1;
    nvs_handle_t h;
    if (nvs_open("nemusic", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "已退出网易云登录");
}

void ne_client_init(void)
{
    nvs_handle_t h;

    /* ★★★ 曾经这里写的是 `if (nvs_open(...) != ESP_OK) return;` —— 一个致命的小坑 ★★★
     *
     * "nemusic" 这个 NVS 命名空间只有【登录过】才会存在。所以从没登录过的设备
     * (也就是所有新用户!) 会在这里直接 return, 后面的 ne_login_worker_start()
     * 永远执行不到 —— 登录轮询任务根本没被创建, 页面上的二维码自然永远是死的:
     *     扫码授权成功 → 设备压根没在查状态 → 一直显示"等待扫码"
     * 更隐蔽的是: 手动点一下网页上的「扫码登录」会走另一个入口把任务建起来,
     * 于是表现为"有时候能用、重启之后就不能用了"。
     * 现在: 没登录过也要把轮询任务建起来。 */
    if (nvs_open("nemusic", NVS_READONLY, &h) == ESP_OK) {
        /* ★ 缓冲必须和存储上限一样大! 以前是栈上 1600 字节, 而 ck_set 允许
           Cookie 累积到 NE_CK_STR(3072) —— 超过后 nvs_get_str 返回
           ESP_ERR_NVS_INVALID_LENGTH 且【不写入】, 整条 Cookie 静默丢失,
           表现就是"重启之后登录态没了"(网易 MUSIC_U 一条就 1030 字符)。
           放 PSRAM 堆上, 顺带把 1.6KB 从调用栈上挪走。 */
        char *buf = heap_caps_malloc(NE_CK_STR, MALLOC_CAP_SPIRAM);
        size_t n;
        if (buf) {
            n = NE_CK_STR;
            if (nvs_get_str(h, "cookie", buf, &n) == ESP_OK && buf[0]) {
                char *save = NULL;
                for (char *t = strtok_r(buf, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
                    while (*t == ' ') t++;
                    ck_set(t);
                }
            }
            free(buf);
        }
        n = sizeof(s_nick);
        if (nvs_get_str(h, "nick", s_nick, &n) != ESP_OK) s_nick[0] = 0;
        nvs_close(h);
    } else {
        s_nick[0] = 0;
    }
    ESP_LOGI(TAG, "直连客户端就绪: 已登录=%d 昵称=%s",
             (int)ne_client_logged_in(), s_nick[0] ? s_nick : "(未登录)");

    /* 扫码登录的后台轮询任务: 无论有没有登录过, 开机都要建起来。
       注意这里【不再】开机创建落盘任务 —— 它是按需创建的一次性任务,
       常驻会永久占 4KB 内部 RAM 把 WebSocket 挤死(见 ck_writer_task 注释)。 */
    ne_login_worker_start();
}

/* ★★★ 解析网易云返回的 JSON 前必须调这个 ★★★
 *
 * 网易云会在字符串里塞【未转义的控制字符】—— 最常见的是歌单描述: 用户自己在
 * APP 里敲了回车, 服务端就原样把 0x0A 发了出来。而标准 JSON 规定字符串内
 * 不允许出现 0x00~0x1F, cJSON 会因此判定整个文档解析失败:
 *     收到 63387 字节, 卡在偏移 62769   ← 位置正好在 "description":"... 里面
 * 表现就是"接口 HTTP 200、数据也拿到了, 歌单却永远是 0 个"。
 *
 * 把控制字符替换成空格: 在字符串里是普通空格(完全没问题), 在字符串外本来就
 * 是可忽略的空白, 所以替换后 JSON 依然等价且合法。 */
static void json_sanitize(char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20) *s = ' ';
    }
}

/* ================= 轻量"歌单列表"提取器 =================
 *
 * 为什么不用 cJSON: 网易云的 /weapi/user/playlist 会把这个账号【全部】歌单的完整
 * 元数据返回(实测这个账号 200+ 个, 响应超过 256KB —— 给多大缓冲就能填满多大),
 * 而且 limit 参数被服务端忽略。cJSON 要把整份文档建成节点树(约 4~8 倍内存),
 * 设备上完全不现实。而我们只需要每个歌单的 id / 名字 / 歌数, 所以直接扫原文,
 * 边扫边丢 —— 内存只用几个小结构。
 *
 * (曾经的三连坑, 记录在此: 16KB 缓冲被截断 → 控制字符让 cJSON 直接失败 →
 *  改大缓冲到 256KB 后才发现体积根本不是"稍微超一点", 而是根本没上限。) */
typedef struct {
    char id[24];
    char name[200];
    int  count;
} ne_pl_item_t;

static const char *js_ws(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    return s;
}

/* 解析一个 JSON 字符串(含转义, 含 \uXXXX), 写进 out; 返回结束引号之后的位置 */
static const char *js_str(const char *s, char *out, size_t out_sz)
{
    if (*s != '"') return NULL;
    s++;
    size_t o = 0;
    while (*s && *s != '"') {
        unsigned int c = (unsigned char)*s++;
        if (c == '\\' && *s) {
            char e = *s++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': {                       /* \uXXXX(含中文) → 转成 UTF-8 */
                    unsigned int v = 0;
                    int k = 0;
                    for (; k < 4 && s[k]; k++) {
                        char h = s[k];
                        v <<= 4;
                        if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                    }
                    s += k;
                    /* ★ UTF-16 代理对合并: 歌名/歌单名里的 emoji 是
                       "\uD83D\uDE00" 这种高低对, 不合并会拆成两个非法的
                       3 字节序列(CESU-8), 网页显示乱码。 */
                    if (v >= 0xD800 && v <= 0xDBFF && s[0] == '\\' && s[1] == 'u') {
                        unsigned int lo = 0;
                        int k2 = 0;
                        for (; k2 < 4 && s[k2 + 2]; k2++) {
                            char h2 = s[k2 + 2];
                            lo <<= 4;
                            if (h2 >= '0' && h2 <= '9') lo |= (unsigned)(h2 - '0');
                            else if (h2 >= 'a' && h2 <= 'f') lo |= (unsigned)(h2 - 'a' + 10);
                            else if (h2 >= 'A' && h2 <= 'F') lo |= (unsigned)(h2 - 'A' + 10);
                        }
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            s += 2 + k2;
                            v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                        }
                    }
                    if (v < 0x80) {
                        if (o + 1 < out_sz) out[o++] = (char)v;
                    } else if (v < 0x800) {
                        if (o + 2 < out_sz) { out[o++] = (char)(0xC0 | (v >> 6)); out[o++] = (char)(0x80 | (v & 63)); }
                    } else if (v < 0x10000) {
                        if (o + 3 < out_sz) {
                            out[o++] = (char)(0xE0 | (v >> 12));
                            out[o++] = (char)(0x80 | ((v >> 6) & 63));
                            out[o++] = (char)(0x80 | (v & 63));
                        }
                    } else {
                        if (o + 4 < out_sz) {
                            out[o++] = (char)(0xF0 | (v >> 18));
                            out[o++] = (char)(0x80 | ((v >> 12) & 63));
                            out[o++] = (char)(0x80 | ((v >> 6) & 63));
                            out[o++] = (char)(0x80 | (v & 63));
                        }
                    }
                    continue;
                }
                default: c = (unsigned int)(unsigned char)e; break;   /* \" \\ \/ 等 */
            }
        }
        if (o + 1 < out_sz) out[o++] = (char)c;
    }
    if (*s != '"') return NULL;
    out[o] = 0;
    return s + 1;
}

/* 从 "playlist":[...] 里逐个抠出 {id, name, trackCount}, 最多 max 个 */
static int ne_extract_playlists(const char *json, ne_pl_item_t *items, int max)
{
    const char *p = strstr(json, "\"playlist\"");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;

    int cnt = 0;
    while (*p && cnt < max) {
        p = js_ws(p);
        if (*p == ']') break;
        if (*p != '{') { p++; continue; }

        ne_pl_item_t it;
        it.id[0] = 0; it.name[0] = 0; it.count = 0;

        int depth = 0;
        const char *q = p;
        while (*q) {
            char c = *q;
            if (c == '"') {                     /* 可能是一个 key */
                char key[48];
                const char *after = js_str(q, key, sizeof(key));
                if (!after) break;
                const char *v = js_ws(after);
                if (depth == 1 && *v == ':') {  /* 只认这一个对象自己那层的字段 */
                    v = js_ws(v + 1);
                    if (strcmp(key, "trackCount") == 0) {
                        it.count = atoi(v);
                    } else if (strcmp(key, "name") == 0 && it.name[0] == 0 && *v == '"') {
                        js_str(v, it.name, sizeof(it.name));
                    } else if (strcmp(key, "id") == 0 && it.id[0] == 0) {
                        /* ★ id 要按【原样文本】收: 网易云的 id 有的有 18 位,
                           用 int/double 装会丢精度(踩过), 直接抄数字最保险 */
                        size_t n = 0;
                        if (*v == '-') v++;
                        while (*v >= '0' && *v <= '9' && n + 1 < sizeof(it.id)) it.id[n++] = *v++;
                        it.id[n] = 0;
                    }
                }
                q = after;
                continue;
            }
            if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') {
                depth--;
                if (depth <= 0) { q++; break; }   /* 这个歌单对象结束 */
            }
            q++;
        }
        if (it.id[0]) items[cnt++] = it;

        p = q;
        while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    }
    return cnt;
}

/* ---- 歌单详情: 同样扫原文(原因同上: 1272 首的歌单详情有几百 KB, 还常被截断,
 *      cJSON 碰到不完整的 JSON 直接返回 NULL → "装进 0 首") ---- */
typedef struct {
    char id[24];
    char name[160];
    char artist[120];
} ne_tr_item_t;

/* array_key: 要扫描的数组字段名 —— 歌单详情是 "tracks", 歌手热门歌是 "hotSongs"。
 * plname 仅在 tracks(歌单详情) 时提取(playlist 对象名), 其它数组传 NULL/0 即可。 */
static int ne_extract_tracks(const char *json, const char *array_key, char *plname, size_t plname_sz,
                             ne_tr_item_t *items, int max)
{
    if (plname && plname_sz) plname[0] = 0;

    /* ① 歌单名: playlist 对象自己那层(depth==1)的 "name" */
    const char *p = (strcmp(array_key, "tracks") == 0) ? strstr(json, "\"playlist\"") : NULL;
    if (p) {
        p = strchr(p, '{');
        if (p) {
            int depth = 0;
            const char *q = p;
            while (*q) {
                if (*q == '"') {
                    char key[48];
                    const char *after = js_str(q, key, sizeof(key));
                    if (!after) break;
                    const char *v = js_ws(after);
                    if (depth == 1 && *v == ':' && strcmp(key, "name") == 0) {
                        v = js_ws(v + 1);
                        if (*v == '"') js_str(v, plname, plname_sz);
                        break;
                    }
                    q = after;
                    continue;
                }
                if (*q == '{' || *q == '[') depth++;
                else if (*q == '}' || *q == ']') { depth--; if (depth <= 0) break; }
                q++;
            }
        }
    }

    /* ② 逐首抠 <array_key>[] */
    char keypat[24];
    snprintf(keypat, sizeof(keypat), "\"%s\"", array_key);
    const char *tr = strstr(json, keypat);
    if (!tr) return 0;
    tr = strchr(tr, '[');
    if (!tr) return 0;
    tr++;

    int cnt = 0;
    while (*tr && cnt < max) {
        tr = js_ws(tr);
        if (*tr == ']') break;
        if (*tr != '{') { tr++; continue; }

        ne_tr_item_t it;
        it.id[0] = 0; it.name[0] = 0; it.artist[0] = 0;

        int depth = 0;
        const char *q = tr;
        while (*q) {
            if (*q == '"') {
                char key[48];
                const char *after = js_str(q, key, sizeof(key));
                if (!after) break;
                const char *v = js_ws(after);
                if (depth == 1 && *v == ':') {
                    v = js_ws(v + 1);
                    if (strcmp(key, "id") == 0 && it.id[0] == 0) {
                        size_t k = 0;
                        while (*v >= '0' && *v <= '9' && k + 1 < sizeof(it.id)) it.id[k++] = *v++;
                        it.id[k] = 0;
                    } else if (strcmp(key, "name") == 0 && it.name[0] == 0 && *v == '"') {
                        js_str(v, it.name, sizeof(it.name));
                    } else if (strcmp(key, "ar") == 0 && it.artist[0] == 0 && *v == '[') {
                        /* 第一个歌手对象里的 name */
                        const char *a = strstr(v, "\"name\"");
                        if (a) {
                            a = js_ws(a + 6);
                            if (*a == ':') { a = js_ws(a + 1); if (*a == '"') js_str(a, it.artist, sizeof(it.artist)); }
                        }
                    }
                }
                q = after;
                continue;
            }
            if (*q == '{' || *q == '[') depth++;
            else if (*q == '}' || *q == ']') { depth--; if (depth <= 0) { q++; break; } }
            q++;
        }
        /* depth>0 → 这个对象没结束(响应正好在这里被截断), 别再往下扫垃圾 */
        bool complete = (depth <= 0);
        if (it.id[0]) items[cnt++] = it;
        if (!complete) break;

        tr = q;
        while (*tr == ',' || *tr == ' ' || *tr == '\n' || *tr == '\r' || *tr == '\t') tr++;
    }
    return cnt;
}

/* ================= HTTPS 收发 ================= */
/* ★ 提前掐断: 歌单列表/歌单详情的响应都是几百 KB, 而我们只需要开头的一小段
   (前 20 个歌单 / 前 30 首歌)。收到够用的字节数就主动断开连接:
     · 省时间 —— 实测整份 260KB 要 8~20 秒(TLS 慢), 只收 160KB 就断开能省一大半
     · 省内存 —— 缓冲不用留那么大
   由发起请求的函数临时设定, 用完清 0(其它小接口本来就不会触发)。 */
static volatile int  s_stop_after;
static volatile bool s_stopped_early;

typedef struct {
    char *buf;
    int   len;
    int   cap;
    int   status;
} ne_ctx_t;

static esp_err_t ne_ev(esp_http_client_event_t *e)
{
    ne_ctx_t *c = (ne_ctx_t *)e->user_data;
    if (!c) return ESP_OK;
    if (e->event_id == HTTP_EVENT_ON_DATA && c->buf && e->data_len > 0) {
        if (c->len + e->data_len < c->cap - 1) {
            memcpy(c->buf + c->len, e->data, e->data_len);
            c->len += e->data_len;
            c->buf[c->len] = 0;
        }
        /* ★ 收够了就主动断开(返回非 ESP_OK 会让 esp_http_client 立刻中止传输)。
           用这种方式"提前收工", 免得为了开头那点数据把几百 KB 都收完。 */
        int lim = s_stop_after;
        if (lim > 0 && c->len >= lim) {
            s_stopped_early = true;
            return ESP_FAIL;
        }
    } else if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value) {
        /* 登录态就是靠 Set-Cookie 发回来的, 来一个收一个 */
        if (strcasecmp(e->header_key, "Set-Cookie") == 0) ck_set(e->header_value);
    }
    return ESP_OK;
}

/* ★ 曾经这里会把 AI 音频链挂起(为了给硬件 AES 让 DMA 内存)。
 *   后来 sdkconfig 已关掉硬件 AES(改软件 AES, 不需要 DMA), 这个挂起就成了纯伤害:
 *   每次网易云请求(拉歌单要 10~20 秒)期间, 唤醒词/上行/播报全部停摆 ——
 *   用户喊「你好小智」完全没反应(实测抓到: 唤醒词明明命中了, WS 却在排队);
 *   而且每次恢复还要重建唤醒词引擎(长挂起)。现已彻底去掉。
 *   注意: 放歌时音乐模块仍然会挂起 AI, 那是另一条路(互斥喇叭/CPU), 与此无关。 */
static bool ne_post(const char *url, const char *body, char *out, size_t out_sz, int *out_len)
{

    ne_ctx_t ctx = { .buf = out, .len = 0, .cap = (int)out_sz, .status = 0 };
    if (out && out_sz) out[0] = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        /* ★★ 发送缓冲必须 ≥ 整个请求头 ★★
         *
         * 网易云的登录 Cookie 很长(MUSIC_U 一条就 1030 字符, 整条 1.4KB),
         * 加上基础 Cookie 和 User-Agent 等, 请求头接近 1.8KB。
         * 这里原来是 2048 —— 刚好放不下, 实测(带着 Cookie 搜歌):
         *     E HTTP_HEADER: Buffer length is small to fit all the headers
         *     W HTTP_CLIENT: Connection timed out before data was ready!
         *     ne: POST .../weapi/search/get -> HTTP -1, 15700ms, 0 字节
         * 表现就是诡异的两极分化: 【不登录】搜歌完全正常(30 首),
         * 【一登录】所有请求全部超时 —— 因为登录后才会有那么长的 Cookie。
         * 这个缓冲会被分配到内部 RAM(TX+RX 各一份), 4096 足够且不紧张。 */
        .buffer_size = 4096,
        /* ★★★ 必须单独设 buffer_size_tx! ★★★
         *
         * esp_http_client 的【发送】缓冲和【接收】缓冲是两个独立的字段:
         *     .buffer_size     → 接收(RX)
         *     .buffer_size_tx  → 发送(TX), 不设时默认只有 DEFAULT_HTTP_BUF_SIZE = 512 字节!
         * IDF 源码(esp_http_client.c):
         *     client->buffer_size_tx = config->buffer_size_tx;
         *     if (config->buffer_size_tx == 0) client->buffer_size_tx = DEFAULT_HTTP_BUF_SIZE;
         * —— 它【不会】跟随 .buffer_size。请求头(整条 Cookie 1479 字节)塞不进 512,
         * 于是 http_header.c 报 "Buffer length is small to fit all the headers",
         * 请求根本发不出去, 服务器一直等 → 15 秒超时:
         *     E HTTP_HEADER: Buffer length is small to fit all the headers
         *     W HTTP_CLIENT: Connection timed out before data was ready!
         * 实测现象非常迷惑: 【没登录】搜歌正常(30 首, Cookie 只有 80 字节 < 512);
         * 【登录后】所有请求全部超时。 */
        .buffer_size_tx = 4096,
        .event_handler = ne_ev,
        .user_data = &ctx,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }

    /* ★ 放 PSRAM 不占栈: MUSIC_U 一条就有 1030 字符, 整条 Cookie 可能 2KB+,
       而本函数可能被 httpd 任务(栈只有几 KB)调用, 栈上开这么大必炸。 */
    /* ★ +128 而不是 +64: NE_BASE_COOKIE(≈85) + "; " + 满额 Cookie(3072) + NUL
       ≈ 3160 > 3136, 以前会静默截掉串尾 —— 截掉的恰好可能是 MUSIC_U,
       表现为"存了登录态却总被当未登录", 且只在 Cookie 很长时出现。 */
    char *ck = heap_caps_malloc(NE_CK_STR + 128, MALLOC_CAP_SPIRAM);
    if (!ck) { esp_http_client_cleanup(c); return false; }
    ck_build(ck, NE_CK_STR + 128);
    esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_header(c, "Referer", "https://music.163.com/");
    esp_http_client_set_header(c, "Origin", "https://music.163.com");
    esp_http_client_set_header(c, "User-Agent", NE_UA);
    esp_http_client_set_header(c, "Cookie", ck);
    esp_http_client_set_post_field(c, body, strlen(body));
    /* 排查用: 请求头到底多大 / 发送缓冲多大(header 放不下会直接发不出去) */
    ESP_LOGI(TAG, "请求头: Cookie=%d 字节, 缓冲=%d, 正文=%d",
             (int)strlen(ck), (int)cfg.buffer_size, (int)strlen(body));

    s_stopped_early = false;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(c);
    ctx.status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(ck);

    if (out_len) *out_len = ctx.len;

    ESP_LOGI(TAG, "POST %s -> HTTP %d, %dms, %d 字节%s, 登录态=%d",
             url + 8, ctx.status, (int)((esp_timer_get_time() - t0) / 1000), ctx.len,
             s_stopped_early ? "(收够即断开)" : "", (int)ne_client_logged_in());

    /* ★ 这里统一净化一次(见 json_sanitize 注释): 网易云的响应里可能带未转义的
       控制字符(歌单描述里的换行), 不处理会让后面所有 cJSON_Parse 失败。
       所有 weapi/eapi 响应都是 JSON, 所以放在这一个出口最省事、也不会漏。 */
    if (ctx.status == 200 && ctx.len > 0 && out && out_sz) json_sanitize(out);

    /* 主动掐断也算成功: 数据(及其 HTTP 200)已经拿到了, 只是没把整份收完 */
    bool ok = (ctx.status == 200 && ctx.len > 0) && (err == ESP_OK || s_stopped_early);
    return ok;
}

/* 调 weapi 接口(json 为明文请求体), 响应原样放进 out */
static bool call_weapi(const char *path, const char *json, char *out, size_t out_sz, int *out_len)
{
    char *body = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!body) return false;
    bool ok = weapi_body(json, body, 4096);
    if (ok) {
        char url[160];
        snprintf(url, sizeof(url), "https://music.163.com%s", path);
        ok = ne_post(url, body, out, out_sz, out_len);
    }
    free(body);
    return ok;
}

/* 调 eapi 接口(取播放地址) */
static bool call_eapi(const char *path, const char *json, char *out, size_t out_sz, int *out_len)
{
    char *body = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!body) return false;
    bool ok = eapi_body(path, json, body, 4096);
    if (ok) {
        char url[192];
        /* ★★★ 必须拼上 /eapi 前缀! ★★★
         *
         * eapi 的真实请求地址是  https://interface3.music.163.com【/eapi】/api/song/...
         * 而加密时用的"逻辑路径"只是 /api/song/...(不带 /eapi) —— 这正是 eapi
         * 那个"两套前缀"的设计。以前 URL 直接拼成
         *     https://interface3.music.163.com/api/song/enhance/player/url
         * 少了 /eapi, 网易云把它当成另一个接口, 一律回
         *     {"msg":"参数错误","code":400}
         * 实测: 加回 /eapi 后立刻返回正常 data[] 带 mp3 直链。
         * (代理 server/music_proxy.js 有同一个 bug, 已一并修。) */
        snprintf(url, sizeof(url), "https://interface3.music.163.com/eapi%s", path);
        ok = ne_post(url, body, out, out_sz, out_len);
    }
    free(body);
    return ok;
}

/* ================= 业务接口 ================= */
/* JSON 字符串转义(歌名/歌手里有引号会把 JSON 打坏) */
static void jesc(char *dst, size_t sz, const char *src)
{
    size_t o = 0;
    for (; *src && o + 7 < sz; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (c < 0x20)  { o += (size_t)snprintf(dst + o, sz - o, "\\u%04x", c); }
        else dst[o++] = (char)c;
    }
    dst[o] = 0;
}

/* 从歌曲对象里取歌手名: 新接口是 ar[], 老接口是 artists[] */
static void get_artist(const cJSON *song, char *out, size_t sz)
{
    out[0] = 0;
    const cJSON *arr = cJSON_GetObjectItem((cJSON *)song, "ar");
    if (!cJSON_IsArray(arr)) arr = cJSON_GetObjectItem((cJSON *)song, "artists");
    if (!cJSON_IsArray(arr)) return;
    const cJSON *it = NULL;
    int first = 1;
    cJSON_ArrayForEach(it, arr) {
        const cJSON *nm = cJSON_GetObjectItem(it, "name");
        if (!cJSON_IsString(nm)) continue;
        if (!first) strncat(out, "/", sz - strlen(out) - 1);
        strncat(out, nm->valuestring, sz - strlen(out) - 1);
        first = 0;
    }
}

bool ne_api_search(const char *keyword, int limit, char *out, size_t out_sz)
{
    char kw[192];
    jesc(kw, sizeof(kw), keyword);
    char json[320];
    snprintf(json, sizeof(json), "{\"s\":\"%s\",\"type\":1,\"limit\":%d,\"offset\":0}", kw, limit);

    /* ★ 缓冲 24KB→64KB: 搜索响应实测到过 20.9KB(30 个结果), 结果多的关键词
       一超过 24KB 就被截断 → cJSON 必然失败 → 用户看到"返回不是JSON"。
       (和歌单当初的截断是同一类坑; PSRAM 大, 不心疼。) */
    char *resp = heap_caps_malloc(65536, MALLOC_CAP_SPIRAM);
    if (!resp) { snprintf(out, out_sz, "{\"code\":500,\"message\":\"内存不足\"}"); return false; }
    int n = 0;
    bool ok = call_weapi("/weapi/search/get", json, resp, 65536, &n);
    if (!ok) { free(resp); snprintf(out, out_sz, "{\"code\":500,\"message\":\"请求失败\"}"); return false; }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        /* 诊断: 收到多少字节 + cJSON 卡在哪, 一眼分清"截断"还是"格式变了" */
        const char *ep = NULL;
        (void)cJSON_ParseWithOpts(resp, &ep, 0);
        ESP_LOGE(TAG, "搜索 JSON 解析失败: 收到 %d 字节, 卡在偏移 %ld, 开头=%.60s",
                 n, ep ? (long)(ep - resp) : -1L, resp);
        free(resp);
        snprintf(out, out_sz, "{\"code\":500,\"message\":\"返回不是JSON(%d字节)\"}", n);
        return false;
    }
    free(resp);

    size_t o = (size_t)snprintf(out, out_sz, "{\"code\":200,\"data\":[");
    int cnt = 0;
    cJSON *res = cJSON_GetObjectItem(root, "result");
    cJSON *songs = res ? cJSON_GetObjectItem(res, "songs") : NULL;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, songs) {
        if (cnt >= limit || o + 400 >= out_sz) break;
        cJSON *jid = cJSON_GetObjectItem(it, "id");
        cJSON *jnm = cJSON_GetObjectItem(it, "name");
        if (!jid) continue;
        /* ★★ 两个坑都在这里 ★★
           ① /weapi/search/get 的 id 是【数字】, 歌单接口是字符串 —— 两种都要认
           ② 网易云的歌曲 id 已经超过 32 位(例如 2652820720), 用 (int)valuedouble
              取会被截断成 2147483647, 十几首歌的 id 全一样, 后面取播放地址必失败。
              所以必须按 double 打印。 */
        char idb[32];
        if (cJSON_IsString(jid)) snprintf(idb, sizeof(idb), "%s", jid->valuestring);
        else                     snprintf(idb, sizeof(idb), "%.0f", jid->valuedouble);
        if (!idb[0] || (idb[0] == '0' && idb[1] == 0)) continue;
        char ar[128], nm[192];
        get_artist(it, ar, sizeof(ar));
        jesc(nm, sizeof(nm), cJSON_IsString(jnm) ? jnm->valuestring : "");
        o += (size_t)snprintf(out + o, out_sz - o, "%s{\"id\":\"%s\",\"name\":\"%s\",\"artist\":\"%s\"}",
                              cnt ? "," : "", idb, nm, ar);
        cnt++;
    }
    snprintf(out + o, out_sz - o, "],\"count\":%d}", cnt);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "搜索「%s」-> %d 首", keyword, cnt);
    return cnt > 0;
}

bool ne_api_playlists(char *out, size_t out_sz)
{
    if (!ne_client_logged_in()) {
        snprintf(out, out_sz, "{\"code\":301,\"message\":\"未登录 —— 请到「音乐」页扫码登录网易云\"}");
        return false;
    }
    /* ★ 歌单响应的缓冲要够大！
       踩坑记录: 原来是 16384 字节, 而网易云返回的"我的歌单"【超过 16KB】——
       缓冲一满就被截断, JSON 少了尾巴 → cJSON 报错在最后一个字节:
           歌单 JSON 解析失败: 收到 16325 字节, 卡在偏移 16322
       表现就是"接口 HTTP 200 一切正常, 歌单却永远是 0 个"。
       现在放 64KB(PSRAM 有 8MB, 不心疼), 并已在 app_main 把 cJSON 的分配器
       也指到 PSRAM, 所以解析这么大的 JSON 也不会挤爆内部 RAM。 */
#define NE_PL_RESP_SZ (192 * 1024)

/* ★ 只取前这么多个歌单(省内存省流量): 这个账号有 200+ 个歌单, 整份响应 260KB+,
   界面上也用不到那么多 —— 取前 20 个, 收到约 160KB 就主动断开, 传输时间也能
   从 8~20 秒降到几秒。 */
#define NE_PL_MAX 20
#define NE_PL_STOP_BYTES (160 * 1024)
    char *resp = heap_caps_malloc(NE_PL_RESP_SZ, MALLOC_CAP_SPIRAM);
    if (!resp) return false;

    /* ① 先取自己的 uid */
    int n = 0;
    if (!call_weapi("/weapi/w/nuser/account/get", "{}", resp, 16384, &n)) {
        free(resp);
        snprintf(out, out_sz, "{\"code\":500,\"message\":\"取账号失败\"}");
        return false;
    }
    cJSON *root = cJSON_Parse(resp);
    const char *uid = "";
    if (root) {
        cJSON *pf = cJSON_GetObjectItem(root, "profile");
        cJSON *ui = pf ? cJSON_GetObjectItem(pf, "userId") : NULL;
        cJSON *nm = pf ? cJSON_GetObjectItem(pf, "nickname") : NULL;
        static char uidbuf[32];
        /* ★ 用 valuedouble/%.0f: 用户 id 和歌曲 id 一样有超 2^31 的,
           valueint 会截成错的, "我的歌单"就变成别人的/空的(同歌曲 id 的坑) */
        if (ui) snprintf(uidbuf, sizeof(uidbuf), "%.0f", ui->valuedouble);
        uid = uidbuf;
        if (nm && cJSON_IsString(nm)) {
            snprintf(s_nick, sizeof(s_nick), "%s", nm->valuestring);
            ck_save();
        }
        cJSON_Delete(root);
    }
    if (!uid[0]) { free(resp); snprintf(out, out_sz, "{\"code\":500,\"message\":\"未登录\"}"); return false; }

    /* ② 取歌单
       ★ limit 别贪大: 网易云返回的是【完整歌单对象】, 100 个就有 16KB,
       而 HTTP 客户端 + cJSON 都要为它分配内存, 内部 RAM 吃紧时会解析失败。
       我们界面最多也就显示 40 个, 所以直接只要 40 个, 响应小一半以上。 */
    char json[160];
    snprintf(json, sizeof(json), "{\"uid\":\"%s\",\"limit\":40,\"offset\":0}", uid);
    int n2 = 0;
    s_stop_after = NE_PL_STOP_BYTES;      /* 收够就断(见 s_stop_after 说明) */
    bool ok = call_weapi("/weapi/user/playlist", json, resp, NE_PL_RESP_SZ, &n2);
    s_stop_after = 0;
    if (!ok) { free(resp); snprintf(out, out_sz, "{\"code\":500,\"message\":\"取歌单失败\"}"); return false; }

    /* ★ 不用 cJSON —— 见上面 ne_extract_playlists 的说明(响应 256KB+, 建树不现实)。
       直接扫原文, 内存只用 48 个小结构(还是放 PSRAM 的)。 */
    ne_pl_item_t *items = heap_caps_malloc(NE_PL_MAX * sizeof(ne_pl_item_t), MALLOC_CAP_SPIRAM);
    if (!items) { free(resp); snprintf(out, out_sz, "{\"code\":500,\"message\":\"内存不足\"}"); return false; }
    int cnt = ne_extract_playlists(resp, items, NE_PL_MAX);
    free(resp);
    if (cnt == 0) ESP_LOGW(TAG, "歌单响应里没扫到歌单(收到 %d 字节, 开头=%.80s)", n2, "");

    size_t o = (size_t)snprintf(out, out_sz, "{\"code\":200,\"data\":[");
    for (int i = 0; i < cnt; i++) {
        if (o + 300 >= out_sz) break;
        char nm[220];
        jesc(nm, sizeof(nm), items[i].name);
        o += (size_t)snprintf(out + o, out_sz - o, "%s{\"id\":\"%s\",\"name\":\"%s\",\"count\":%d}",
                              i ? "," : "", items[i].id, nm, items[i].count);
    }
    snprintf(out + o, out_sz - o, "],\"count\":%d}", cnt);
    free(items);
    ESP_LOGI(TAG, "我的歌单 -> %d 个 (昵称 %s)", cnt, s_nick);
    return cnt > 0;
}

bool ne_api_playlist_songs(const char *id, int limit, char *out, size_t out_sz)
{
    /* ★ id 只可能是数字(网易云的歌单 id)。外部传来的字串【不校验就直接拼进
       请求体】的话, 带引号或超长内容会把这段 JSON 打坏甚至伪造请求参数。
       这里直接拒绝非数字, 比转义更彻底。 */
    if (!id || !*id) { snprintf(out, out_sz, "{\"code\":500,\"message\":\"歌单 id 为空\"}"); return false; }
    for (const char *p = id; *p; p++) {
        if (*p < '0' || *p > '9') {
            ESP_LOGW(TAG, "歌单 id 含非法字符, 已拒绝: %.24s", id);
            snprintf(out, out_sz, "{\"code\":500,\"message\":\"歌单 id 非法\"}");
            return false;
        }
    }
    char json[128];
    snprintf(json, sizeof(json), "{\"id\":\"%s\",\"n\":%d,\"s\":0}", id, limit);

    /* 同样是 PSRAM 大缓冲: 歌单详情会把整张歌单的歌都返回, 曲目多时轻松超 32KB */
    char *resp = heap_caps_malloc(NE_PL_RESP_SZ, MALLOC_CAP_SPIRAM);
    if (!resp) { snprintf(out, out_sz, "{\"code\":500,\"message\":\"内存不足\"}"); return false; }
    /* 只要前 limit 首, 收够就断开(1272 首的歌单详情有几百 KB, 全收要十几秒) */
    int n = 0;
    s_stop_after = 140 * 1024;
    bool ok = call_weapi("/weapi/v6/playlist/detail", json, resp, NE_PL_RESP_SZ, &n);
    s_stop_after = 0;
    if (!ok) { free(resp); snprintf(out, out_sz, "{\"code\":500,\"message\":\"请求失败\"}"); return false; }

    /* ★ 不用 cJSON: 见 ne_extract_tracks 的说明(大歌单详情几百 KB 且常被截断,
       cJSON 遇不完整的 JSON 直接返回 NULL, 表现就是"装进 0 首")。 */
    char plname[160];
    ne_tr_item_t *items = heap_caps_malloc((size_t)limit * sizeof(ne_tr_item_t), MALLOC_CAP_SPIRAM);
    if (!items) { free(resp); snprintf(out, out_sz, "{\"code\":500,\"message\":\"内存不足\"}"); return false; }
    int cnt = ne_extract_tracks(resp, "tracks", plname, sizeof(plname), items, limit);
    free(resp);

    char pln[200];
    jesc(pln, sizeof(pln), plname);
    size_t o = (size_t)snprintf(out, out_sz, "{\"code\":200,\"name\":\"%s\",\"data\":[", pln);
    for (int i = 0; i < cnt; i++) {
        if (o + 400 >= out_sz) break;
        char nm[192], ar[128];
        jesc(nm, sizeof(nm), items[i].name);
        jesc(ar, sizeof(ar), items[i].artist);
        o += (size_t)snprintf(out + o, out_sz - o,
                              "%s{\"id\":\"%s\",\"name\":\"%s\",\"artist\":\"%s\"}",
                              i ? "," : "", items[i].id, nm, ar);
    }
    snprintf(out + o, out_sz - o, "]}");
    free(items);
    ESP_LOGI(TAG, "歌单「%s」-> %d 首(扫原文)", plname, cnt);
    return cnt > 0;
}

/* ==================== 按歌手名搜歌 ====================
 * ① /weapi/search/get  type=100 → 搜"歌手"(网易云官方网页搜索切到"歌手"标签
 *    用的就是 type=100, 同一个接口);  取匹配度最高的第一个歌手的 id
 * ② /weapi/v1/artist/<id>       → 该歌手的热门歌曲(官方歌手页同款接口)
 * 输出与歌名搜索完全同构: {"code":200,"data":[{id,name,artist}...]},
 * music.c 的解析/装列代码原样复用。 */
bool ne_api_search_artist_id(const char *name, char *aid, size_t aid_sz,
                             char *aname, size_t aname_sz)
{
    if (aid_sz) aid[0] = 0;
    if (aname_sz) aname[0] = 0;
    char kw[192];
    jesc(kw, sizeof(kw), name);
    char json[320];
    snprintf(json, sizeof(json), "{\"s\":\"%s\",\"type\":100,\"limit\":3,\"offset\":0}", kw);

    char *resp = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    if (!resp) return false;
    int n = 0;
    if (!call_weapi("/weapi/search/get", json, resp, 16384, &n)) { free(resp); return false; }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return false;
    cJSON *res  = cJSON_GetObjectItem(root, "result");
    cJSON *arts = res ? cJSON_GetObjectItem(res, "artists") : NULL;
    cJSON *a0   = (arts && cJSON_IsArray(arts)) ? cJSON_GetArrayItem(arts, 0) : NULL;
    cJSON *jid  = a0 ? cJSON_GetObjectItem(a0, "id") : NULL;
    cJSON *jnm  = a0 ? cJSON_GetObjectItem(a0, "name") : NULL;
    bool ok = false;
    if (jid && !cJSON_IsString(jid)) {
        /* 歌手 id 同样可能超 2^31 —— 一律按 double 打印(歌曲 id 踩过的同款坑) */
        snprintf(aid, aid_sz, "%.0f", jid->valuedouble);
        ok = aid[0] != 0;
    }
    if (ok && cJSON_IsString(jnm)) snprintf(aname, aname_sz, "%s", jnm->valuestring);
    cJSON_Delete(root);
    return ok;
}

bool ne_api_artist_hot(const char *aid, int limit, char *out, size_t out_sz)
{
    char path[64];
    snprintf(path, sizeof(path), "/weapi/v1/artist/%s", aid);

    char *resp = heap_caps_malloc(NE_PL_RESP_SZ, MALLOC_CAP_SPIRAM);
    if (!resp) { snprintf(out, out_sz, "{\"code\":500,\"message\":\"内存不足\"}"); return false; }
    int n = 0;
    /* 歌手 id 在路径里, body 为空 —— 官方歌手页就是这么调的 */
    if (!call_weapi(path, "{}", resp, NE_PL_RESP_SZ, &n)) {
        free(resp);
        snprintf(out, out_sz, "{\"code\":500,\"message\":\"请求失败\"}");
        return false;
    }

    ne_tr_item_t *items = heap_caps_malloc((size_t)limit * sizeof(ne_tr_item_t), MALLOC_CAP_SPIRAM);
    if (!items) {
        free(resp);
        snprintf(out, out_sz, "{\"code\":500,\"message\":\"内存不足\"}");
        return false;
    }
    int cnt = ne_extract_tracks(resp, "hotSongs", NULL, 0, items, limit);
    free(resp);

    size_t o = (size_t)snprintf(out, out_sz, "{\"code\":200,\"data\":[");
    for (int i = 0; i < cnt; i++) {
        if (o + 400 >= out_sz) break;
        char nm[192], ar[128];
        jesc(nm, sizeof(nm), items[i].name);
        jesc(ar, sizeof(ar), items[i].artist);
        o += (size_t)snprintf(out + o, out_sz - o,
                              "%s{\"id\":\"%s\",\"name\":\"%s\",\"artist\":\"%s\"}",
                              i ? "," : "", items[i].id, nm, ar);
    }
    snprintf(out + o, out_sz - o, "]}");
    free(items);
    ESP_LOGI(TAG, "歌手(id=%s)热门歌曲 -> %d 首", aid, cnt);
    return cnt > 0;
}

/* 取播放地址: 必须已登录, 否则网易云一律不给 */
bool ne_api_song_url(const char *id, int br, char *out_url, size_t out_sz)
{
    out_url[0] = 0;
    if (!ne_client_logged_in()) {
        ESP_LOGW(TAG, "取播放地址: 未登录");
        return false;
    }
    char json[256];
    snprintf(json, sizeof(json),
             "{\"ids\":\"[%s]\",\"br\":%d,\"encodeType\":\"mp3\",\"header\":{\"os\":\"pc\",\"appver\":\"2.9.7\"}}",
             id, br);

    char *resp = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!resp) return false;
    int n = 0;
    bool ok = call_eapi("/api/song/enhance/player/url", json, resp, 8192, &n);
    if (!ok) { free(resp); return false; }
    ESP_LOGI(TAG, "取地址响应(%d 字节): %.200s", n, resp);   /* 排查用 */

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) return false;
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *d0 = cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 0) : NULL;
    cJSON *u = d0 ? cJSON_GetObjectItem(d0, "url") : NULL;
    cJSON *cd = d0 ? cJSON_GetObjectItem(d0, "code") : NULL;
    bool got = false;
    if (cJSON_IsString(u) && u->valuestring[0] && (!cd || cd->valueint == 200)) {
        snprintf(out_url, out_sz, "%s", u->valuestring);
        got = true;
        ESP_LOGI(TAG, "取到播放地址: %.80s...", out_url);
    } else {
        ESP_LOGW(TAG, "无可用播放地址 (code=%d, 多为 VIP/版权受限)",
                 cd ? cd->valueint : -1);
    }
    cJSON_Delete(root);
    return got;
}

/* ================= 扫码登录 ================= */
bool ne_login_qr_key(char *key_out, size_t key_sz)
{
    key_out[0] = 0;
    char *resp = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!resp) return false;
    int n = 0;
    /* ★ 必须带 type:1 —— 实测传 {} 网易云回"参数错误" */
    bool ok = call_weapi("/weapi/login/qrcode/unikey", "{\"type\":1}", resp, 4096, &n);
    if (ok) {
        cJSON *root = cJSON_Parse(resp);
        if (root) {
            cJSON *u = cJSON_GetObjectItem(root, "unikey");
            if (cJSON_IsString(u)) snprintf(key_out, key_sz, "%s", u->valuestring);
            cJSON_Delete(root);
        }
    }
    free(resp);
    if (key_out[0]) ESP_LOGI(TAG, "二维码 key: %s", key_out);
    return key_out[0] != 0;
}

int ne_login_qr_check(const char *key, char *nick_out, size_t nick_sz)
{
    if (nick_out && nick_sz) nick_out[0] = 0;
    char json[160];
    snprintf(json, sizeof(json), "{\"key\":\"%s\",\"type\":1}", key);

    char *resp = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!resp) return 0;
    int n = 0;
    bool ok = call_weapi("/weapi/login/qrcode/client/login", json, resp, 4096, &n);
    int code = 0;
    if (ok) {
        /* 把网易云的原话打出来: 801=未扫 802=已扫待确认 803=成功 800=过期。
           排查"扫了没反应"时, 这行能直接说明是 key 不对还是状态没变。 */
        ESP_LOGI(TAG, "扫码状态响应: %.70s", resp);
        cJSON *root = cJSON_Parse(resp);
        if (root) {
            cJSON *c = cJSON_GetObjectItem(root, "code");
            if (c) code = c->valueint;
            /* 803 = 登录成功, 网易云在 Set-Cookie 里发回了 MUSIC_U(已在 ne_ev 里收下) */
            cJSON *pf = cJSON_GetObjectItem(root, "profile");
            cJSON *nm = pf ? cJSON_GetObjectItem(pf, "nickname") : NULL;
            if (cJSON_IsString(nm)) snprintf(s_nick, sizeof(s_nick), "%s", nm->valuestring);
            cJSON_Delete(root);
        }
        if (code == 803) {
            if (!s_nick[0]) snprintf(s_nick, sizeof(s_nick), "网易云用户");
            ck_save();
            ESP_LOGW(TAG, "★ 扫码登录成功: %s (已登录=%d)", s_nick, (int)ne_client_logged_in());
        }
    }
    free(resp);
    if (nick_out && nick_sz) snprintf(nick_out, nick_sz, "%s", s_nick);
    return code;
}

/* 手动设置 Cookie(给"不方便扫码"的人用: 从浏览器 F12 复制 MUSIC_U=... 贴进来)。
   扫码是主路径, 这条是兜底 —— 两条路最终都写同一份 NVS。 */
bool ne_login_set_cookie(const char *ck)
{
    if (!ck || !ck[0]) return false;
    s_ck_raw[0] = 0;                       /* 重新粘贴 → 整条替换 */
    char *buf = heap_caps_malloc(strlen(ck) + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return false;
    strcpy(buf, ck);
    char *save = NULL;
    for (char *t = strtok_r(buf, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        while (*t == ' ') t++;
        ck_set(t);
    }
    free(buf);
    if (!ne_client_logged_in()) {
        ESP_LOGW(TAG, "粘贴的 Cookie 里没有 MUSIC_U, 无效");
        s_ck_ok = 0;
        return false;
    }
    /* ★★ 这里【绝对不能】顺手调 ne_client_verify() ★★
     *
     * 本函数是 httpd 的回调, 而 httpd 任务栈只有 4KB —— 校验要发 HTTPS,
     * TLS 握手需要 16KB 栈, 在 httpd 里发会直接卡死(实测: 网页 POST 超时,
     * httpd 卡住不再响应任何请求)。
     * 正确做法: 只存 Cookie 并把状态置为"待校验", 由 nelogin 任务(16KB 栈,
     * 常驻, 每 15 秒一次)去问网易云, 结果通过 /nlogin 的 ckok 字段告诉网页。 */
    s_ck_ok = -1;                 /* 待校验: 交给登录轮询任务 */
    if (!s_nick[0]) snprintf(s_nick, sizeof(s_nick), "网易云用户");
    ck_save();
    ESP_LOGW(TAG, "已收到粘贴的 Cookie (%d 字节), 待后台校验", (int)strlen(s_ck_raw));
    return true;
}

void ne_login_status_json(char *out, size_t out_sz)
{
    /* ★ 昵称必须转义: 网易云昵称是用户可控的, 带 " 或 \ 时原样嵌入会让
       整个 JSON 解析失败, 网页登录状态就死了。jesc() 是本文件现成的。 */
    char nick_esc[128];
    jesc(nick_esc, sizeof(nick_esc), ne_client_logged_in() ? s_nick : "");
    snprintf(out, out_sz, "{\"login\":%s,\"nick\":\"%s\"}",
             ne_client_logged_in() ? "true" : "false", nick_esc);
}

/* ================= 扫码登录的后台轮询 =================
 *
 * ★★ 这里曾经是一个【严重设计错误】, 记录在此以免再犯 ★★
 *
 * 旧做法: 网页每 2 秒 GET 一次 /nlogin, 每次都在设备上新建一个任务去打一次
 * 网易云 HTTPS。看起来"很实时", 实际上是:
 *   开 2 个标签页 → 每秒 1 次 TLS 握手 → 每次都要 socket + 几十 KB 内部 RAM
 *   → LWIP 的 socket 池(默认 10 个)被占干 → httpd 的 accept() 直接失败:
 *        E httpd: httpd_accept_conn: error in accept (23)   ← errno 23 = 无可用 socket
 *   → 表现就是【网页打不开了】("访问网页进不去" 的真凶, 实测复现过)。
 *   而且它还每 2 秒续一次, 自己不会停。
 *
 * 新做法: 轮询由设备自己做 —— 一个常驻任务, 3 秒一次, 且满足下面三个条件才动网络:
 *   ① 确实有一张没被扫过的二维码(s_qr_active)
 *   ② 用户点了登录/刷新(s_qr_want)
 *   ③ 不在播音乐(播歌时 socket/内部 RAM 已经被音乐占满, 再叠 TLS 会互相拖死)
 * /nlogin 则变成【纯查询】: 只回缓存状态, 一次网络都不打 —— 开多少个标签页
 * 都不会影响设备。 */
static char s_qr_key[64];
static int  s_qr_code;          /* 801 待扫 / 802 待确认 / 803 成功 / 800 过期 / -1 取key失败 */
static volatile bool s_login_busy;
static volatile bool s_qr_want;     /* 用户点了"扫码登录/刷新二维码" → 去取一张新码 */
static bool s_skip_logged;          /* 只打一次"播歌暂停"日志, 避免刷屏 */
static volatile TickType_t s_last_ui;      /* 网页最后一次来查状态的时间 */
static TickType_t s_last_key_try;          /* 上次尝试取二维码的时间(限速用) */
static volatile bool s_qr_active;   /* 有一张未过期的码在等扫 → 才需要轮询 */

static void ne_login_task(void *arg)
{
    ESP_LOGI(TAG, "登录轮询任务已就绪(常驻: 有码才查, 3 秒一次, 播歌时暂停)");
    int hb = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (++hb >= 60) {           /* 每 30 秒一次存活心跳, 方便排查 */
            hb = 0;
            ESP_LOGI(TAG, "登录轮询存活: 登录=%d 有码=%d key=%.8s ui空闲=%ds",
                     (int)ne_client_logged_in(), (int)s_qr_active, s_qr_key,
                     (int)((xTaskGetTickCount() - s_last_ui) / configTICK_RATE_HZ));
        }

        if (ne_client_logged_in()) {                /* 已登录: 不用再轮询扫码 */
            s_qr_want = false; s_qr_active = false;
            /* ★ 开机带着 NVS 里的 Cookie 进来 → 验一次真伪。
               不验的话, 过期 Cookie 会让网页一直显示"已登录", 用户却搜不到歌。
               (播歌时跳过, 网络留给音乐。) */
            bool cool = (s_last_verify == 0) ||
                        ((xTaskGetTickCount() - s_last_verify) > pdMS_TO_TICKS(15000));
            if (s_ck_ok < 0 && !music_is_playing() && cool) {
                s_last_verify = xTaskGetTickCount();
                s_login_busy = true;
                ne_client_verify(NULL, 0);
                s_login_busy = false;
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
            s_login_busy = false;
            continue;
        }
        /* ★ 播音乐时绝不打网络: 音乐那条流已经占满 socket 和内部 RAM,
           再叠一次 TLS 握手会把两边一起拖死(实测音乐缓冲被拖到 0)。 */
        if (music_is_playing()) {
            if (!s_skip_logged) {
                s_skip_logged = true;
                ESP_LOGW(TAG, "★ 播歌中 → 登录轮询暂停(音乐结束会自动继续)");
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (s_skip_logged) { s_skip_logged = false; ESP_LOGI(TAG, "音乐结束 → 登录轮询恢复"); }

        if (s_qr_want) {                            /* ① 用户要一张新码 */
            s_qr_want = false;
            s_last_key_try = xTaskGetTickCount();
            s_login_busy = true;
            char k[64];
            if (ne_login_qr_key(k, sizeof(k))) {
                snprintf(s_qr_key, sizeof(s_qr_key), "%s", k);
                s_qr_code = 801; s_qr_active = true;
            } else {
                s_qr_code = -1; s_qr_active = false;
            }
            s_login_busy = false;
            vTaskDelay(pdMS_TO_TICKS(1200));
            continue;
        }

        if (!s_qr_active) {                         /* ② 没有待扫的码 */
            /* ★ 自动补码: 只要【网页还有人开着】(2 分钟内来查过状态), 而且距上次
               尝试超过 25 秒, 就自动去要一张新码。
               为什么必须这么做: 以前码过期(800)或取码失败后任务就"躺平", 网页上
               那张码已经作废了、设备却什么都不做 —— 用户扫的就是一张死码, 表现就是
               "我明明扫码授权了, 却一直显示等待扫码"。现在过期会自动换新的,
               页面 2 秒内就跟上(二维码下面的 ID 会跟着变)。
               没人看时不补码: 免得白白消耗网易云的取码额度。 */
            bool viewing = (xTaskGetTickCount() - s_last_ui) < pdMS_TO_TICKS(120000);
            bool cool_ok  = (s_last_key_try == 0) ||                 /* 开机第一次: 立刻要 */
                            ((xTaskGetTickCount() - s_last_key_try) > pdMS_TO_TICKS(25000));
            if (viewing && cool_ok) {
                s_last_key_try = xTaskGetTickCount();
                s_login_busy = true;
                char k[64];
                if (ne_login_qr_key(k, sizeof(k))) {
                    snprintf(s_qr_key, sizeof(s_qr_key), "%s", k);
                    s_qr_code = 801; s_qr_active = true;
                    ESP_LOGI(TAG, "★ 自动补了一张新二维码: %.8s", s_qr_key);
                } else {
                    s_qr_code = -1;
                }
                s_login_busy = false;
                vTaskDelay(pdMS_TO_TICKS(1500));
                continue;
            }
            s_login_busy = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        s_login_busy = true;                        /* ③ 查一次扫码状态 */
        char nick[48];
        int c = ne_login_qr_check(s_qr_key, nick, sizeof(nick));
        s_login_busy = false;
        if (c) s_qr_code = c;
        if (c == 803) { s_qr_active = false; continue; }        /* 登录成功 → 停止 */
        if (c == 800) { s_qr_key[0] = 0; s_qr_active = false; } /* 过期 → 停下等用户点刷新 */

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* 开机建一次(内部 RAM 留给 WebSocket —— 这个任务栈长期存在, 必须放 PSRAM) */
void ne_login_worker_start(void)
{
    static bool started = false;
    if (started) return;
    started = true;
    BaseType_t r = xTaskCreatePinnedToCoreWithCaps(ne_login_task, "nelogin", 16384, NULL, 3, NULL, 1,
                                                   MALLOC_CAP_SPIRAM);
    if (r != pdPASS) {
        started = false;
        /* PSRAM 栈建不起来时的兜底: 退回内部 RAM(小一点), 否则登录功能整个失效 */
        ESP_LOGW(TAG, "登录轮询任务: PSRAM 栈创建失败, 改用内部 RAM");
        r = xTaskCreatePinnedToCoreWithCaps(ne_login_task, "nelogin", 12288, NULL, 3, NULL, 1,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (r != pdPASS) { started = false; ESP_LOGE(TAG, "登录轮询任务创建失败(内存不足)"); }
    }
}

/* 网页点了"扫码登录 / 刷新二维码": 只打个标记, 真正的请求由常驻任务去做 */
void ne_login_request_key(void)
{
    s_qr_want = true;
    s_qr_active = false;
    s_qr_code = 0;
    ne_login_worker_start();
}

void ne_login_reset(void)
{
    s_qr_key[0] = 0;
    s_qr_code = 0;
    s_qr_active = false;
    s_qr_want = false;
}

void ne_login_state_json(char *out, size_t out_sz)
{
    s_last_ui = xTaskGetTickCount();     /* 网页还活着 → 允许自动补码 */
    /* ckok: -1 还没验过 / 0 已失效 / 1 有效。只有验证通过的才敢说"已登录" */
    bool logged = (s_ck_ok == 1) || (s_ck_ok < 0 && ne_client_logged_in());
    char nick_esc[128];
    jesc(nick_esc, sizeof(nick_esc), (s_ck_ok == 1) ? s_nick : "");
    snprintf(out, out_sz,
             "{\"login\":%s,\"nick\":\"%s\",\"ckok\":%d,\"key\":\"%s\",\"code\":%d,\"busy\":%s}",
             logged ? "true" : "false",
             nick_esc,
             (int)s_ck_ok, s_qr_key, s_qr_code, s_login_busy ? "true" : "false");
}

/*__TAIL__*/

