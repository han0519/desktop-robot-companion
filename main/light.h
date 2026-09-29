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
#include <stddef.h>

/* ================= 8 灯珠环形灯效 =================
 *
 * 顺序与 docs/led_preview.html 里的预览完全一致(触摸双击按这个顺序轮换)。
 * 设计原则(踩过坑, 写下来免得后人乱改):
 *   · 颜色一律用 HSV 现算, 不存表 —— 8 颗灯算这点量对 CPU 可忽略, 但省 flash
 *   · 【音乐律动/音乐频谱】的颜色是"绑在位置"上的(音量只决定条多长),
 *     所以低音量时是青蓝、满音量顶端才出现绿色 —— 这是刻意跟参考产品对齐的
 *   · 随机类(火焰/星空)需要跨帧状态, 存在 s 里而不是函数里
 */
typedef enum {
    LIGHT_OFF = 0,      /* 关灯 */
    LIGHT_MONO,         /* 单色常亮 */
    LIGHT_BREATHE,      /* 单色呼吸 (2.4s) */
    LIGHT_RAINBOW,      /* 彩虹环 (8 灯均分色相, 缓慢旋转) */
    LIGHT_RAINBOW_BR,   /* 彩虹呼吸 (彩虹 + 整体亮度呼吸 3.2s) */
    LIGHT_CHASE,        /* 追光 (亮点沿环跑 + 3 颗拖尾) */
    LIGHT_TWIN,         /* 双点对撞 (两个亮点相对旋转) */
    LIGHT_MIRROR,       /* 镜像呼吸 (从 0 号向两侧对称点亮) */
    LIGHT_PULSE,        /* 脉冲扩散 (一圈光波向外扩) */
    LIGHT_FIRE,         /* 火焰 (橙红随机跳动) */
    LIGHT_STARRY,       /* 星空闪烁 (随机点亮再慢慢淡出) */
    LIGHT_LEVEL,        /* 电平环 (跟音量填格, 不跳) */
    LIGHT_MUSIC,        /* ★ 音乐律动 (放歌自动进: 环形音频条 + 峰值点) */
    LIGHT_MUSIC_BANDS,  /* ★ 音乐频谱 (8 段: 每颗灯管一个频段) */
    LIGHT_POLICE,       /* 警车爆闪 (红蓝交替) */
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
/* 只改主色、【不】切灯效 —— light_set_color 会强制切到单色, 配网结束要
 * 恢复原状态时(先恢复主色再恢复效果)必须用这个 */
void light_set_rgb(uint8_t r, uint8_t g, uint8_t b);
void light_get_color(uint8_t *r, uint8_t *g, uint8_t *b);

/* ---- 音乐律动(LIGHT_MUSIC) ----
 * 放歌时由 music 模块调用: start 进律动(记住当前灯效), 每帧喂电平,
 * 结束 stop 自动恢复。电平取 0~1 的峰值, 灯环内部做"快攻慢放"包络,
 * 看起来是跟着节奏弹, 而不是频闪。 */
void light_music_start(void);
void light_music_stop(void);
void light_music_level(float level01);   /* 0..1, 只喂电平(旧接口) */
/* ★ 推荐用这个: 直接把解码后的 PCM 喂进来, 里面除了算电平, 还会做 8 段
   "滤波器组"分析(低/高各段能量), 音乐频谱灯效就靠它。每帧调用开销可忽略。 */
void light_music_pcm(const int16_t *pcm, size_t frames);
/* 麦克风 PCM 喂频谱: 没放歌时「音乐频谱」也能跟着环境声音跳(内部自判, 无开销) */
void light_mic_pcm(const int16_t *pcm, size_t frames);

/* 供网页/串口显示: 当前灯效的一行中文描述 */
const char *light_state_str(void);
