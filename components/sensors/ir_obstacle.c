/*
 * ir_obstacle.c — 红外避障传感器实现 (左右双通道)
 *
 * 在原始数字读数之上增加了软件消抖 + 释放迟滞:
 *   - 检测到物体: 需连续稳定 IR_DETECT_CONFIRM_MS 才置位 (滤除噪声误触发)
 *   - 物体离开:   需连续稳定 IR_RELEASE_CONFIRM_MS 才清零 (滤除边界临界抖动)
 * 这样在两个传感器的交界区不会左右反复横跳。
 */
#include "ir_obstacle.h"
#include "config.h"
#include "driver/gpio.h"

typedef struct {
    bool     stable;   /* 消抖后的稳定状态: true=检测到 */
    bool     pending;  /* 正在确认中的候选值 */
    uint32_t since;    /* 候选值持续起始时间 */
} ir_ch_t;

static ir_ch_t s_ch[2] = {
    [IR_LEFT]  = { .stable = false, .pending = false, .since = 0 },
    [IR_RIGHT] = { .stable = false, .pending = false, .since = 0 },
};
static bool s_raw[2] = { false, false };

void ir_obstacle_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << SENSOR_IR_LEFT_GPIO) | (1ULL << SENSOR_IR_RIGHT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,    /* 传感器未接/踩不到时保持"未检测到", 防悬空误触发 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    /* 初始读数当一次权威值, 避免上电首帧被当成刚检测到 */
    s_raw[IR_LEFT]  = (gpio_get_level(SENSOR_IR_LEFT_GPIO) == 0);
    s_raw[IR_RIGHT] = (gpio_get_level(SENSOR_IR_RIGHT_GPIO) == 0);
    for (int i = 0; i < 2; i++) {
        s_ch[i].stable  = s_raw[i];
        s_ch[i].pending = s_raw[i];
        s_ch[i].since   = 0;
    }
}

/* 单通道消抖: raw 需持续 confirm_ms 才更新稳定值 */
static void ir_channel_update(int i, bool raw, uint32_t now_ms)
{
    ir_ch_t *ch = &s_ch[i];

    if (raw == ch->stable) {
        ch->pending = raw;
        ch->since   = now_ms;
        return;
    }
    if (ch->pending != raw) {
        ch->pending = raw;
        ch->since   = now_ms;
        return;
    }
    uint32_t confirm = raw ? IR_DETECT_CONFIRM_MS : IR_RELEASE_CONFIRM_MS;
    if (now_ms - ch->since >= confirm) {
        ch->stable = raw;
        ch->since  = now_ms;
    }
}

void ir_obstacle_update(uint32_t now_ms)
{
    s_raw[IR_LEFT]  = (gpio_get_level(SENSOR_IR_LEFT_GPIO) == 0);
    s_raw[IR_RIGHT] = (gpio_get_level(SENSOR_IR_RIGHT_GPIO) == 0);
    ir_channel_update(IR_LEFT,  s_raw[IR_LEFT],  now_ms);
    ir_channel_update(IR_RIGHT, s_raw[IR_RIGHT], now_ms);
}

bool ir_obstacle_left_detected(void)
{
    return s_ch[IR_LEFT].stable;
}

bool ir_obstacle_right_detected(void)
{
    return s_ch[IR_RIGHT].stable;
}

bool ir_obstacle_detected(void)
{
    return s_ch[IR_LEFT].stable || s_ch[IR_RIGHT].stable;
}

bool ir_obstacle_left_raw(void)
{
    return s_raw[IR_LEFT];
}

bool ir_obstacle_right_raw(void)
{
    return s_raw[IR_RIGHT];
}
