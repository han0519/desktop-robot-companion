/*
 * ne_probe.c — 网易云【直连】可行性探针 (临时验证用, 不属于发布功能)
 *
 * 目的: 只验证两件最难的事, 不动现有代理架构
 *   ① 设备能不能自己算出网易云要求的加密参数
 *      weapi = base64( AES-CBC( base64(AES-CBC(json, 预设密钥)), 随机密钥 ) ) + RSA 裸加密随机密钥
 *   ② 设备直连 music.163.com 的 HTTPS(握手+收发) 内存够不够
 *
 * 触发: 浏览器/curl 访问  http://<设备IP>/neprobe?q=晴天
 */
#include "ne_probe.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "xiaozhi.h"   /* ai_client_set_audio_suspended() */
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/bignum.h"

static const char *TAG = "neprobe";

#define NE_PRESET_KEY "0CoJUm6Qyw8W8jud"
static const uint8_t NE_IV[16] = {'0','1','0','2','0','3','0','4','0','5','0','6','0','7','0','8'};
static const char    NE_BASE62[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

/* 网易云网页端 RSA 公钥的模数 N (1024 位), 指数 E = 65537。
   直接从 PEM 里预先算出来的固定常量 —— 设备端省掉 PEM 解析, 也少一份依赖。 */
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

/* PKCS#7 填充 + AES-128-CBC 加密 + base64 输出 */
static int aes_cbc_b64(const uint8_t *in, size_t in_len, const uint8_t *key,
                       char *out, size_t out_sz)
{
    size_t pad = 16 - (in_len % 16);
    size_t tot = in_len + pad;
    uint8_t *src = heap_caps_malloc(tot, MALLOC_CAP_SPIRAM);
    uint8_t *dst = heap_caps_malloc(tot, MALLOC_CAP_SPIRAM);
    if (!src || !dst) { free(src); free(dst); return -1; }
    memcpy(src, in, in_len);
    memset(src + in_len, (int)pad, pad);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    uint8_t iv[16];
    memcpy(iv, NE_IV, 16);
    int r = mbedtls_aes_setkey_enc(&aes, key, 128);
    if (r == 0) r = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, tot, iv, src, dst);
    mbedtls_aes_free(&aes);
    free(src);
    if (r != 0) { free(dst); ESP_LOGE(TAG, "AES 失败 %d", r); return -2; }

    size_t olen = 0;
    r = mbedtls_base64_encode((unsigned char *)out, out_sz, &olen, dst, tot);
    free(dst);
    if (r != 0) { ESP_LOGE(TAG, "base64 失败 %d (缓冲 %u)", r, (unsigned)out_sz); return -3; }
    return (int)olen;
}

/* RSA 裸加密(不做填充): out = in^65537 mod N, 输出 256 字符十六进制 */
static bool rsa_raw_hex(const uint8_t in[128], char out[257])
{
    mbedtls_mpi N, E, M, C;
    mbedtls_mpi_init(&N); mbedtls_mpi_init(&E);
    mbedtls_mpi_init(&M); mbedtls_mpi_init(&C);
    int r = mbedtls_mpi_read_binary(&N, NE_RSA_N, 128);
    if (r == 0) r = mbedtls_mpi_lset(&E, 65537);
    if (r == 0) r = mbedtls_mpi_read_binary(&M, in, 128);
    if (r == 0) r = mbedtls_mpi_exp_mod(&C, &M, &E, &N, NULL);
    uint8_t bin[128] = {0};
    if (r == 0) r = mbedtls_mpi_write_binary(&C, bin, 128);
    mbedtls_mpi_free(&N); mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&M); mbedtls_mpi_free(&C);
    if (r != 0) { ESP_LOGE(TAG, "RSA 失败 %d", r); return false; }
    static const char HX[] = "0123456789abcdef";
    for (int i = 0; i < 128; i++) {
        out[i * 2]     = HX[bin[i] >> 4];
        out[i * 2 + 1] = HX[bin[i] & 0xF];
    }
    out[256] = '\0';
    return true;
}

/* URL 编码(表单用): base64 里的 + / = 必须转义 */
static void urlenc(const char *s, char *out, size_t out_sz)
{
    size_t o = 0;
    for (; *s && o + 4 < out_sz; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            static const char H[] = "0123456789ABCDEF";
            out[o++] = '%'; out[o++] = H[c >> 4]; out[o++] = H[c & 0xF];
        }
    }
    out[o] = '\0';
}

/* 组装 weapi 表单体: params=<...>&encSecKey=<...> */
static bool build_weapi_body(const char *json, char *body, size_t body_sz)
{
    uint8_t sk[16];
    for (int i = 0; i < 16; i++) sk[i] = (uint8_t)NE_BASE62[esp_random() % 62];

    char step1[1024];
    if (aes_cbc_b64((const uint8_t *)json, strlen(json),
                    (const uint8_t *)NE_PRESET_KEY, step1, sizeof(step1)) < 0) return false;

    char params[1024];
    if (aes_cbc_b64((const uint8_t *)step1, strlen(step1), sk, params, sizeof(params)) < 0) return false;

    /* 关键: 16 字节密钥【反序】后左侧补零到 128 字节, 再做裸 RSA */
    uint8_t rev[128] = {0};
    for (int i = 0; i < 16; i++) rev[112 + i] = sk[15 - i];

    char enc[257];
    if (!rsa_raw_hex(rev, enc)) return false;

    char ep[1400], ee[600];
    urlenc(params, ep, sizeof(ep));
    urlenc(enc, ee, sizeof(ee));
    snprintf(body, body_sz, "params=%s&encSecKey=%s", ep, ee);
    return true;
}

/* HTTP 事件: 收到数据就追加到缓冲 */
static char *s_rx;          /* PSRAM */
static int   s_rx_len, s_rx_cap;
static esp_err_t http_ev(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && s_rx && e->data_len > 0) {
        int n = e->data_len;
        if (s_rx_len + n < s_rx_cap - 1) {
            memcpy(s_rx + s_rx_len, e->data, n);
            s_rx_len += n;
            s_rx[s_rx_len] = '\0';
        }
    }
    return ESP_OK;
}

static void ne_probe_task(void *arg)
{
    char kw[64];
    strncpy(kw, (const char *)arg, sizeof(kw) - 1);
    kw[sizeof(kw) - 1] = '\0';
    free(arg);

    size_t hb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t hl = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "=== 开始 === 内部RAM 剩余=%u 最大连续=%u", (unsigned)hb, (unsigned)hl);

    /* ① 组装加密参数 */
    char json[192];
    snprintf(json, sizeof(json), "{\"s\":\"%s\",\"type\":1,\"limit\":3,\"offset\":0}", kw);
    static char body[4096];
    int64_t t0 = esp_timer_get_time();
    if (!build_weapi_body(json, body, sizeof(body))) {
        ESP_LOGE(TAG, "加密参数组装失败");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "① 加密参数 OK, 耗时 %d ms, body %u 字节",
             (int)((esp_timer_get_time() - t0) / 1000), (unsigned)strlen(body));
    ESP_LOGI(TAG, "   params 前 60: %.60s", strstr(body, "params=") + 7);

    /* ② 直连网易云 HTTPS */
    s_rx_cap = 8192;
    s_rx = heap_caps_malloc(s_rx_cap, MALLOC_CAP_SPIRAM);
    s_rx_len = 0;
    if (!s_rx) { ESP_LOGE(TAG, "接收缓冲不足"); vTaskDelete(NULL); return; }

    /* ★ 实测发现: TLS 握手能过(证书已验证), 但读数据时报
       "esp-aes: Failed to allocate memory" —— AES 硬件加速走 DMA,
       需要【DMA 可用的内部 RAM】, 而 AI 音频链(唤醒词/Opus)正占着。
       生产上的做法就是: 收发网易云请求的这几百毫秒里, 把 AI 音频链短暂挂起。
       (放音乐时它本来就挂起着, 所以播歌反而不缺内存) */
    ESP_LOGI(TAG, "DMA 可用: 剩余=%u 最大=%u  (挂起 AI 音频链前)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    ai_client_set_audio_suspended(true);
    vTaskDelay(pdMS_TO_TICKS(120));     /* 等音频任务真正让出内存 */
    ESP_LOGI(TAG, "DMA 可用: 剩余=%u 最大=%u  (挂起后)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    esp_http_client_config_t cfg = {
        .url = "https://music.163.com/weapi/search/get",
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .event_handler = http_ev,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { ESP_LOGE(TAG, "客户端初始化失败"); vTaskDelete(NULL); return; }

    esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_header(c, "Referer", "https://music.163.com/");
    esp_http_client_set_header(c, "Origin", "https://music.163.com");
    esp_http_client_set_header(c, "User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36");
    esp_http_client_set_header(c, "Cookie",
        "os=pc; appver=2.9.7; channel=netease; osver=Microsoft-Windows-10--build-22631-64bit");
    esp_http_client_set_post_field(c, body, strlen(body));

    t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(c);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    int st = esp_http_client_get_status_code(c);

    ESP_LOGI(TAG, "② HTTPS 直连: err=%s HTTP=%d 耗时=%d ms 收到=%d 字节",
             esp_err_to_name(err), st, ms, s_rx_len);
    if (s_rx_len > 0) ESP_LOGI(TAG, "   返回前 320: %.320s", s_rx);

    ai_client_set_audio_suspended(false);     /* 恢复 AI 音频链 */

    size_t ha = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t la = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "=== 结束 === 内部RAM 剩余=%u(Δ%d) 最大连续=%u(Δ%d)",
             (unsigned)ha, (int)ha - (int)hb, (unsigned)la, (int)la - (int)hl);
    ESP_LOGI(TAG, "结论: %s",
             (st == 200 && s_rx_len > 0) ? "✅ 直连可行 —— 加密与 HTTPS 都通了"
                                         : "❌ 未通过, 看上面 err/HTTP 码");

    esp_http_client_cleanup(c);
    free(s_rx); s_rx = NULL;
    vTaskDelete(NULL);
}

void ne_probe_start(const char *keyword)
{
    char *arg = strdup(keyword ? keyword : "晴天");
    /* 栈放 PSRAM: 和音频任务一致的做法, 不给内部 RAM 添压力(TLS 握手约需 16KB) */
    TaskHandle_t h = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(ne_probe_task, "neprobe", 16384, arg, 5, &h, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "探针任务创建失败");
        free(arg);
    }
}


