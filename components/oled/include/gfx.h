/**
 * @file gfx.h
 * @brief 基础绘图库（基于 SSD1305 显存）
 *
 * 提供点、线、矩形、圆角矩形、圆、椭圆、弧等基础图元。
 * 所有操作直接写入显存，需调用 ssd1305_display() 刷新。
 */
#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 写像素（转发到 ssd1305） */
void gfx_set_pixel(int x, int y, uint8_t color);

/** @brief Bresenham 画线 */
void gfx_draw_line(int x0, int y0, int x1, int y1, uint8_t color);

/** @brief 画空心矩形 */
void gfx_draw_rect(int x, int y, int w, int h, uint8_t color);

/** @brief 画填充矩形 */
void gfx_fill_rect(int x, int y, int w, int h, uint8_t color);

/**
 * @brief 画空心圆角矩形
 * @param x 左上角 x
 * @param y 左上角 y
 * @param w 宽度
 * @param h 高度
 * @param r 圆角半径
 */
void gfx_draw_round_rect(int x, int y, int w, int h, int r, uint8_t color);

/**
 * @brief 画填充圆角矩形（眼睛主体用这个）
 */
void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint8_t color);

/** @brief 画空心圆（中点圆算法） */
void gfx_draw_circle(int cx, int cy, int r, uint8_t color);

/** @brief 画填充圆 */
void gfx_fill_circle(int cx, int cy, int r, uint8_t color);

/** @brief 画空心椭圆 */
void gfx_draw_ellipse(int cx, int cy, int rx, int ry, uint8_t color);

/** @brief 画填充椭圆 */
void gfx_fill_ellipse(int cx, int cy, int rx, int ry, uint8_t color);

/**
 * @brief 画圆弧（从 start_deg 到 end_deg，顺时针）
 * @param cx 圆心 x
 * @param cy 圆心 y
 * @param r  半径
 * @param start_deg 起始角度（0=右, 90=下, 180=左, 270=上）
 * @param end_deg   结束角度
 */
void gfx_draw_arc(int cx, int cy, int r, int start_deg, int end_deg, uint8_t color);

#ifdef __cplusplus
}
#endif

#endif /* GFX_H */
