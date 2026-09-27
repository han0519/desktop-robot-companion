/*
 * speaker_max98357a.h — MAX98357A I2S 功放 (输出)
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* 初始化 I2S 功放 */
void speaker_init(void);

/* 使能 I2S 发送通道。
 * 注意: 必须调用, 否则 speaker_write() 写不进任何数据(一点声音都没有)。
 * speaker_write()/speaker_beep() 内部也会兜底调用, 所以重复调用无害。 */
void speaker_start(void);

/* 播放音频数据 (16bit PCM) */
size_t speaker_write(const int16_t *buf, size_t sample_count, int timeout_ms);

/* 静音/取消静音 */
void speaker_set_mute(bool mute);

/* 播放提示音 (简单的蜂鸣音, freq_hz, duration_ms) */
void speaker_beep(uint32_t freq_hz, uint32_t duration_ms);

/* 提示音音量 0~100 (百分比, 默认 50) */
void speaker_set_volume(int percent);
int  speaker_get_volume(void);

/* 动态切换播放采样率。
 * 语音(Opus)是 24kHz, 在线音乐通常是 44.1kHz —— MAX98357A 的采样率由
 * BCLK/LRCK 直接决定, 所以改一下 I2S 时钟就能原速播放音乐, 不用软件重采样。
 * 传 0 或负值无效; 返回 ESP_OK 表示切换成功。 */
int  speaker_set_sample_rate(int hz);
int  speaker_get_sample_rate(void);

/* 把功放"钉住", 期间不做空闲自动关断。
 * 播在线音乐必须调用(播放中 speaker_write 会阻塞几百毫秒, 空闲任务会误判成
 * 空闲而中途关掉功放, 声音断一块)。语音(每帧 60ms 一次写)不需要。 */
void speaker_set_hold(bool on);
bool speaker_get_hold(void);
