/*
 * wake_word.h — esp-sr WakeNet 唤醒词（「你好小智」）
 *
 * 模型来自 flash 的 model 分区，由 esp-sr 的构建脚本 movemodel.py 自动打包烧入。
 * 输入必须是 16kHz / 单声道 / int16 PCM，按 wake_word_chunk() 个样本为一帧送入。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化唤醒词引擎。成功返回 true（模型缺失/内存不足返回 false，此时应回退到能量 VAD） */
bool wake_word_init(void);

/** 引擎是否可用 */
bool wake_word_ready(void);

/** 引擎要求的每帧样本数（16bit samples），未就绪返回 0 */
int wake_word_chunk(void);

/** 模型名（用于日志/网页显示），未就绪返回 "-" */
const char *wake_word_name(void);

/**
 * @brief 送入一帧 PCM 并检测
 * @param pcm     16kHz 单声道 int16 样本
 * @param samples 样本数（应 >= wake_word_chunk()，内部按整帧切分）
 * @return 命中时返回唤醒词名字，否则 NULL
 */
const char *wake_word_feed(const int16_t *pcm, size_t samples);

/** 清空引擎内部状态（对话结束后调用，避免残留音频串扰下一次） */
void wake_word_reset(void);

#ifdef __cplusplus
}
#endif
