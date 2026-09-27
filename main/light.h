/*
 * light.h — WS2812 灯效引擎
 *
 * 把"灯效"从硬件驱动里独立出来: ws2812 组件只负责把颜色送到灯珠, 这里负责
 * 让灯动起来(呼吸/渐变/彩虹/爆闪), 并由一个 ~40Hz 的后台任务统一刷新。
 *
 * 灯效可以通过两种方式切换:
 *   1. 小智 AI 通过 MCP 工具调用 (见 mcp.c: self.light.set_effect)
 *   2. 头部触摸双击 (见 main.c 的手势识别)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    LIGHT_OFF = 0,      /* 关灯 */
    LIGHT_MONO,         /* 单色常亮 */
    LIGHT_BREATHE,      /* 单色呼吸 */
    LIGHT_GRADIENT,     /* 渐变(颜色沿灯环过渡并缓慢流动) */
    LIGHT_RAINBOW,      /* 多色呼吸(彩虹分布 + 整体亮度呼吸) */
    LIGHT_POLICE,       /* 警车爆闪(红蓝交替) */
    LIGHT_MUSIC,        /* ★ 音乐律动: 灯环跟着音乐电平转(放歌时自动进入) */
    LIGHT_COUNT
} light_effect_t;

/* 初始化并启动灯效任务 */
void light_init(void);

/* ---- 灯效切换 ---- */
void           light_set_effect(light_effect_t e);
light_effect_t light_get_effect(void);
void           light_next_effect(void);          /* 切到下一个灯效(双击用) */
const char    *light_effect_name(light_effect_t e);   /* 英文标识 */
const char    *light_effect_cn(light_effect_t e);     /* 中文名 */
/* 按名字匹配灯效(中英文都认), 成功返回 true */
bool           light_effect_from_name(const char *name, light_effect_t *out);

/* ---- 亮度 0~100 ---- */
void light_set_brightness(int percent);
int  light_get_brightness(void);

/* ---- 主色(单色/呼吸用) ---- */
void light_set_color(uint8_t r, uint8_t g, uint8_t b);
void light_get_color(uint8_t *r, uint8_t *g, uint8_t *b);

/* ---- 音乐律动(LIGHT_MUSIC) ----
 * 放歌时由 music 模块调用: start 进律动(记住当前灯效), 每帧喂电平,
 * 结束 stop 自动恢复。电平取 0~1 的峰值, 灯环内部做"快攻慢放"包络,
 * 看起来是跟着节奏弹, 而不是频闪。 */
void light_music_start(void);
void light_music_stop(void);
void light_music_level(float level01);   /* 0..1, 音乐解码循环里每帧调用 */

/* 供网页/串口显示: 当前灯效的一行中文描述 */
const char *light_state_str(void);
