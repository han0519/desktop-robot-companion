/*
 * test_all.c — 综合测试脚本
 * 用法: 将此文件复制为 main/main.c 替换原文件, 编译烧录
 * 依次测试: OLED → 舵机 → WS2812 → 传感器 → 音频
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "config.h"
#include "ssd1305.h"
#include "gfx.h"
#include "servo.h"
#include "ws2812.h"
#include "ir_obstacle.h"
#include "touch_sensor.h"
#include "dht11.h"

static const char *TAG = "test";

void app_main(void)
{
    ESP_LOGI(TAG, "=== Robot Companion - Comprehensive Test ===");

    /* 1. OLED 测试 */
    ESP_LOGI(TAG, "[1/5] OLED test...");
    ssd1305_init(CONFIG_I2C_PORT, CONFIG_I2C_SDA_GPIO, CONFIG_I2C_SCL_GPIO, 100000);
    ssd1305_clear();
    gfx_set_framebuffer(ssd1305_get_framebuffer());
    gfx_draw_string(0, 0, "OLED TEST OK", 1, 1);
    gfx_draw_string(0, 16, "128x64 SSD1305", 1, 1);
    ssd1305_display();
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* 2. 舵机测试 */
    ESP_LOGI(TAG, "[2/5] Servo test...");
    servo_init();
    ssd1305_clear();
    gfx_draw_string(0, 0, "SERVO TEST", 1, 1);
    gfx_draw_string(0, 16, "PAN (GPIO12)", 1, 1);
    ssd1305_display();
    for (int a = 0; a <= 180; a += 30) { servo_set_angle(SERVO_PAN, a); vTaskDelay(200); }
    servo_set_angle(SERVO_PAN, 90);
    gfx_draw_string(0, 28, "TILT (GPIO13)", 1, 1);
    ssd1305_display();
    for (int a = 0; a <= 180; a += 30) { servo_set_angle(SERVO_TILT, a); vTaskDelay(200); }
    servo_set_angle(SERVO_TILT, 90);
    gfx_draw_string(0, 40, "ARM (GPIO38)", 1, 1);
    ssd1305_display();
    for (int a = 0; a <= 180; a += 30) { servo_set_angle(SERVO_ARM, a); vTaskDelay(200); }
    servo_set_angle(SERVO_ARM, 0);

    /* 3. WS2812 测试 */
    ESP_LOGI(TAG, "[3/5] WS2812 test...");
    ws2812_init();
    ssd1305_clear();
    gfx_draw_string(0, 0, "WS2812 TEST", 1, 1);
    gfx_draw_string(0, 16, "Ring: RED", 1, 1);
    ssd1305_display();
    ws2812_ring_fill(255, 0, 0); ws2812_ring_show();
    ws2812_onboard_set(255, 0, 0);
    vTaskDelay(500);
    gfx_draw_string(0, 28, "Ring: GREEN", 1, 1); ssd1305_display();
    ws2812_ring_fill(0, 255, 0); ws2812_ring_show();
    ws2812_onboard_set(0, 255, 0);
    vTaskDelay(500);
    gfx_draw_string(0, 40, "Ring: BLUE", 1, 1); ssd1305_display();
    ws2812_ring_fill(0, 0, 255); ws2812_ring_show();
    ws2812_onboard_set(0, 0, 255);
    vTaskDelay(500);
    ws2812_ring_fill(0, 0, 0); ws2812_ring_show();

    /* 4. 传感器测试 */
    ESP_LOGI(TAG, "[4/5] Sensor test...");
    ir_obstacle_init();
    touch_sensor_init();
    dht11_init();
    ssd1305_clear();
    gfx_draw_string(0, 0, "SENSOR TEST", 1, 1);
    gfx_draw_string(0, 16, "Wave hand @ IR", 1, 1);
    gfx_draw_string(0, 28, "Touch pad", 1, 1);
    ssd1305_display();
    for (int i = 0; i < 50; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "IR:%s TOUCH:%s",
                 ir_obstacle_detected() ? "YES" : "no",
                 touch_sensor_pressed() ? "YES" : "no");
        gfx_fill_rect(0, 40, 128, 12, 0);
        gfx_draw_string(0, 40, buf, 1, 1);
        dht11_data_t d;
        if (dht11_read(&d)) {
            char buf2[32];
            snprintf(buf2, sizeof(buf2), "T:%.0fC H:%.0f%%", d.temperature, d.humidity);
            gfx_fill_rect(0, 52, 128, 12, 0);
            gfx_draw_string(0, 52, buf2, 1, 1);
        }
        ssd1305_display();
        vTaskDelay(100);
    }

    /* 5. 完成 */
    ssd1305_clear();
    gfx_draw_string(0, 20, "ALL TESTS DONE", 1, 1);
    gfx_draw_string(0, 36, "Check serial log", 1, 1);
    ssd1305_display();
    ESP_LOGI(TAG, "=== All tests complete ===");
}
