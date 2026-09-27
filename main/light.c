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
#include <math.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "light";

#define LIGHT_TASK_STACK   3072
#define LIGHT_FRAME_MS     25          /* 40Hz */
#define PI_F               3.14159265f
#define LIGHT_MAX_LED      WS2812_LED_RING_NUM

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
    { "off",      "关灯"     },
    { "mono",     "单色"     },
    { "breathe",  "呼吸"     },
    { "gradient", "渐变"     },
    { "rainbow",  "多色呼吸" },
    { "police",   "警车爆闪" },
    { "music",    "律动"     },
};

static struct {
    light_effect_t effect;
    int      brightness;     /* 0~100 */
    uint8_t  r, g, b;        /* 主色 */
    /* ---- 音乐律动 ---- */
    volatile float music_lvl;      /* 音乐模块喂进来的最新电平 0..1 */
    float          music_env;      /* 平滑包络(快攻慢放), 让鼓点"弹"起来 */
    light_effect_t saved_effect;   /* 进律动前的灯效(歌停恢复用) */
    bool           music_on;
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

/* 按当前灯效算出这一帧 */
static void compute_frame(float t, int n)
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

    case LIGHT_GRADIENT: {
        /* 渐变: 沿灯环从主色色相过渡 120°, 整体缓慢流动 */
        float base_h = 0.0f;
        {   /* 从主色反推一个近似色相, 让"渐变"带着主色的味道 */
            float mx = fmaxf(fmaxf(s.r, s.g), s.b);
            float mn = fminf(fminf(s.r, s.g), s.b);
            if (mx > mn) {
                if      (mx == s.r) base_h = 60.0f * fmodf((s.g - s.b) / (mx - mn) + 6.0f, 6.0f);
                else if (mx == s.g) base_h = 60.0f * ((s.b - s.r) / (mx - mn) + 2.0f);
                else                base_h = 60.0f * ((s.r - s.g) / (mx - mn) + 4.0f);
            }
        }
        float drift = t * 22.0f;                      /* 度/秒 */
        for (int i = 0; i < n; i++) {
            uint8_t r, g, b;
            hsv_to_rgb(base_h + drift + 120.0f * (float)i / (float)n, 100.0f, br, &r, &g, &b);
            s_frame[i].r = r; s_frame[i].g = g; s_frame[i].b = b;
        }
        break;
    }

    case LIGHT_RAINBOW: {
        /* 多色呼吸: 整环彩虹(每灯一个色相) + 整体亮度呼吸 */
        float k = 0.20f + 0.80f * (0.5f + 0.5f * sinf(t * 2.0f * PI_F / 3.2f));
        float drift = t * 45.0f;
        for (int i = 0; i < n; i++) {
            uint8_t r, g, b;
            hsv_to_rgb(drift + 360.0f * (float)i / (float)n, 100.0f, br * k, &r, &g, &b);
            s_frame[i].r = r; s_frame[i].g = g; s_frame[i].b = b;
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
        /* ★★ 环形律动 ★★
         * 音乐电平 → 点亮的灯数 + 整体亮度; 一个亮点沿灯环旋转(彗星拖尾),
         * 色相随时间慢慢转。包络【快攻慢放】(attack 0.55 / release 0.10):
         * 鼓点一来立刻弹起, 声音一停慢慢落下 —— 看起来是跟着节奏跳,
         * 而不是频闪。电平由音乐解码循环每帧喂 (light_music_level)。 */
        float lv = s.music_lvl;
        if (lv > s.music_env) s.music_env += (lv - s.music_env) * 0.55f;   /* 快攻 */
        else                  s.music_env += (lv - s.music_env) * 0.10f;   /* 慢放 */
        if (s.music_env < 0.015f) s.music_env = 0.015f;                    /* 别全黑 */

        float env = s.music_env;
        int   lit = 1 + (int)(env * (n - 1) + 0.5f);   /* 电平越高亮得越多 */
        if (lit > n) lit = n;
        float base = 0.22f + 0.78f * env;              /* 声音越大整体越亮 */
        int   head = ((int)(t * 240.0f) % n + n) % n;  /* 亮点沿环旋转 */

        for (int i = 0; i < n; i++) {
            int d = (head - i + n) % n;                /* 0=头部, 越大越靠尾 */
            float k = 0.0f;
            if (d < lit) k = 1.0f - 0.72f * (float)d / (float)lit;
            uint8_t r, g, b;
            hsv_to_rgb(t * 40.0f + 360.0f * (float)i / (float)n, 100.0f,
                       br * base * k, &r, &g, &b);
            s_frame[i].r = r; s_frame[i].g = g; s_frame[i].b = b;
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
        float t = (float)(esp_timer_get_time() - t0) / 1000000.0f;
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

    xTaskCreatePinnedToCore(light_task, "light", LIGHT_TASK_STACK, NULL, 2, NULL, 0);
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
    if (strstr(name, "music") || strstr(name, "律动") || strstr(name, "节奏") ||
        strstr(name, "音乐")) {
        *out = LIGHT_MUSIC; return true;
    }
    if (strstr(name, "breath") || strstr(name, "呼吸")) {
        *out = LIGHT_BREATHE; return true;
    }
    if (strstr(name, "gradient") || strstr(name, "渐变") || strstr(name, "流水")) {
        *out = LIGHT_GRADIENT; return true;
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
}

const char *light_state_str(void)
{
    static char buf[48];
    snprintf(buf, sizeof(buf), "%s %d%%", light_effect_cn(s.effect), s.brightness);
    return buf;
}
