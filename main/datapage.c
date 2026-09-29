/*
 * datapage.c — 数据页实现(时间 + 环境温湿度)
 *
 * 布局样式与 docs/data_preview.html 一一对应(A~E), 字模用
 * datapage_glyphs.h(SimSun 16x16 内嵌点阵采样, 与预览页完全同一套)。
 *
 * 绘制策略: 数据页激活时由主循环调用 datapage_update(), 【秒变化才重画】,
 * 平时不碰 I2C —— OLED 刷 1KB 要 ~25ms, 每秒一次完全无感。
 */
#include "datapage.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "ssd1305.h"

#include "datapage_glyphs.h"

/* 字模索引: 0温 1湿 2周 3日 4一 5二 6三 7四 8五 9六 (见 datapage_glyphs.h) */
static const uint8_t *const CN_GLYPHS[10] = {
    GLYPH_0, GLYPH_1, GLYPH_2, GLYPH_3, GLYPH_4,
    GLYPH_5, GLYPH_6, GLYPH_7, GLYPH_8, GLYPH_9,
};

static const char *TAG = "dpage";

/* ================= 5x7 ASCII 点阵(数字与符号) =================
 * 每字符 7 字节, 每字节 5 位(bit7=最左), 与预览页同源。 */
static const uint8_t FONT5x7[16][7] = {
    { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E },   /* 0 */
    { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E },   /* 1 */
    { 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F },   /* 2 */
    { 0x1F,0x02,0x04,0x02,0x01,0x11,0x0E },   /* 3 */
    { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 },   /* 4 */
    { 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E },   /* 5 */
    { 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E },   /* 6 */
    { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 },   /* 7 */
    { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E },   /* 8 */
    { 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C },   /* 9 */
    { 0x00,0x04,0x00,0x00,0x00,0x04,0x00 },   /* : */
    { 0x00,0x00,0x00,0x1F,0x00,0x00,0x00 },   /* - */
    { 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C },   /* . */
    { 0x0E,0x11,0x10,0x10,0x10,0x11,0x0E },   /* C */
    { 0x19,0x1A,0x02,0x04,0x08,0x0B,0x13 },   /* % */
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00 },   /* 空 */
};
static int font_idx(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    switch (c) {
        case ':': return 10;
        case '-': return 11;
        case '.': return 12;
        case 'C': return 13;
        case '%': return 14;
        default:  return 15;
    }
}

/* 汉字字模下标: 0温 1湿 2周 3日 4一 5二 6三 7四 8五 9六 */
#define CN_WEN   0
#define CN_SHI   1
#define CN_RI    3
/* 星期字模: tm_wday 0=日 1=一 ... 6=六; 越界一律返回「日」, 绝不给 draw_cn 传脏值 */
static int cn_weekday(int wday)
{
    if (wday < 0 || wday > 6) return CN_RI;
    return (wday == 0) ? CN_RI : 3 + wday;
}
#define CN_ZHOU 2

/* ================= 绘制原语 ================= */
static void put_px(int x, int y)
{
    if (x >= 0 && x < SSD1305_WIDTH && y >= 0 && y < SSD1305_HEIGHT)
        ssd1305_set_pixel(x, y, 1);
}

static void draw_text(const char *s, int x, int y, int scale)
{
    for (; *s; s++) {
        const uint8_t *g = FONT5x7[font_idx(*s)];
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] & (0x10 >> c))
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++)
                            put_px(x + c*scale + dx, y + r*scale + dy);
        x += 6 * scale;
    }
}

static int text_w(const char *s, int scale) { return (int)strlen(s) * 6 * scale; }

static void draw_cn(int idx, int x, int y)
{
    if (idx < 0 || idx >= 10) return;
    const uint8_t *g = CN_GLYPHS[idx];
    for (int r = 0; r < 16; r++)
        for (int c8 = 0; c8 < 2; c8++)
            for (int bit = 0; bit < 8; bit++)
                if (g[r*2 + c8] & (0x80 >> bit))
                    put_px(x + c8*8 + bit, y + r);
}

static void draw_rect(int x, int y, int w, int h)
{
    for (int i = 0; i < w; i++) { put_px(x+i, y); put_px(x+i, y+h-1); }
    for (int j = 0; j < h; j++) { put_px(x, y+j); put_px(x+w-1, y+j); }
}

static void draw_bar(int x, int y, int w, int h, float frac)
{
    draw_rect(x, y, w, h);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int fw = (int)((w - 2) * frac);
    for (int j = 1; j < h - 1; j++)
        for (int i = 0; i < fw; i++) put_px(x+1+i, y+j);
}

/* ================= 状态 ================= */
static struct {
    bool       active;
    uint8_t    style;
    float      env_t, env_h;
    bool       env_valid;
    uint32_t   last_sec;
    bool       force;
} s = { .style = 0 };

static bool get_tm(struct tm *tm)
{
    /* ★ 不管成功失败都要把 tm 填成确定的值:
       下面几个样式在读不到时间时会直接读 tm.tm_wday 画"星期",
       如果这个结构体是未初始化的栈内存, 画出来的就是随机汉字(实机复现过)。 */
    memset(tm, 0, sizeof(*tm));
    time_t now = time(NULL);
    if (now < 1700000000) return false;    /* 还没对上时 */
    if (localtime_r(&now, tm) == NULL) {   /* localtime 也可能失败 */
        memset(tm, 0, sizeof(*tm));
        return false;
    }
    return true;
}

bool datapage_time_synced(void) { return time(NULL) >= 1700000000; }

/* ================= 样式 A: 紧凑三行 ================= */
static void render_a(void)
{
    struct tm tm;
    bool synced = get_tm(&tm);
    char buf[32];

    if (synced) {
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        draw_text(buf, 2, 5, 1);
        draw_cn(cn_weekday(tm.tm_wday), 128 - 34, 0);   /* 只有对上时才画星期 */
    } else {
        draw_text("--:--:--", 2, 5, 1);
        draw_cn(CN_ZHOU, 128 - 34, 0);                  /* 「周」+ 短横, 表示待对时 */
        draw_text("-", 128 - 18, 5, 1);
    }

    for (int x = 2; x < 126; x++) put_px(x, 17);

    if (synced) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        draw_text(buf, (128 - text_w(buf, 2)) / 2, 21, 2);
    } else {
        draw_text("--:--:--", (128 - text_w("--:--:--", 2)) / 2, 21, 2);
    }

    draw_cn(CN_WEN, 2, 42);
    if (s.env_valid) { snprintf(buf, sizeof(buf), "%.1f", s.env_t); draw_text(buf, 21, 43, 2); }
    else               draw_text("--", 21, 43, 2);
    draw_cn(CN_SHI, 72, 42);
    if (s.env_valid) { snprintf(buf, sizeof(buf), "%d%%", (int)s.env_h); draw_text(buf, 91, 43, 2); }
    else               draw_text("--", 91, 43, 2);
}

/* ================= 样式 B: 大时钟 ================= */
static void render_b(void)
{
    struct tm tm;
    bool synced = get_tm(&tm);
    char buf[32];

    if (synced) {
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        draw_text(buf, 2, 2, 1);
        draw_cn(cn_weekday(tm.tm_wday), 128 - 34, 0);
    } else {
        draw_cn(CN_ZHOU, 128 - 34, 0);
        draw_text("-", 128 - 18, 2, 1);
    }
    for (int x = 2; x < 126; x++) put_px(x, 15);

    if (synced) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
        draw_text(buf, (128 - text_w(buf, 3)) / 2, 19, 3);
        snprintf(buf, sizeof(buf), "%02d", tm.tm_sec);
        draw_text(buf, 128 - text_w(buf,1) - 2, 36, 1);
    } else {
        draw_text("--:--", (128 - text_w("--:--", 3)) / 2, 19, 3);
    }

    draw_cn(CN_WEN, 2, 48);
    if (s.env_valid) { snprintf(buf, sizeof(buf), "%.1f", s.env_t); draw_text(buf, 21, 49, 2); }
    else               draw_text("--", 21, 49, 2);
    draw_cn(CN_SHI, 72, 48);
    if (s.env_valid) { snprintf(buf, sizeof(buf), "%d%%", (int)s.env_h); draw_text(buf, 91, 49, 2); }
    else               draw_text("--", 91, 49, 2);
}

/* ================= 样式 C: 仪表盘三格 ================= */
static void render_c(void)
{
    struct tm tm;
    bool synced = get_tm(&tm);
    char buf[32];

    draw_rect(0, 0, 128, 31);
    if (synced) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
        draw_text(buf, 3, 4, 2);
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        draw_text(buf, 3, 22, 1);
        draw_cn(cn_weekday(tm.tm_wday), 94, 7);
    } else {
        draw_text("--:--", 3, 4, 2);
        draw_cn(CN_ZHOU, 94, 7);
        draw_text("-", 110, 11, 1);
    }

    draw_rect(0, 33, 63, 31);
    draw_cn(CN_WEN, 3, 35);
    if (s.env_valid) { snprintf(buf, sizeof(buf), "%.1f", s.env_t); draw_text(buf, 3, 49, 2); }
    else               draw_text("--", 3, 49, 2);
    draw_rect(65, 33, 63, 31);
    draw_cn(CN_SHI, 68, 35);
    if (s.env_valid) { snprintf(buf, sizeof(buf), "%d%%", (int)s.env_h); draw_text(buf, 68, 49, 2); }
    else               draw_text("--", 68, 49, 2);
}

/* ================= 样式 D: 环境监控(带进度条) ================= */
static void render_d(void)
{
    struct tm tm;
    bool synced = get_tm(&tm);
    char buf[32];

    if (synced) {
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        draw_text(buf, 2, 1, 1);
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        draw_text(buf, 128 - text_w(buf,1) - 2, 1, 1);
    }

    draw_cn(CN_WEN, 2, 12);
    if (s.env_valid) { snprintf(buf, sizeof(buf), "%.1fC", s.env_t); draw_text(buf, 21, 13, 2); }
    else               draw_text("--", 21, 13, 2);
    draw_bar(2, 31, 124, 7, s.env_valid ? s.env_t / 45.0f : 0);

    draw_cn(CN_SHI, 2, 41);
    if (s.env_valid) { snprintf(buf, sizeof(buf), "%d%%", (int)s.env_h); draw_text(buf, 21, 42, 2); }
    else               draw_text("--", 21, 42, 2);
    draw_bar(2, 58, 124, 6, s.env_valid ? s.env_h / 100.0f : 0);
}

/* ================= 样式 E: 极简居中 ================= */
static void render_e(void)
{
    struct tm tm;
    bool synced = get_tm(&tm);
    char buf[32];

    if (synced) {
        snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
        int tx = (128 - text_w(buf, 3)) / 2;
        draw_text(buf, tx, 2, 3);
        snprintf(buf, sizeof(buf), "%02d", tm.tm_sec);
        draw_text(buf, tx + text_w("--:--", 3) + 3, 16, 1);
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        draw_text(buf, (128 - text_w(buf,1)) / 2, 28, 1);
    } else {
        draw_text("--:--", (128 - text_w("--:--", 3)) / 2, 2, 3);
    }

    draw_cn(CN_WEN, 21, 46);
    if (s.env_valid) { snprintf(buf, sizeof(buf), "%.1f", s.env_t); draw_text(buf, 39, 47, 1); }
    draw_cn(CN_SHI, 71, 46);
    if (s.env_valid) { snprintf(buf, sizeof(buf), "%d%%", (int)s.env_h); draw_text(buf, 89, 47, 1); }
    if ((esp_timer_get_time() / 500000ULL) % 2 == 0) { put_px(63, 62); put_px(64, 62); }
}

static const char *STYLE_NAMES[DP_STYLE_COUNT] = {
    "A 紧凑三行", "B 大时钟", "C 仪表盘", "D 环境监控", "E 极简居中",
};

const char *datapage_style_name(int style)
{
    if (style < 0 || style >= DP_STYLE_COUNT) return "?";
    return STYLE_NAMES[style];
}

/* ================= 对外接口 ================= */
bool datapage_active(void) { return s.active; }

void datapage_toggle(void)
{
    s.active = !s.active;
    s.force  = true;
    ESP_LOGI(TAG, "数据页 %s (样式 %s)", s.active ? "开" : "关(回表情页)",
             datapage_style_name(s.style));
}

void datapage_force_redraw(void) { s.force = true; }

void datapage_set_style(int style)
{
    if (style < 0 || style >= DP_STYLE_COUNT) return;
    s.style = (uint8_t)style;
    s.force = true;
    nvs_handle_t h;
    if (nvs_open("datapage", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "style", s.style);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "数据页样式 -> %s", datapage_style_name(s.style));
}

int datapage_get_style(void) { return s.style; }

void datapage_set_env(float temp_c, float hum_pct, bool valid)
{
    s.env_valid = valid;
    s.env_t = temp_c;
    s.env_h = hum_pct;
}

void datapage_update(uint32_t dt_ms)
{
    (void)dt_ms;
    if (!s.active) return;
    if (ssd1305_absent()) return;             /* 屏幕没接好就不画 */

    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    if (!s.force && now_s == s.last_sec) return;   /* 秒没变不重画 */
    s.last_sec = now_s;
    s.force = false;

    ssd1305_clear();
    switch (s.style % DP_STYLE_COUNT) {
        case 0: render_a(); break;
        case 1: render_b(); break;
        case 2: render_c(); break;
        case 3: render_d(); break;
        case 4: render_e(); break;
    }
    ssd1305_display();
}

static void sntp_start(void)
{
    setenv("TZ", "CST-8", 1);   /* 中国标准时间 UTC+8 */
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP 对时已启动(ntp.aliyun.com / pool.ntp.org)");
}

void datapage_init(void)
{
    /* 读取上次样式(NVS, 掉电不丢) */
    nvs_handle_t h;
    if (nvs_open("datapage", NVS_READONLY, &h) == ESP_OK) {
        uint8_t st = 0;
        if (nvs_get_u8(h, "style", &st) == ESP_OK && st < DP_STYLE_COUNT)
            s.style = st;
        nvs_close(h);
    }
    sntp_start();
    ESP_LOGI(TAG, "数据页就绪: 默认样式 %s (BOOT 短按切换)", datapage_style_name(s.style));
}
