/*
 * wifi_screen.c — 配网信息页实现 (5x7 ASCII 字库 + 简单排版)
 */
#include "wifi_screen.h"

#include <string.h>
#include <stdio.h>
#include "ssd1305.h"

#define SCR_W 128
#define SCR_H 64

/* ------------------------------------------------------------------
 * 5x7 点阵字库 (0x20 空格 ~ 0x5A 'Z')
 * 每个字符 5 字节 = 5 列, 每字节 bit0 是最上面一行, bit6 是最下面一行。
 * 只做大写; 渲染时会把小写统一转成大写, 省一半字库体积。
 * ------------------------------------------------------------------ */
static const uint8_t F5X7[][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, /* ' ' */
    { 0x00, 0x00, 0x5F, 0x00, 0x00 }, /* '!' */
    { 0x00, 0x07, 0x00, 0x07, 0x00 }, /* '"' */
    { 0x14, 0x7F, 0x14, 0x7F, 0x14 }, /* '#' */
    { 0x24, 0x2A, 0x7F, 0x2A, 0x12 }, /* '$' */
    { 0x23, 0x13, 0x08, 0x64, 0x62 }, /* '%' */
    { 0x36, 0x49, 0x55, 0x22, 0x50 }, /* '&' */
    { 0x00, 0x05, 0x03, 0x00, 0x00 }, /* '\'' */
    { 0x00, 0x1C, 0x22, 0x41, 0x00 }, /* '(' */
    { 0x00, 0x41, 0x22, 0x1C, 0x00 }, /* ')' */
    { 0x14, 0x08, 0x3E, 0x08, 0x14 }, /* '*' */
    { 0x08, 0x08, 0x3E, 0x08, 0x08 }, /* '+' */
    { 0x00, 0x50, 0x30, 0x00, 0x00 }, /* ',' */
    { 0x08, 0x08, 0x08, 0x08, 0x08 }, /* '-' */
    { 0x00, 0x60, 0x60, 0x00, 0x00 }, /* '.' */
    { 0x20, 0x10, 0x08, 0x04, 0x02 }, /* '/' */
    { 0x3E, 0x51, 0x49, 0x45, 0x3E }, /* '0' */
    { 0x00, 0x42, 0x7F, 0x40, 0x00 }, /* '1' */
    { 0x42, 0x61, 0x51, 0x49, 0x46 }, /* '2' */
    { 0x21, 0x41, 0x45, 0x4B, 0x31 }, /* '3' */
    { 0x18, 0x14, 0x12, 0x7F, 0x10 }, /* '4' */
    { 0x27, 0x45, 0x45, 0x45, 0x39 }, /* '5' */
    { 0x3C, 0x4A, 0x49, 0x49, 0x30 }, /* '6' */
    { 0x01, 0x71, 0x09, 0x05, 0x03 }, /* '7' */
    { 0x36, 0x49, 0x49, 0x49, 0x36 }, /* '8' */
    { 0x06, 0x49, 0x49, 0x29, 0x1E }, /* '9' */
    { 0x00, 0x36, 0x36, 0x00, 0x00 }, /* ':' */
    { 0x00, 0x56, 0x36, 0x00, 0x00 }, /* ';' */
    { 0x08, 0x14, 0x22, 0x41, 0x00 }, /* '<' */
    { 0x14, 0x14, 0x14, 0x14, 0x14 }, /* '=' */
    { 0x00, 0x41, 0x22, 0x14, 0x08 }, /* '>' */
    { 0x02, 0x01, 0x51, 0x09, 0x06 }, /* '?' */
    { 0x32, 0x49, 0x79, 0x41, 0x3E }, /* '@' */
    { 0x7E, 0x11, 0x11, 0x11, 0x7E }, /* 'A' */
    { 0x7F, 0x49, 0x49, 0x49, 0x36 }, /* 'B' */
    { 0x3E, 0x41, 0x41, 0x41, 0x22 }, /* 'C' */
    { 0x7F, 0x41, 0x41, 0x22, 0x1C }, /* 'D' */
    { 0x7F, 0x49, 0x49, 0x49, 0x41 }, /* 'E' */
    { 0x7F, 0x09, 0x09, 0x09, 0x01 }, /* 'F' */
    { 0x3E, 0x41, 0x49, 0x49, 0x7A }, /* 'G' */
    { 0x7F, 0x08, 0x08, 0x08, 0x7F }, /* 'H' */
    { 0x00, 0x41, 0x7F, 0x41, 0x00 }, /* 'I' */
    { 0x20, 0x40, 0x41, 0x3F, 0x01 }, /* 'J' */
    { 0x7F, 0x08, 0x14, 0x22, 0x41 }, /* 'K' */
    { 0x7F, 0x40, 0x40, 0x40, 0x40 }, /* 'L' */
    { 0x7F, 0x02, 0x0C, 0x02, 0x7F }, /* 'M' */
    { 0x7F, 0x04, 0x08, 0x10, 0x7F }, /* 'N' */
    { 0x3E, 0x41, 0x41, 0x41, 0x3E }, /* 'O' */
    { 0x7F, 0x09, 0x09, 0x09, 0x06 }, /* 'P' */
    { 0x3E, 0x41, 0x51, 0x21, 0x5E }, /* 'Q' */
    { 0x7F, 0x09, 0x19, 0x29, 0x46 }, /* 'R' */
    { 0x46, 0x49, 0x49, 0x49, 0x31 }, /* 'S' */
    { 0x01, 0x01, 0x7F, 0x01, 0x01 }, /* 'T' */
    { 0x3F, 0x40, 0x40, 0x40, 0x3F }, /* 'U' */
    { 0x1F, 0x20, 0x40, 0x20, 0x1F }, /* 'V' */
    { 0x3F, 0x40, 0x38, 0x40, 0x3F }, /* 'W' */
    { 0x63, 0x14, 0x08, 0x14, 0x63 }, /* 'X' */
    { 0x07, 0x08, 0x70, 0x08, 0x07 }, /* 'Y' */
    { 0x61, 0x51, 0x49, 0x45, 0x43 }, /* 'Z' */
};
#define F5X7_COUNT ((int)(sizeof(F5X7) / sizeof(F5X7[0])))

#define GLYPH_W   5
#define GLYPH_H   7
#define ADVANCE   6      /* 字宽 5 + 1 列间隔 */
#define MAX_COLS  (SCR_W / ADVANCE)   /* 21 列 */

/* 页面状态 */
static struct {
    char   banner[20];
    char   line[3][24];
    bool   invert_banner;
    bool   blink;          /* 是否画底部闪烁块 */
    bool   blink_on;
    int    blink_x;
} s_ws;

static void px(int x, int y, int on)
{
    if (x < 0 || x >= SCR_W || y < 0 || y >= SCR_H) return;
    ssd1305_set_pixel(x, y, on ? 1 : 0);
}

/* 画一个字符; invert=true 表示反色(底色白/字黑) */
static void draw_char(int x, int y, char c, bool invert)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    int idx = (int)(unsigned char)c - 0x20;
    if (idx < 0 || idx >= F5X7_COUNT) idx = 0;   /* 不认识的字符画成空格 */

    for (int col = 0; col < GLYPH_W; col++) {
        uint8_t bits = F5X7[idx][col];
        for (int row = 0; row < GLYPH_H; row++) {
            int on = (bits >> row) & 1;
            px(x + col, y + row, invert ? !on : on);
        }
    }
    if (invert) {
        /* 反色时字符右侧那一列间隔也要填白, 否则会和背景连成一片 */
        for (int row = 0; row < GLYPH_H; row++) px(x + GLYPH_W, y + row, 1);
    }
}

static void draw_text(int x, int y, const char *s, bool invert)
{
    if (!s) return;
    for (; *s; s++) {
        draw_char(x, y, *s, invert);
        x += ADVANCE;
    }
}

static int text_px(const char *s) { return s ? (int)strlen(s) * ADVANCE : 0; }

static void draw_text_center(int y, const char *s, bool invert)
{
    int w = text_px(s);
    int x = (SCR_W - w + 1) / 2;
    if (x < 0) x = 0;
    draw_text(x, y, s, invert);
}

/* 把整屏画出来 (只写显存, 不发送) */
static void render(void)
{
    ssd1305_clear();

    /* 顶部标题条: 先铺满白底, 再用反色画黑字 */
    if (s_ws.invert_banner) {
        for (int x = 0; x < SCR_W; x++)
            for (int y = 0; y < GLYPH_H + 1; y++) px(x, y, 1);
        draw_text_center(0, s_ws.banner, true);
    } else {
        draw_text_center(0, s_ws.banner, false);
    }

    for (int i = 0; i < 3; i++) {
        if (s_ws.line[i][0]) draw_text(0, 14 + i * 12, s_ws.line[i], false);
    }
}

static void flush_full(void)
{
    render();
    ssd1305_display();
}

/* 底部闪烁块所在页: y = 56..62 -> 第 7 页 */
#define BLINK_Y 56
#define BLINK_SZ 4

static void fill_ap_lines(const char *ssid, const char *pass);   /* 实现在下面 */

void wifi_screen_provision(const char *ssid, const char *pass)
{
    memset(&s_ws, 0, sizeof(s_ws));
    snprintf(s_ws.banner, sizeof(s_ws.banner), "WIFI SETUP");
    s_ws.invert_banner = true;
    s_ws.blink = true;

    /* SSID 一行最多 21 列, "AP: " 占 4 列 */
    fill_ap_lines(ssid, pass);

    flush_full();
}

void wifi_screen_connecting(void)
{
    memset(&s_ws, 0, sizeof(s_ws));
    snprintf(s_ws.banner, sizeof(s_ws.banner), "CONNECTING");
    s_ws.invert_banner = true;
    s_ws.blink = true;
    snprintf(s_ws.line[0], sizeof(s_ws.line[0]), "PLEASE WAIT...");
    flush_full();
}

/* 列出热点信息 (配网页 / 失败页共用) */
static void fill_ap_lines(const char *ssid, const char *pass)
{
    snprintf(s_ws.line[0], sizeof(s_ws.line[0]), "AP: %.*s",
             MAX_COLS - 4, ssid ? ssid : "?");
    snprintf(s_ws.line[1], sizeof(s_ws.line[1]), "PW: %.*s",
             MAX_COLS - 4, pass ? pass : "?");
    snprintf(s_ws.line[2], sizeof(s_ws.line[2]), "OPEN %.*s",
             MAX_COLS - 5, "192.168.4.1");
}

void wifi_screen_ok(const char *ip)
{
    memset(&s_ws, 0, sizeof(s_ws));
    snprintf(s_ws.banner, sizeof(s_ws.banner), "WIFI OK");
    s_ws.invert_banner = true;
    s_ws.blink = false;
    if (ip && ip[0])
        snprintf(s_ws.line[0], sizeof(s_ws.line[0]), "IP: %.*s", MAX_COLS - 4, ip);
    snprintf(s_ws.line[1], sizeof(s_ws.line[1]), "STARTING...");
    flush_full();
}

void wifi_screen_failed(const char *ap_ssid, const char *ap_pass)
{
    memset(&s_ws, 0, sizeof(s_ws));
    snprintf(s_ws.banner, sizeof(s_ws.banner), "WIFI FAILED");
    s_ws.invert_banner = false;   /* 不反色 = 视觉上更"警示" */
    s_ws.blink = true;
    fill_ap_lines(ap_ssid, ap_pass);
    flush_full();
}

void wifi_screen_tick(uint32_t now_ms)
{
    if (!s_ws.blink) return;

    /* 每 500ms 翻转一次; 只改第 7 页并只发这一页, 几乎不占 I2C */
    bool on = ((now_ms / 500) & 1) != 0;
    if (on == s_ws.blink_on) return;
    s_ws.blink_on = on;

    int x0 = (SCR_W - BLINK_SZ) / 2;
    for (int x = x0; x < x0 + BLINK_SZ; x++)
        for (int y = BLINK_Y; y < BLINK_Y + BLINK_SZ; y++)
            px(x, y, on ? 1 : 0);

    ssd1305_reassert();
    ssd1305_display_pages((uint8_t)(BLINK_Y / 8), (uint8_t)(BLINK_Y / 8));
}
