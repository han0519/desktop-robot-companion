/*
 * ws2812.h — WS2812 灯环 + 板载RGB 驱动 (新 RMT TX API)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

/* 初始化 WS2812 (灯环 + 板载灯) */
void ws2812_init(void);

/* 设置灯环某个灯的颜色 (0~num-1) */
void ws2812_ring_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b);

/* 设置灯环所有灯的颜色 */
void ws2812_ring_fill(uint8_t r, uint8_t g, uint8_t b);

/* 灯环刷新 (把缓冲区数据发出去) */
void ws2812_ring_show(void);

/* 设置板载 RGB 灯颜色 */
void ws2812_onboard_set(uint8_t r, uint8_t g, uint8_t b);

/* 灯环彩虹渐变效果 (hue 0~255) */
void ws2812_ring_rainbow(uint8_t hue);

/* 灯环呼吸灯效果 (单颜色, brightness 0~255) */
void ws2812_ring_breathe(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);

/* 台灯模式: 暖白色全亮 (亮度 0~255) */
void ws2812_ring_lamp_on(uint8_t brightness);

/* 台灯模式: 关闭 */
void ws2812_ring_lamp_off(void);

/* 获取灯环灯数 */
int ws2812_ring_num(void);
