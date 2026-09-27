/**
 * @file gfx.c
 * @brief 基础绘图库实现
 */
#include "gfx.h"
#include "ssd1305.h"
#include <math.h>
#include <stdlib.h>

/* ---------- 像素 ---------- */

void gfx_set_pixel(int x, int y, uint8_t color)
{
    ssd1305_set_pixel(x, y, color);
}

/* ---------- 线 (Bresenham) ---------- */

void gfx_draw_line(int x0, int y0, int x1, int y1, uint8_t color)
{
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        gfx_set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ---------- 矩形 ---------- */

void gfx_draw_rect(int x, int y, int w, int h, uint8_t color)
{
    gfx_draw_line(x, y, x + w - 1, y, color);
    gfx_draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    gfx_draw_line(x, y, x, y + h - 1, color);
    gfx_draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void gfx_fill_rect(int x, int y, int w, int h, uint8_t color)
{
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            gfx_set_pixel(x + dx, y + dy, color);
        }
    }
}

/* ---------- 圆角矩形 ---------- */

/**
 * 计算圆角矩形某一行的左右缩进量
 * dy: 相对于顶部的行号 (0..h-1)
 * 返回该行需要从左右各缩进的像素数
 */
static int round_rect_indent(int dy, int h, int r)
{
    if (r <= 0) return 0;
    if (dy < r) {
        /* 上圆角：从顶部往下 r 行 */
        int t = r - dy; /* 距角顶的距离, r..1 */
        return r - (int)sqrtf((float)(r * r - t * t));
    } else if (dy >= h - r) {
        /* 下圆角：从底部往上 r 行 */
        int t = dy - (h - r) + 1; /* 1..r */
        return r - (int)sqrtf((float)(r * r - t * t));
    }
    return 0;
}

void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint8_t color)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    for (int dy = 0; dy < h; dy++) {
        int indent = round_rect_indent(dy, h, r);
        int x_start = x + indent;
        int x_end = x + w - 1 - indent;
        for (int dx = x_start; dx <= x_end; dx++) {
            gfx_set_pixel(dx, y + dy, color);
        }
    }
}

void gfx_draw_round_rect(int x, int y, int w, int h, int r, uint8_t color)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* 四条直边 */
    gfx_draw_line(x + r, y, x + w - r - 1, y, color);               /* 上 */
    gfx_draw_line(x + r, y + h - 1, x + w - r - 1, y + h - 1, color); /* 下 */
    gfx_draw_line(x, y + r, x, y + h - r - 1, color);               /* 左 */
    gfx_draw_line(x + w - 1, y + r, x + w - 1, y + h - r - 1, color); /* 右 */

    /* 四个角的弧（用圆的八对称性画四分之一圆） */
    /* 左上角圆心 (x+r, y+r) */
    /* 右上角圆心 (x+w-1-r, y+r) */
    /* 左下角圆心 (x+r, y+h-1-r) */
    /* 右下角圆心 (x+w-1-r, y+h-1-r) */

    int cx[4] = {x + r, x + w - 1 - r, x + r, x + w - 1 - r};
    int cy[4] = {y + r, y + r, y + h - 1 - r, y + h - 1 - r};
    /* 每个角画对应的象限：左上=第二象限, 右上=第一, 左下=第三, 右下=第四 */
    /* 用中点圆算法，只画对应象限的点 */

    for (int q = 0; q < 4; q++) {
        int ox = cx[q], oy = cy[q];
        int fx = 1 - 2 * r;
        int fy = 0;
        int dx = 0;
        int dy = -r;
        int px = 0, py = r;

        while (px <= py) {
            /* 根据象限选择偏移符号 */
            int sx, sy;
            switch (q) {
                case 0: sx = -px; sy = -py; break; /* 左上 */
                case 1: sx =  px; sy = -py; break; /* 右上 */
                case 2: sx = -px; sy =  py; break; /* 左下 */
                default:sx =  px; sy =  py; break; /* 右下 */
            }
            gfx_set_pixel(ox + sx, oy + sy, color);

            /* 同时画交换后的点（同一象限内的另一个八分之一） */
            switch (q) {
                case 0: sx = -py; sy = -px; break;
                case 1: sx =  py; sy = -px; break;
                case 2: sx = -py; sy =  px; break;
                default:sx =  py; sy =  px; break;
            }
            gfx_set_pixel(ox + sx, oy + sy, color);

            px++;
            if (fx < 0) {
                fx += 2 * px + 1;
            } else {
                py--;
                fx += 2 * (px - py) + 1;
            }
            (void)fy; (void)dx; (void)dy;
        }
    }
}

/* ---------- 圆 ---------- */

void gfx_draw_circle(int cx, int cy, int r, uint8_t color)
{
    int x = 0, y = r;
    int d = 1 - r;
    while (x <= y) {
        gfx_set_pixel(cx + x, cy + y, color);
        gfx_set_pixel(cx - x, cy + y, color);
        gfx_set_pixel(cx + x, cy - y, color);
        gfx_set_pixel(cx - x, cy - y, color);
        gfx_set_pixel(cx + y, cy + x, color);
        gfx_set_pixel(cx - y, cy + x, color);
        gfx_set_pixel(cx + y, cy - x, color);
        gfx_set_pixel(cx - y, cy - x, color);
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

void gfx_fill_circle(int cx, int cy, int r, uint8_t color)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx_max = (int)sqrtf((float)(r * r - dy * dy));
        for (int dx = -dx_max; dx <= dx_max; dx++) {
            gfx_set_pixel(cx + dx, cy + dy, color);
        }
    }
}

/* ---------- 椭圆 ---------- */

void gfx_draw_ellipse(int cx, int cy, int rx, int ry, uint8_t color)
{
    /* 中点椭圆算法 */
    long rx2 = (long)rx * rx;
    long ry2 = (long)ry * ry;
    long x = 0, y = ry;
    long px = 0, py = 2 * rx2 * y;

    /* 区域1 */
    long p = ry2 - (rx2 * ry) + (rx2 / 4);
    while (px < py) {
        gfx_set_pixel(cx + x, cy + y, color);
        gfx_set_pixel(cx - x, cy + y, color);
        gfx_set_pixel(cx + x, cy - y, color);
        gfx_set_pixel(cx - x, cy - y, color);
        x++;
        px += 2 * ry2;
        if (p < 0) {
            p += ry2 + px;
        } else {
            y--;
            py -= 2 * rx2;
            p += ry2 + px - py;
        }
    }
    /* 区域2 */
    p = ry2 * (x + 1) * (x + 1) + rx2 * (y - 1) * (y - 1) - rx2 * ry2;
    while (y >= 0) {
        gfx_set_pixel(cx + x, cy + y, color);
        gfx_set_pixel(cx - x, cy + y, color);
        gfx_set_pixel(cx + x, cy - y, color);
        gfx_set_pixel(cx - x, cy - y, color);
        y--;
        py -= 2 * rx2;
        if (p > 0) {
            p += rx2 - py;
        } else {
            x++;
            px += 2 * ry2;
            p += rx2 - py + px;
        }
    }
}

void gfx_fill_ellipse(int cx, int cy, int rx, int ry, uint8_t color)
{
    for (int dy = -ry; dy <= ry; dy++) {
        float t = 1.0f - (float)(dy * dy) / (float)(ry * ry);
        if (t < 0) t = 0;
        int dx_max = (int)(rx * sqrtf(t));
        for (int dx = -dx_max; dx <= dx_max; dx++) {
            gfx_set_pixel(cx + dx, cy + dy, color);
        }
    }
}

/* ---------- 弧 ---------- */

void gfx_draw_arc(int cx, int cy, int r, int start_deg, int end_deg, uint8_t color)
{
    /* 规范化角度到 0..360 */
    while (start_deg < 0) start_deg += 360;
    while (end_deg < 0) end_deg += 360;
    while (start_deg >= 360) start_deg -= 360;
    while (end_deg >= 360) end_deg -= 360;

    int steps = (end_deg - start_deg + 360) % 360;
    if (steps == 0) steps = 360;

    for (int i = 0; i <= steps; i++) {
        int deg = (start_deg + i) % 360;
        float rad = (float)deg * 3.14159265f / 180.0f;
        int x = cx + (int)(r * cosf(rad) + 0.5f);
        int y = cy + (int)(r * sinf(rad) + 0.5f);
        gfx_set_pixel(x, y, color);
    }
}
