/*
 * ws2812.c — WS2812 驱动实现 (新 RMT TX API)
 * 注意: rmt_transmit 的 config 参数必须传有效结构体, 不能传 NULL
 */
#include "ws2812.h"
#include "config.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ws2812";

/* RMT 通道和编码器 */
static rmt_channel_handle_t s_ring_chan = NULL;
static rmt_encoder_handle_t s_ring_encoder = NULL;
static rmt_channel_handle_t s_onboard_chan = NULL;
static rmt_encoder_handle_t s_onboard_encoder = NULL;

/* 灯环像素缓冲区 */
static rgb_t s_ring_buf[WS2812_LED_RING_NUM];

/* 灯效任务以 ~40Hz 刷新, 每帧都 calloc/free 会造成堆抖动。
   改用静态符号缓冲 + 互斥锁(灯效任务和触摸任务都会调到这里)。 */
#define WS2812_SYM_MAX  (WS2812_LED_RING_NUM * 24)
static rmt_symbol_word_t s_symbols[WS2812_SYM_MAX];
static SemaphoreHandle_t  s_lock = NULL;

/* WS2812 时序: 10MHz 分辨率, 1 tick = 100ns
   T0H=400ns(4ticks), T0L=850ns(9ticks)
   T1H=800ns(8ticks), T1L=450ns(5ticks) */
#define WS2812_T0H  4
#define WS2812_T0L  9
#define WS2812_T1H  8
#define WS2812_T1L  5

static void ws2812_build_symbols(rmt_symbol_word_t *symbols, const rgb_t *pixels, int count)
{
    int idx = 0;
    for (int i = 0; i < count; i++) {
        /* WS2812 顺序: GRB */
        uint8_t bytes[3] = { pixels[i].g, pixels[i].r, pixels[i].b };
        for (int b = 0; b < 3; b++) {
            for (int bit = 7; bit >= 0; bit--) {
                if (bytes[b] & (1 << bit)) {
                    symbols[idx].level0 = 1; symbols[idx].duration0 = WS2812_T1H;
                    symbols[idx].level1 = 0; symbols[idx].duration1 = WS2812_T1L;
                } else {
                    symbols[idx].level0 = 1; symbols[idx].duration0 = WS2812_T0H;
                    symbols[idx].level1 = 0; symbols[idx].duration1 = WS2812_T0L;
                }
                idx++;
            }
        }
    }
}

static void ws2812_init_channel(int gpio, rmt_channel_handle_t *chan,
                                 rmt_encoder_handle_t *encoder, const char *name)
{
    rmt_tx_channel_config_t ch_cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = WS2812_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&ch_cfg, chan));

    rmt_copy_encoder_config_t enc_cfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&enc_cfg, encoder));

    ESP_ERROR_CHECK(rmt_enable(*chan));
    ESP_LOGI(TAG, "%s init OK (GPIO%d)", name, gpio);
}

void ws2812_init(void)
{
    memset(s_ring_buf, 0, sizeof(s_ring_buf));
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    ws2812_init_channel(WS2812_LED_RING_GPIO, &s_ring_chan, &s_ring_encoder, "WS2812 ring");
    ws2812_init_channel(WS2812_ONBOARD_GPIO, &s_onboard_chan, &s_onboard_encoder, "WS2812 onboard");
}

static void ws2812_send(rmt_channel_handle_t chan, rmt_encoder_handle_t encoder,
                        const rgb_t *pixels, int count)
{
    int symbol_count = count * 24;  /* 每像素 24 bit */
    if (symbol_count > WS2812_SYM_MAX) return;

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    rmt_symbol_word_t *symbols = s_symbols;   /* 静态缓冲, 不碰堆 */
    ws2812_build_symbols(symbols, pixels, count);

    rmt_transmit_config_t tx_cfg = {0};  /* 必须传有效结构体, 不能传 NULL */
    esp_err_t ret = rmt_transmit(chan, encoder, symbols,
                                  symbol_count * sizeof(rmt_symbol_word_t), &tx_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_transmit failed: %s", esp_err_to_name(ret));
    }
    rmt_tx_wait_all_done(chan, pdMS_TO_TICKS(100));
    if (s_lock) xSemaphoreGive(s_lock);
}

void ws2812_ring_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index < 0 || index >= WS2812_LED_RING_NUM) return;
    s_ring_buf[index].r = r;
    s_ring_buf[index].g = g;
    s_ring_buf[index].b = b;
}

void ws2812_ring_fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < WS2812_LED_RING_NUM; i++) {
        s_ring_buf[i].r = r;
        s_ring_buf[i].g = g;
        s_ring_buf[i].b = b;
    }
}

void ws2812_ring_show(void)
{
    ws2812_send(s_ring_chan, s_ring_encoder, s_ring_buf, WS2812_LED_RING_NUM);
}

void ws2812_onboard_set(uint8_t r, uint8_t g, uint8_t b)
{
    rgb_t px = { .r = r, .g = g, .b = b };
    ws2812_send(s_onboard_chan, s_onboard_encoder, &px, 1);
}

/* HSV 转 RGB (hue 0~255) */
static void hsv_to_rgb(uint8_t hue, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region = hue / 43;
    uint8_t remainder = (hue - region * 43) * 6;
    uint8_t p = 0;
    uint8_t q = (255 - remainder);
    uint8_t t = remainder;
    switch (region) {
        case 0: *r = 255; *g = t;   *b = p; break;
        case 1: *r = q;   *g = 255; *b = p; break;
        case 2: *r = p;   *g = 255; *b = t; break;
        case 3: *r = p;   *g = q;   *b = 255; break;
        case 4: *r = t;   *g = p;   *b = 255; break;
        default: *r = 255; *g = p;  *b = q; break;
    }
}

void ws2812_ring_rainbow(uint8_t hue)
{
    for (int i = 0; i < WS2812_LED_RING_NUM; i++) {
        uint8_t r, g, b;
        hsv_to_rgb((hue + i * 32) & 0xFF, &r, &g, &b);
        s_ring_buf[i].r = r;
        s_ring_buf[i].g = g;
        s_ring_buf[i].b = b;
    }
    ws2812_ring_show();
}

void ws2812_ring_breathe(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
    ws2812_ring_fill(r * brightness / 255, g * brightness / 255, b * brightness / 255);
    ws2812_ring_show();
}

void ws2812_ring_lamp_on(uint8_t brightness)
{
    /* 暖白色: R=255, G=200, B=120 */
    ws2812_ring_fill(255 * brightness / 255, 200 * brightness / 255, 120 * brightness / 255);
    ws2812_ring_show();
}

void ws2812_ring_lamp_off(void)
{
    ws2812_ring_fill(0, 0, 0);
    ws2812_ring_show();
}

int ws2812_ring_num(void)
{
    return WS2812_LED_RING_NUM;
}
