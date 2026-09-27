/*
 * mic_inmp441.c — INMP441 I2S 麦克风实现 (I2S1)
 */
#include "mic_inmp441.h"
#include "config.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mic";
static i2s_chan_handle_t s_rx_chan = NULL;

void mic_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    /* 官方 audio_codec.h 是 6 个描述符; 采集侧只有 4 个时, 一旦音频任务被
       解码/发送占住, DMA 缓冲就会读空 → 采集卡顿、上行音频断续。给到 6。 */
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_IN_BCLK_GPIO,
            .ws   = AUDIO_I2S_IN_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = AUDIO_I2S_IN_DIN_GPIO,
        },
    };
    /* INMP441 L/R 接 GND = 左声道 */
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));
    ESP_LOGI(TAG, "INMP441 init: BCLK=GPIO%d, WS=GPIO%d, DIN=GPIO%d",
             AUDIO_I2S_IN_BCLK_GPIO, AUDIO_I2S_IN_WS_GPIO, AUDIO_I2S_IN_DIN_GPIO);
}

void mic_start(void)
{
    if (s_rx_chan) i2s_channel_enable(s_rx_chan);
}

void mic_stop(void)
{
    if (s_rx_chan) i2s_channel_disable(s_rx_chan);
}

size_t mic_read(int16_t *buf, size_t sample_count, int timeout_ms)
{
    if (!s_rx_chan || !buf) return 0;
    size_t bytes_read = 0;
    i2s_channel_read(s_rx_chan, buf, sample_count * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(timeout_ms));
    return bytes_read / sizeof(int16_t);
}
