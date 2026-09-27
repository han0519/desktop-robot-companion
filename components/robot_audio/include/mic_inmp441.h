/*
 * mic_inmp441.h — INMP441 I2S 麦克风 (输入)
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* 初始化 I2S 麦克风 */
void mic_init(void);

/* 读取音频数据 (返回实际读取的采样数) */
size_t mic_read(int16_t *buf, size_t sample_count, int timeout_ms);

/* 启动麦克风 */
void mic_start(void);

/* 停止麦克风 */
void mic_stop(void);
