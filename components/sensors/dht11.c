/*
 * dht11.c — DHT11 温湿度传感器实现 (单总线协议)
 */
#include "dht11.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "rom/ets_sys.h"

static const char *TAG = "dht11";

#define DHT11_PIN  SENSOR_DHT11_GPIO

static void dht11_set_output(int level)
{
    gpio_set_direction(DHT11_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT11_PIN, level);
}

static void dht11_set_input(void)
{
    gpio_set_direction(DHT11_PIN, GPIO_MODE_INPUT);
}

static int dht11_wait_level(int level, int timeout_us)
{
    int count = 0;
    while (gpio_get_level(DHT11_PIN) != level) {
        if (count++ > timeout_us) return -1;
        ets_delay_us(1);
    }
    return count;
}

void dht11_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << DHT11_PIN),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(DHT11_PIN, 1);
}

bool dht11_read(dht11_data_t *out)
{
    uint8_t data[5] = {0};

    /* 起始信号: 主机拉低 >=18ms */
    dht11_set_output(0);
    ets_delay_us(20000);
    dht11_set_output(1);
    ets_delay_us(30);
    dht11_set_input();

    /* DHT11 响应: 拉低 80us, 拉高 80us */
    if (dht11_wait_level(0, 100) < 0) { ESP_LOGW(TAG, "no response"); return false; }
    if (dht11_wait_level(1, 100) < 0) { ESP_LOGW(TAG, "response timeout"); return false; }
    if (dht11_wait_level(0, 100) < 0) return false;

    /* 读取 40 bit 数据 */
    for (int i = 0; i < 40; i++) {
        if (dht11_wait_level(1, 100) < 0) return false;
        int high_us = dht11_wait_level(0, 100);
        if (high_us < 0) return false;
        /* 高电平 >40us 表示 1, 否则 0 (DHT11: 0=26-28us, 1=70us) */
        if (high_us > 40) {
            data[i / 8] |= (1 << (7 - (i % 8)));
        }
    }

    /* 校验和 */
    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        ESP_LOGW(TAG, "checksum fail: %02X %02X %02X %02X %02X",
                 data[0], data[1], data[2], data[3], data[4]);
        return false;
    }

    out->humidity = data[0] + data[1] * 0.1f;
    out->temperature = data[2] + data[3] * 0.1f;
    return true;
}
