/*
 * light.c — WS2812 灯效引擎实现
 *
 * 一个 ~40Hz 的后台任务算出每一帧的颜色, 然后整环刷新。
 * 所有效果都由"时间"驱动, 不需要外部喂帧。
 *
 * ★ 限流: 8 颗 WS2812 全白约 480mA。实测把它接到开发板的 5V 上之后,
 *   灯一亮就把电压拉塌, ESP32 直接掉电复位
 *   (串口表现为 rst:0x1 (POWERON), boot:0xb —— strapping 电平都变了)。
 *   所以这里对每一帧做"总亮度预算"限流: 超预算就整帧等比缩小,
 *   既能保留亮度控制, 又不会让总电流超过预算。
 */
#include "light.h"
#include "ws2812.h"
#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "xiaozhi.h"      /* ai_client_mic_rms(): 电平环在没放歌时跟麦克风 */

static const char *TAG = "light";

#define LIGHT_TASK_STACK   3072
#define LIGHT_FRAME_MS     25          /* 40Hz */
#define PI_F               3.14159265f
#define LIGHT_MAX_LED      WS2812_LED_RING_NUM

/* 音乐频谱的频段数(8 灯环 = 8 段, 一颗灯一段) */
#define LIGHT_BANDS        8

/* 电流预算(近似): 每颗灯的"最大通道值"之和。
   WS2812 单颗静态约 1mA, 全色约 40mA; 8 颗全白 ≈ 320mA + 基础 ≈ 480mA。
   灯环已改由独立 5V 供电, 不再受开发板电源限制, 所以预算给足(num*255 = 不限流)。
   如果哪天又改成从开发板取电, 一定要把这个值调回 num*80 左右,
   否则全亮(约 480mA)会把电压拉塌 —— 表现为 rst:0x1 (POWERON) 重启或屏幕雪花。 */
#define LIGHT_CURRENT_BUDGET  (LIGHT_MAX_LED * 255)

typedef struct {
    const char *id;    /* 英文标识(MCP 用) */
    const char *cn;    /* 中文名 */
} light_name_t;

static const light_name_t NAMES[LIGHT_COUNT] = {
    { "off",            "关灯"     },
    { "mono",           "单色常亮" },
    { "breathe",        "呼吸"     },
    { "rainbow",        "彩虹环"   },
    { "rainbow_breath", "彩虹呼吸" },
    { "chase",          "追光"     },
    { "twin",           "双点对撞" },
    { "mirror",         "镜像呼吸" },
    { "pulse",          "脉冲扩散" },
    { "fire",           "火焰"     },
    { "starry",         "星空闪烁" },
    { "level",          "电平环"   },
    { "music",          "音乐律动" },
    { "music_bands",    "音乐频谱" },
    { "police",         "警车爆闪" },
};

static struct {
    /* ★ 这些字段由【别的任务】写(MCP/WS 回调、httpd 回调、音乐任务),
       而 light_task 每 25ms 读 —— 不加 volatile 编译器可能把读缓存进寄存器,
       表现为"改了灯效要过一会才生效"这类偶发问题。 */
    volatile light_effect_t effect;
    volatile int      brightness;  /* 0~100 */
    volatile uint8_t  r, g, b;     /* 主色 */
    /* ---- 音乐律动 ---- */
    volatile float music_lvl;      /* 音乐模块喂进来的最新电平 0..1 */
    float          music_env;      /* 平滑包络(快攻慢放), 让鼓点"弹"起来 */
    float          music_peak;     /* 峰值点(保持最高 + 缓慢下落) */
    volatile float band[LIGHT_BANDS];  /* 8 段频谱的包络 0..1 */
    volatile light_effect_t saved_effect;   /* 进律动前的灯效(歌停恢复用) */
    volatile bool  music_on;
    uint32_t       last_feed_ms;   /* 音乐模块最后一次喂数据的时间(判"停喂"用) */
    /* ---- 随机类灯效的跨帧状态(必须存在这里, 不能放函数里当局部变量) ---- */
    float fire[LIGHT_MAX_LED];
    float star[LIGHT_MAX_LED];
    float star_t[LIGHT_MAX_LED];
    int   star_slot;               /* 上次挑灯的时间槽, 用来限速 */
} s = {
    /* 开机默认【关灯】—— 灯只在两种情况下亮:
       ① 用户/AI 明确开灯(网页/AI/摸头双击)
       ② 放歌时自动进入音乐律动(可随时说"关灯"关掉, 歌停也不会再亮) */
    .effect = LIGHT_OFF,
    .brightness = 60,        /* 灯环独立供电, 亮度可以放开 */
    .r = 255, .g = 160, .b = 40,
};

/* 当前帧的工作缓冲(先算完、限流、再一次性推给灯珠) */
static rgb_t s_frame[LIGHT_MAX_LED];

static inline uint8_t scale(uint8_t v, float k)
{
    int x = (int)(v * k);
    if (x < 0) x = 0;
    if (x > 255) x = 255;
    return (uint8_t)x;
}

static void frame_fill(int n, uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < n; i++) {
        s_frame[i].r = r; s_frame[i].g = g; s_frame[i].b = b;
    }
}

/* hue 0~360, sat 0~100, val 0~1 -> rgb */
static void hsv_to_rgb(float h, float sat, float val, uint8_t *r, uint8_t *g, uint8_t *b)
{
    while (h < 0)       h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    float s01 = sat / 100.0f;
    float c = val * s01;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = val - c;
    float rf = 0, gf = 0, bf = 0;
    if      (h <  60) { rf = c; gf = x; }
    else if (h < 120) { rf = x; gf = c; }
    else if (h < 180) { gf = c; bf = x; }
    else if (h < 240) { gf = x; bf = c; }
    else if (h < 300) { rf = x; bf = c; }
    else              { rf = c; bf = x; }
    *r = (uint8_t)((rf + m) * 255.0f + 0.5f);
    *g = (uint8_t)((gf + m) * 255.0f + 0.5f);
    *b = (uint8_t)((bf + m) * 255.0f + 0.5f);
}

/* ================= 音乐条色阶(与参考产品对齐) =================
 * 颜色【固定绑在"条内位置"】上, 音量只决定条有多长 —— 这是关键:
 * 低音量时看到的是条底那截(青蓝), 声音越大越往上走, 满音量顶端才出现绿色。
 * 位置 0 = 条底, 1 = 条顶。色阶: 青→蓝→紫→粉→红→橙→黄→绿。
 * 想换成"常见款"(底绿、顶紫青) 只要把 p 换成 1-p 即可。 */
static const struct { float p; uint8_t r, g, b; } BAR_STOPS[] = {
    { 0.00f,  46, 219, 255 },   /* 青 */
    { 0.14f,  71, 133, 255 },   /* 蓝 */
    { 0.28f, 158,  77, 255 },   /* 紫 */
    { 0.42f, 255,  64, 158 },   /* 粉 */
    { 0.56f, 255,  77,  51 },   /* 红 */
    { 0.70f, 255, 148,   0 },   /* 橙 */
    { 0.84f, 255, 224,   0 },   /* 黄 */
    { 1.00f,  26, 235, 102 },   /* 绿 */
};
#define BAR_STOP_N (sizeof(BAR_STOPS) / sizeof(BAR_STOPS[0]))

static void bar_color(float p, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    for (size_t i = 0; i + 1 < BAR_STOP_N; i++) {
        if (p <= BAR_STOPS[i + 1].p || i + 2 == BAR_STOP_N) {
            float span = BAR_STOPS[i + 1].p - BAR_STOPS[i].p;
            float k = span > 0 ? (p - BAR_STOPS[i].p) / span : 0.0f;
            if (k < 0) k = 0;
            if (k > 1) k = 1;
            *r = (uint8_t)(BAR_STOPS[i].r + (BAR_STOPS[i + 1].r - BAR_STOPS[i].r) * k);
            *g = (uint8_t)(BAR_STOPS[i].g + (BAR_STOPS[i + 1].g - BAR_STOPS[i].g) * k);
            *b = (uint8_t)(BAR_STOPS[i].b + (BAR_STOPS[i + 1].b - BAR_STOPS[i].b) * k);
            return;
        }
    }
}

/* ================= 音乐频谱: 8 段滤波器组 =================
 * 做法: 8 个一阶低通【串起来】, 截止频率一个比一个低(11k → 240Hz)。
 * 每一级的"输入 - 输出"就是那一段(带通)的能量 —— 相当于一个很省的带通滤波器组,
 * 每样本 8 次乘加(44.1kHz 下约 35 万次/秒), 在 ESP32-S3 上完全可忽略。
 * 比做 FFT 省得多, 而"每颗灯管一个频段"用这个精度足够了。 */
static const float LB_FC_HZ[LIGHT_BANDS] = {
    11000.0f, 6500.0f, 3800.0f, 2200.0f, 1300.0f, 750.0f, 430.0f, 240.0f
};
#define LB_FS  44100.0f          /* 按 44.1kHz 标定; 换采样率只会让频段整体平移, 不影响观感 */
static float lb_a[LIGHT_BANDS];  /* 一阶低通系数 */
static float lb_z[LIGHT_BANDS];  /* 各级状态 */
static float lb_band[LIGHT_BANDS]; /* 各频段的能量包络(喂给 s.band) */
static float lb_fs_used;         /* 系数当前按哪个采样率算的(换数据源要重算) */
static bool  lb_ready;
/* ★ 滤波器组状态被两个任务共享: light_music_pcm(音乐任务) 和
 * light_mic_pcm(音频任务)。门禁 if(music_on||effect!=BANDS) 存在竞态窗口
 * (放歌瞬间两边同时进来, lb_z/lb_a 被交替写 → 频谱乱闪, 还是标准意义的
 * data race), 必须互斥。临界区每帧只有几百次乘加, mutex 开销可忽略。 */
static SemaphoreHandle_t lb_mux;

static void lb_lock(void)   { xSemaphoreTake(lb_mux, portMAX_DELAY); }
static void lb_unlock(void) { xSemaphoreGive(lb_mux); }

/* 采样率变了(音乐 44.1k / 麦克风 16k)必须重算系数, 否则频段整体偏掉 */
static void lb_init_for(float fs)
{
    lb_lock();
    if (!(lb_ready && lb_fs_used == fs)) {
        lb_ready   = true;
        lb_fs_used = fs;
        for (int i = 0; i < LIGHT_BANDS; i++)
            lb_a[i] = 1.0f - expf(-2.0f * PI_F * LB_FC_HZ[i] / fs);
    }
    lb_unlock();
}

/* 级联一阶低通: 每级的(输入-输出)就是那一段的能量。
 *    包络的时间常数按【秒】设计, 换算成每样本的系数(44.1kHz):
 *    攻 ~30ms → 0.0008/样本, 放 ~250ms → 0.00009/样本。
 *    (若按帧算会跟采样率耦合, 采样率一变手感就全变 —— 踩过。) */
static void lb_process(const int16_t *pcm, size_t frames)
{
    lb_lock();
    for (size_t i = 0; i < frames; i++) {
        float v = (float)pcm[i] / 32768.0f;
        for (int b = 0; b < LIGHT_BANDS; b++) {
            lb_z[b] += lb_a[b] * (v - lb_z[b]);
            float band = fabsf(v - lb_z[b]);   /* 本段(带通)的瞬时能量 */
            v = lb_z[b];                       /* 传给下一级 */
            lb_band[b] += (band - lb_band[b]) * (band > lb_band[b] ? 0.0008f : 0.00009f);
        }
    }
    /* 归一化: 带通差值的幅度在 0.01~0.1 量级, 乘个增益让灯有得看(可调) */
    for (int b = 0; b < LIGHT_BANDS; b++) {
        float v = lb_band[b] * 12.0f;
        if (v > 1.0f) v = 1.0f;
        s.band[b] = v;
    }
    lb_unlock();
}

/* 把当前帧按电流预算等比缩小 */
static void apply_current_limit(int n)
{
    int sum = 0;
    for (int i = 0; i < n; i++) {
        int mx = s_frame[i].r;
        if (s_frame[i].g > mx) mx = s_frame[i].g;
        if (s_frame[i].b > mx) mx = s_frame[i].b;
        sum += mx;
    }
    if (sum <= LIGHT_CURRENT_BUDGET || sum == 0) return;
    for (int i = 0; i < n; i++) {
        s_frame[i].r = (uint8_t)((int)s_frame[i].r * LIGHT_CURRENT_BUDGET / sum);
        s_frame[i].g = (uint8_t)((int)s_frame[i].g * LIGHT_CURRENT_BUDGET / sum);
        s_frame[i].b = (uint8_t)((int)s_frame[i].b * LIGHT_CURRENT_BUDGET / sum);
    }
}

/* 主色 → 色相(0~360)。追光/镜像/脉冲这些"跟主色走"的灯效用。 */
static float main_hue(void)
{
    float r = s.r / 255.0f, g = s.g / 255.0f, b = s.b / 255.0f;
    float mx = fmaxf(fmaxf(r, g), b), mn = fminf(fminf(r, g), b), d = mx - mn;
    if (d <= 0.0001f) return 0.0f;
    if (mx == r)      return 60.0f * fmodf((g - b) / d + 6.0f, 6.0f);
    else if (mx == g) return 60.0f * ((b - r) / d + 2.0f);
    else              return 60.0f * ((r - g) / d + 4.0f);
}

/* 把音乐电平推进包络(快攻慢放) —— MUSIC/LEVEL 共用,
 * 三个跟声音有关的灯效"弹跳感"必须一致, 所以只写一份。 */
static float music_env_step(void)
{
    /* ★ 音乐暂停/停止之后, 音乐任务不再喂 PCM, 但灯效可能仍是律动/频谱 ——
       如果不主动把电平衰减到 0, 条和峰值点会【永远停在暂停前那一刻】
       (实机复现: 暂停后灯环定格不落)。这里做"停喂检测": 超过 300ms 没数据
       就每帧衰减 10%, 约半秒落到底。 */
    uint32_t now = esp_timer_get_time() / 1000;
    if (s.last_feed_ms && (now - s.last_feed_ms) > 300) {
        s.music_lvl *= 0.90f;
        if (s.music_lvl < 0.002f) s.music_lvl = 0.0f;
        for (int i = 0; i < LIGHT_BANDS; i++) {
            s.band[i] *= 0.90f;
            if (s.band[i] < 0.002f) s.band[i] = 0.0f;
        }
    }
    float lv = s.music_lvl;
    if (lv > s.music_env) s.music_env += (lv - s.music_env) * 0.55f;   /* 快攻 */
    else                  s.music_env += (lv - s.music_env) * 0.10f;   /* 慢放 */
    if (s.music_env < 0.015f) s.music_env = 0.015f;                    /* 别全黑 */
    return s.music_env;
}

/* 环形条/频谱的生长顺序: 0 号灯在正上方, 所以"底部"是 4 号,
 * 从它向两侧一起长 —— 看起来就是一条音频条从中间立起来。 */
static const int RING_ORDER[8] = { 4, 3, 5, 2, 6, 1, 7, 0 };

/* 按当前灯效算出这一帧 */
static void compute_frame(double t, int n)
{
    float br = (float)s.brightness / 100.0f;

    switch (s.effect) {
    case LIGHT_OFF:
        frame_fill(n, 0, 0, 0);
        break;

    case LIGHT_MONO:
        frame_fill(n, scale(s.r, br), scale(s.g, br), scale(s.b, br));
        break;

    case LIGHT_BREATHE: {
        /* 单色呼吸: 周期 2.4s, 最暗不低于 12%, 免得看起来像关了 */
        float k = 0.12f + 0.88f * (0.5f + 0.5f * sinf(t * 2.0f * PI_F / 2.4f));
        frame_fill(n, scale(s.r, br * k), scale(s.g, br * k), scale(s.b, br * k));
        break;
    }

    case LIGHT_RAINBOW: {
        /* 彩虹环: 8 灯均分 360° 色相, 整环缓慢旋转 */
        for (int i = 0; i < n; i++) {
            uint8_t r, g, b;
            hsv_to_rgb(t * 45.0f + 360.0f * (float)i / (float)n, 100.0f, br, &r, &g, &b);
            s_frame[i].r = r; s_frame[i].g = g; s_frame[i].b = b;
        }
        break;
    }

    case LIGHT_RAINBOW_BR: {
        /* 彩虹呼吸: 彩虹分布 + 整体一起呼吸(3.2s), 比"彩虹环"柔和, 适合待机 */
        float k = 0.20f + 0.80f * (0.5f + 0.5f * sinf(t * 2.0f * PI_F / 3.2f));
        for (int i = 0; i < n; i++) {
            uint8_t r, g, b;
            hsv_to_rgb(t * 30.0f + 360.0f * (float)i / (float)n, 100.0f, br * k, &r, &g, &b);
            s_frame[i].r = r; s_frame[i].g = g; s_frame[i].b = b;
        }
        break;
    }

    case LIGHT_CHASE: {
        /* 追光: 一个亮点沿环跑(2.5 圈/秒), 后面带 3 颗拖尾 */
        int head = ((int)(t * 2.5f * (float)n)) % n;
        for (int i = 0; i < n; i++) {
            int d = (head - i + n) % n;
            float k = (d < 4) ? (1.0f - 0.28f * (float)d) : 0.0f;
            uint8_t r, g, b;
            hsv_to_rgb(main_hue(), 100.0f, br * k, &r, &g, &b);
            s_frame[i].r = r; s_frame[i].g = g; s_frame[i].b = b;
        }
        break;
    }

    case LIGHT_TWIN: {
        /* 双点对撞: 两个亮点相隔半圈同向旋转, 各带 2 颗拖尾, 比追光热闹 */
        int h1 = ((int)(t * 1.6f * (float)n)) % n;
        for (int i = 0; i < n; i++) {
            int d1 = (h1 - i + n) % n;
            int d2 = (h1 + n / 2 - i + n) % n;
            float k = 0.0f;
            if (d1 < 3) { float v = 1.0f - 0.34f * (float)d1; if (v > k) k = v; }
            if (d2 < 3) { float v = 1.0f - 0.34f * (float)d2; if (v > k) k = v; }
            uint8_t r, g, b;
            hsv_to_rgb(main_hue(), 100.0f, br * k, &r, &g, &b);
            s_frame[i].r = r; s_frame[i].g = g; s_frame[i].b = b;
        }
        break;
    }

    case LIGHT_MIRROR: {
        /* 镜像呼吸: 以 0 号灯为中心向两侧对称点亮, 像从中间张开又合上(1.8s) */
        float brs = 0.5f + 0.5f * sinf(t * 2.0f * PI_F / 1.8f);
        for (int i = 0; i < n; i++) {
            int d = (i <= n - i) ? i : (n - i);
            float k = (1.0f - 0.20f * (float)d) * (0.25f + 0.75f * brs);
            uint8_t r, g, b;
            hsv_to_rgb(main_hue(), 100.0f, br * k, &r, &g, &b);
            s_frame[i].r = r; s_frame[i].g = g; s_frame[i].b = b;
        }
        break;
    }

    case LIGHT_PULSE: {
        /* 脉冲扩散: 一圈光波从 0 号灯向两侧扩散出去, 1.4s 一次 */
        float w = fmodf(t / 1.4f, 1.0f) * (float)n;
        for (int i = 0; i < n; i++) {
            float d1 = fmodf((float)i - w + 2.0f * n, (float)n);
            float d2 = fmodf(w - (float)i + 2.0f * n, (float)n);
            float d = (d1 < d2) ? d1 : d2;             /* 环上最短距离 */
            float k = 1.0f - fabsf(d) / 2.2f;
            if (k < 0) k = 0;
            uint8_t r, g, b;
            hsv_to_rgb(main_hue(), 100.0f, br * k, &r, &g, &b);
            s_frame[i].r = r; s_frame[i].g = g; s_frame[i].b = b;
        }
        break;
    }

    case LIGHT_FIRE: {
        /* 火焰: 每颗灯自己随机跳(带惯性, 免得变成雪花噪点), 橙红, 有生气又不刺眼 */
        for (int i = 0; i < n; i++) {
            float target = 0.45f + (float)rand() / ((float)RAND_MAX + 1.0f) * 0.55f;
            s.fire[i] += (target - s.fire[i]) * 0.45f;
            float h = 8.0f + 26.0f * s.fire[i];
            uint8_t r, g, b;
            hsv_to_rgb(h, 100.0f, br * s.fire[i], &r, &g, &b);
            s_frame[i].r = r; s_frame[i].g = g; s_frame[i].b = b;
        }
        break;
    }

    case LIGHT_STARRY: {
        /* 星空闪烁: 每 0.25s 随机点亮一颗, 然后慢慢淡掉 —— 很安静的待机效果 */
        int slot = (int)(t / 0.25f);
        if (slot != s.star_slot) {
            s.star_slot = slot;
            int i = rand() % n;
            s.star[i] = 0.6f + (float)rand() / ((float)RAND_MAX + 1.0f) * 0.4f;
            s.star_t[i] = 0.35f;
        }
        for (int i = 0; i < n; i++) {
            s.star_t[i] -= 0.025f;                     /* 一帧 25ms */
            if (s.star_t[i] <= 0) {
                s.star[i] *= 0.93f;
                if (s.star[i] < 0.03f) s.star[i] = 0.0f;
            }
            uint8_t r, g, b;
            hsv_to_rgb(main_hue(), 65.0f, br * s.star[i], &r, &g, &b);
            s_frame[i].r = r; s_frame[i].g = g; s_frame[i].b = b;
        }
        break;
    }

    case LIGHT_LEVEL: {
        /* 电平环: 跟着音量填格(从 0 号灯顺时针), 不跳, 安静显示当前音量。
           放歌时用音乐电平; 平时用麦克风电平 —— 你说话它就跟着跳。 */
        float lv;
        if (s.music_on) {
            lv = music_env_step();
        } else {
            /* ★ 标定(实测, 不是拍的): ai_client_mic_rms 返回的是【增益后】的 rms,
               安静时 ~600、正常说话 ~1840、大声 6000+。原来的 /8000 会让说话
               最多只有 0.23, 灯几乎不动 —— 必须先扣底噪再拉满量程。 */
            static float mic_env;
            float raw = (ai_client_mic_rms() - 600.0f) / 5400.0f;
            if (raw < 0.0f) raw = 0.0f;
            if (raw > 1.0f) raw = 1.0f;
            /* 平滑: 快攻慢放, 说话有"弹"的感觉, 停下不会立刻熄灭 */
            mic_env += (raw - mic_env) * (raw > mic_env ? 0.5f : 0.12f);
            lv = mic_env;
        }
        int lit = 1 + (int)(lv * (float)(n - 1) + 0.5f);
        if (lit > n) lit = n;
        for (int i = 0; i < n; i++) {
            float p = (n > 1) ? (float)i / (float)(n - 1) : 1.0f;
            uint8_t r, g, b;
            bar_color(p, &r, &g, &b);
            float k = (i < lit - 1) ? 1.0f
                    : ((i == lit - 1) ? 1.0f - (lv * (float)(n - 1) - (float)(lit - 1)) * 0.7f
                                      : 0.0f);
            k = 0.25f + 0.75f * k;
            s_frame[i].r = scale(r, br * k);
            s_frame[i].g = scale(g, br * k);
            s_frame[i].b = scale(b, br * k);
        }
        break;
    }

    case LIGHT_POLICE: {
        /* 警车爆闪: 0.11s 红 / 0.11s 蓝, 相位末尾极短全灭, 爆闪感更强 */
        int phase = (int)(t / 0.11f);
        float k = br;
        if (fmodf(t, 0.11f) > 0.095f) k = 0.0f;
        if (phase & 1) frame_fill(n, 0, 0, scale(255, k));
        else           frame_fill(n, scale(255, k), 0, 0);
        break;
    }

    case LIGHT_MUSIC: {
        /* ★★ 音乐律动(环形音频条) ★★ —— 参考图那种"音乐节奏氛围灯"的环形版
         * 音量越大条越长(从底部 4 号灯向两侧一起长), 颜色【绑在位置】上:
         * 条底青 → 蓝 → 紫 → 粉 → 红 → 橙 → 黄 → 顶部绿(bar_color)。
         * 条上面那颗单独的亮青灯是【峰值点】, 停在最高处再慢慢滑下来。
         * 包络快攻慢放: 鼓点立刻弹起、声音停了慢慢落下 —— 跟着节奏跳而不是频闪。 */
        float env = music_env_step();
        s.music_peak = fmaxf(env, s.music_peak - 0.0225f);   /* 峰值每帧下落 0.9/s */

        int bar = (int)(env * (float)(n - 1) + 0.5f);
        if (bar > n - 1) bar = n - 1;
        int pk = (int)(s.music_peak * (float)(n - 1) + 0.5f);
        if (pk > n - 1) pk = n - 1;

        for (int i = 0; i < n; i++) s_frame[i].r = s_frame[i].g = s_frame[i].b = 0;
        for (int k = 0; k <= bar && k < n; k++) {
            int idx = RING_ORDER[k & 7];
            if (idx >= n) continue;
            float p = (n > 1) ? (float)k / (float)(n - 1) : 1.0f;
            uint8_t r, g, b;
            bar_color(p, &r, &g, &b);
            /* 底部那颗留一点底亮, 免得安静时整个环全黑 */
            float v = (k == 0) ? fmaxf(0.35f, 0.35f + 0.65f * env)
                               : (0.55f + 0.45f * env);
            s_frame[idx].r = scale(r, br * v);
            s_frame[idx].g = scale(g, br * v);
            s_frame[idx].b = scale(b, br * v);
        }
        if (pk > bar && pk < n) {                            /* 峰值点(亮青) */
            int idx = RING_ORDER[pk & 7];
            s_frame[idx].r = scale(140, br);
            s_frame[idx].g = scale(242, br);
            s_frame[idx].b = scale(255, br);
        }
        break;
    }

    case LIGHT_MUSIC_BANDS: {
        /* ★★ 音乐频谱(8 段) ★★ —— 8 颗灯各管一个频段(低频在底部, 向两侧往高频走),
         * 每颗的亮度/颜色跟它那个频段的能量走(同一套色带, 能量越大越靠红)。
         * 频段数据由 light_music_pcm() 里的滤波器组实时算出来, 不用 FFT。 */
        for (int i = 0; i < n; i++) s_frame[i].r = s_frame[i].g = s_frame[i].b = 0;
        for (int i = 0; i < n && i < LIGHT_BANDS; i++) {
            float v = s.band[i];
            if (v < 0.03f) continue;
            if (v > 1.0f) v = 1.0f;
            int idx = RING_ORDER[i & 7];
            if (idx >= n) continue;
            uint8_t r, g, b;
            bar_color(v, &r, &g, &b);
            float k = 0.30f + 0.70f * v;
            s_frame[idx].r = scale(r, br * k);
            s_frame[idx].g = scale(g, br * k);
            s_frame[idx].b = scale(b, br * k);
        }
        break;
    }

    default:
        frame_fill(n, 0, 0, 0);
        break;
    }
}

static void light_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    TickType_t last = xTaskGetTickCount();
    int n = ws2812_ring_num();
    if (n <= 0) n = 1;
    if (n > LIGHT_MAX_LED) n = LIGHT_MAX_LED;

    while (1) {
        /* ★ 用 double: float 只有 24 位有效数字, t ≈ 88000 秒(约一天)后,
           t*速度*n 这类乘法的精度开始崩, 追光/脉冲会出现卡顿跳变。
           每帧几次 double 运算对 CPU 可忽略(ESP32-S3 有硬件 FPU, double 是
           软件的但量太小)。 */
        double t = (double)(esp_timer_get_time() - t0) / 1000000.0;
        compute_frame(t, n);
        apply_current_limit(n);
        for (int i = 0; i < n; i++)
            ws2812_ring_set_pixel(i, s_frame[i].r, s_frame[i].g, s_frame[i].b);
        ws2812_ring_show();
        /* 板载灯跟着走, 没接灯环也能看出效果 */
        ws2812_onboard_set(s_frame[0].r, s_frame[0].g, s_frame[0].b);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(LIGHT_FRAME_MS));
    }
}

void light_init(void)
{
    ws2812_init();
    /* ★ 先强制刷一次全黑再启动任务。
       上电瞬间数据脚电平不确定, 灯珠可能锁存出一个亮态("明明设了关灯还是亮着")。
       RMT 通道刚使能时也一样, 不显式写一次就是不确定状态。 */
    ws2812_ring_fill(0, 0, 0);
    ws2812_ring_show();
    ws2812_onboard_set(0, 0, 0);

    lb_mux = xSemaphoreCreateMutex();      /* 滤波器组: 音乐/音频任务互斥(见 lb_process) */

    if (xTaskCreatePinnedToCore(light_task, "light", LIGHT_TASK_STACK,
                                NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "灯效任务创建失败(内存不足) —— 灯会停在当前状态不动");
    }
    ESP_LOGI(TAG, "灯效引擎启动: %s, 亮度 %d%% (单帧电流预算 %d)",
             light_effect_cn(s.effect), s.brightness, LIGHT_CURRENT_BUDGET);
}

void light_set_effect(light_effect_t e)
{
    if (e < 0 || e >= LIGHT_COUNT) return;
    if (e == s.effect) return;
    s.effect = e;
    ESP_LOGI(TAG, "灯效 -> %s (亮度 %d%%)", light_effect_cn(e), s.brightness);
}

light_effect_t light_get_effect(void) { return s.effect; }

void light_next_effect(void)
{
    light_set_effect((light_effect_t)((s.effect + 1) % LIGHT_COUNT));
}

const char *light_effect_name(light_effect_t e)
{
    if (e < 0 || e >= LIGHT_COUNT) return "?";
    return NAMES[e].id;
}

const char *light_effect_cn(light_effect_t e)
{
    if (e < 0 || e >= LIGHT_COUNT) return "?";
    return NAMES[e].cn;
}

bool light_effect_from_name(const char *name, light_effect_t *out)
{
    if (!name || !out) return false;
    for (int i = 0; i < LIGHT_COUNT; i++) {
        if (strcasecmp(name, NAMES[i].id) == 0 ||
            strcmp(name, NAMES[i].cn) == 0) {
            *out = (light_effect_t)i;
            return true;
        }
    }
    /* 常见别名, 让 AI 说"彩虹/流光/闪烁"也能对上 */
    if (strstr(name, "rainbow") || strstr(name, "彩虹") || strstr(name, "多彩")) {
        *out = LIGHT_RAINBOW; return true;
    }
    if (strstr(name, "spectrum") || strstr(name, "频谱")) {
        *out = LIGHT_MUSIC_BANDS; return true;
    }
    if (strstr(name, "music") || strstr(name, "律动") || strstr(name, "节奏") ||
        strstr(name, "音乐")) {
        *out = LIGHT_MUSIC; return true;
    }
    if (strstr(name, "breath") || strstr(name, "呼吸")) {
        *out = LIGHT_BREATHE; return true;
    }
    /* 旧版有"渐变/流水"这个灯效, 新版并进彩虹呼吸了 —— 名字继续认, 免得 AI/老配置失效 */
    if (strstr(name, "gradient") || strstr(name, "渐变") || strstr(name, "流水")) {
        *out = LIGHT_RAINBOW_BR; return true;
    }
    if (strstr(name, "chase") || strstr(name, "追光") || strstr(name, "流动")) {
        *out = LIGHT_CHASE; return true;
    }
    if (strstr(name, "fire") || strstr(name, "火焰") || strstr(name, "火")) {
        *out = LIGHT_FIRE; return true;
    }
    if (strstr(name, "star") || strstr(name, "星空")) {
        *out = LIGHT_STARRY; return true;
    }
    if (strstr(name, "police") || strstr(name, "警") || strstr(name, "爆闪") ||
        strstr(name, "闪")) {
        *out = LIGHT_POLICE; return true;
    }
    if (strstr(name, "off") || strstr(name, "关")) {
        *out = LIGHT_OFF; return true;
    }
    if (strstr(name, "mono") || strstr(name, "单色") || strstr(name, "常亮")) {
        *out = LIGHT_MONO; return true;
    }
    return false;
}

void light_set_brightness(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    s.brightness = percent;
    ESP_LOGI(TAG, "亮度 -> %d%%", percent);
}

int light_get_brightness(void) { return s.brightness; }

void light_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    s.r = r; s.g = g; s.b = b;
    s.effect = LIGHT_MONO;    /* 设了具体颜色就切单色, 否则看不到效果 */
    ESP_LOGI(TAG, "主色 -> (%u,%u,%u), 切到单色", r, g, b);
}

void light_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    s.r = r; s.g = g; s.b = b;    /* 只改主色(追光/镜像等按主色取色), 效果不动 */
}

void light_get_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (r) *r = s.r;
    if (g) *g = s.g;
    if (b) *b = s.b;
}

/* ==================== 音乐律动 ==================== */

void light_music_start(void)
{
    if (s.music_on) return;
    s.music_on     = true;
    s.music_lvl    = 0.0f;
    s.music_env    = 0.0f;
    s.saved_effect = s.effect;       /* 记住放歌前用户选的灯效 */
    s.effect       = LIGHT_MUSIC;
    s.music_peak   = 0;              /* 峰值点/频谱从零开始, 别带上一次的残影 */
    /* 滤波器状态清零要上锁: 音频任务可能正在 light_mic_pcm 里跑 lb_process */
    if (lb_mux) lb_lock();
    memset((void *)s.band, 0, sizeof(s.band));   /* volatile 数组需显式去限定 */
    memset(lb_z,   0, sizeof(lb_z));
    memset(lb_band,0, sizeof(lb_band));
    if (lb_mux) lb_unlock();
    ESP_LOGI(TAG, "灯效 -> 音乐律动 (歌停恢复 %s)", light_effect_cn(s.saved_effect));
}

void light_music_stop(void)
{
    if (!s.music_on) return;
    s.music_on = false;
    /* 只在"还在律动"时才恢复 —— 放歌途中用户手动换了灯效的话, 尊重用户的选择 */
    if (s.effect == LIGHT_MUSIC) s.effect = s.saved_effect;
    ESP_LOGI(TAG, "音乐律动结束, 恢复 %s", light_effect_cn(s.effect));
}

void light_music_level(float lv)
{
    if (lv < 0.0f)   lv = 0.0f;
    if (lv > 1.0f)   lv = 1.0f;
    s.music_lvl = lv;    /* volatile float 单写单读, 不需要加锁 */
    s.last_feed_ms = esp_timer_get_time() / 1000;
}

/* ★ 推荐入口: 直接喂解码后的 PCM。
 * ① 算总电平(峰值, 跟鼓点) ② 过 8 段滤波器组得到各频段能量(给音乐频谱灯效用)。
 * 由音乐解码循环每帧调用; 开销约 8 次乘加/样本, 44.1kHz 下可忽略。 */
void light_music_pcm(const int16_t *pcm, size_t frames)
{
    if (!pcm || frames == 0) return;
    lb_init_for(LB_FS);

    /* ① 总电平: 取峰值而不是均方根, 峰值更"跟鼓点" */
    int32_t peak = 0;
    for (size_t i = 0; i < frames; i++) {
        int32_t a = pcm[i];
        if (a < 0) a = -a;
        if (a > peak) peak = a;
    }
    float lvl = (float)peak / 32768.0f;
    if (lvl > 1.0f) lvl = 1.0f;
    s.music_lvl = lvl;
    s.last_feed_ms = esp_timer_get_time() / 1000;   /* 有数据 = 还在播 */

    /* ② 滤波器组(见 lb_process 注释) */
    lb_process(pcm, frames);
}

/* ★ 麦克风喂频谱: 没放歌时让「音乐频谱」灯效也活起来 ——
 * 对着它说话、或者手机外放音乐, 8 颗灯照样跳频谱。
 * 由麦克风采集循环调用; 只在"正在显示频谱且没放歌"时才真正干活,
 * 其它情况一个比较就返回, 对音频链路零负担。 */
void light_mic_pcm(const int16_t *pcm, size_t frames)
{
    if (!pcm || frames == 0) return;
    if (s.music_on || s.effect != LIGHT_MUSIC_BANDS) return;
    lb_init_for(16000.0f);      /* 麦克风是 16k 采样(频段整体往低挪, 观感一致) */
    lb_process(pcm, frames);
}

const char *light_state_str(void)
{
    static char buf[48];
    snprintf(buf, sizeof(buf), "%s %d%%", light_effect_cn(s.effect), s.brightness);
    return buf;
}
