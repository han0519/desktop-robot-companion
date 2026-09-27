/**
 * @file ssd1305.h
 * @brief SSD1305 128x64 I2C OLED 驱动
 *
 * 基于 ESP-IDF v5.x I2C Master 新 API。
 * 显存 1024 字节，按页(8行/页)存储，所有绘图先写入显存再统一刷新。
 */
#ifndef SSD1305_H
#define SSD1305_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SSD1305_WIDTH    128
#define SSD1305_HEIGHT   64
#define SSD1305_PAGES    (SSD1305_HEIGHT / 8)
#define SSD1305_FB_SIZE  (SSD1305_WIDTH * SSD1305_PAGES)

#define SSD1305_I2C_ADDR 0x3C   /**< 7位 I2C 地址 (SA0=0) */

/**
 * @brief 初始化 SSD1305（I2C 总线 + 设备 + 显示序列）
 *
 * @param port      I2C 端口号 (I2C_NUM_0 / I2C_NUM_1)
 * @param sda_pin   SDA GPIO
 * @param scl_pin   SCL GPIO
 * @param clk_speed I2C 时钟频率 (Hz)，建议 400000
 * @return esp_err_t ESP_OK 成功
 */
esp_err_t ssd1305_init(i2c_port_t port, int sda_pin, int scl_pin, uint32_t clk_speed);

/** @brief 清显存（全部熄灭） */
void ssd1305_clear(void);

/** @brief 填充显存（全部点亮） */
void ssd1305_fill(void);

/**
 * @brief 写单个像素
 * @param x     0..127
 * @param y     0..63
 * @param color 1=点亮 0=熄灭
 */
void ssd1305_set_pixel(int x, int y, uint8_t color);

/** @brief 读单个像素 */
uint8_t ssd1305_get_pixel(int x, int y);

/**
 * @brief 获取显存指针（供 GFX 层直接操作）
 * @return uint8_t* 1024 字节显存
 */
uint8_t *ssd1305_get_framebuffer(void);

/** @brief 将显存内容刷新到屏幕（全部 8 页） */
esp_err_t ssd1305_display(void);

/**
 * @brief 只刷新指定页区间（脏页局部刷新，大幅减少 I2C 流量）
 * @param page_start 起始页 0..7（每页 8 行）
 * @param page_end   结束页 0..7（含）
 * @return ESP_OK 全部页发送成功；否则返回最后一个错误，调用方应重新整屏刷新
 */
esp_err_t ssd1305_display_pages(uint8_t page_start, uint8_t page_end);

/**
 * @brief 设置屏幕方向（面板装反时用，可在运行时随时改）
 * @param flip_x true = 左右翻转（段重映射 0xA0）
 * @param flip_y true = 上下翻转（COM 扫描方向 0xC0）
 */
void ssd1305_set_flip(bool flip_x, bool flip_y);

/** @brief 读取当前方向设置 */
void ssd1305_get_flip(bool *flip_x, bool *flip_y);

/**
 * @brief 重申显示配置（方向/寻址/电荷泵/开显示）
 *
 * SSD1305 的内部寄存器是易失的：I2C 总线上的毛刺、或舵机堵转造成的
 * 电源跌落，都可能把它打回上电默认值 —— 表现就是画面「过一会儿自己倒过来」。
 * 只发命令、不含 0xAE(关显示)，所以不会闪屏。建议每几秒调一次。
 */
void ssd1305_reassert(void);

/** @brief 屏幕整体开关 */
void ssd1305_set_display_on(bool on);

/** @brief 设置对比度 0..255 */
void ssd1305_set_contrast(uint8_t contrast);

#ifdef __cplusplus
}
#endif

#endif /* SSD1305_H */
