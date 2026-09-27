/*
 * xiaozhi.h — 小智 AI 客户端 (官方 WebSocket 协议)
 *
 * 流程:
 *   1. 开机/重连时 HTTP POST 到 api.tenclass.net/xiaozhi/ota/ 做设备激活/取配置
 *   2. 拿到 websocket.url + token 后连 WebSocket (Protocol-Version: 1)
 *   3. 按住说话: 麦克风 16kHz PCM -> Opus(60ms/960样本) -> 二进制帧发给服务端
 *   4. 服务端回 TTS 音频(Opus 二进制帧) -> 解码 -> 功放播放
 *   5. 服务端回 llm 消息里的 emotion 字段 -> 驱动 OLED 表情
 *
 * WiFi 由 main.c 统一管理(APSTA), 本模块只用 ai_client_notify_online() 感知联网。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* 对话触发: 由 main.c 的触摸逻辑调用 */
void ai_client_start_listening(void);   /* 按下 */
void ai_client_stop_listening(void);    /* 松开 */
void ai_client_abort(void);             /* 打断当前播报 */

/* 初始化(创建任务, 不阻塞) */
void ai_client_init(void);

/* 联网状态, 由 main.c 的 WiFi 事件回调同步进来 */
void ai_client_notify_online(bool online);
bool ai_client_online(void);

/* 当前状态字符串, 给网页/串口看 */
const char *ai_client_state_str(void);

/* 是否正在对话中(聆听或播报)。外层用它来屏蔽触摸手势,
   免得播报时功放/喇叭的电磁耦合把触摸脚打成假触发。 */
bool ai_client_is_busy(void);

/* 是否正在播报(AI 说话)。
 * ★ 屏蔽触摸只能用这个, 不能用 is_busy(): 连续对话时设备几乎一直处于
 *   "聆听"状态, 用 is_busy() 会让触摸永远失效(双击切灯/单击撸猫都没反应)。 */
bool ai_client_is_speaking(void);

/* 设备激活码(未激活时非空), 去 xiaozhi.me 绑定用 */
const char *ai_client_activation_code(void);

/* 运行统计 */
uint32_t ai_client_tx_frames(void);
uint32_t ai_client_rx_frames(void);

/* ---------- 音乐互斥: 挂起/恢复整条 AI 音频链 ----------
 *
 * 播在线音乐时整个挂起, 音乐停止后恢复。为什么必须互斥:
 *   1. CPU: 唤醒词引擎(WakeNet9) + Opus 编码每 60ms 就要吃掉一大块时间,
 *      而两者都和音乐解码抢同一个核 —— 音乐必然一卡一卡。
 *   2. 喇叭: 音乐和 AI 播报共用同一个 I2S/功放, 不互斥就会互相插话。
 *
 * 挂起期间: 不读麦克风、不喂唤醒词、不上行、不播 TTS(下行的音频帧直接丢)。
 *           WebSocket 保持连接(不占带宽), 所以恢复是即时的。
 * 恢复时: 重置唤醒词引擎(挂起期间没喂音频, 内部状态是脏的)并重新武装。
 */
void ai_client_set_audio_suspended(bool suspended);
bool ai_client_get_audio_suspended(void);

/* ---------- 免触摸对话 (本地 VAD) ----------
 * 没有 esp-sr 唤醒词时, 用麦克风能量判断有人在说话:
 * 连续 180ms 人声 → 自动开始; 连续 800ms 静音 → 自动结束.
 * 播报期间不读麦克风, 播报结束后还有 600ms 冷却, 防止自触发. */
void  ai_client_set_vad(bool on);
bool  ai_client_get_vad(void);
/* 唤醒词引擎状态: 就绪则为 true, 名字形如 "wn9_nihaoxiaozhi_tts" */
bool        ai_client_wake_ready(void);
const char *ai_client_wake_name(void);

/* 下行音频链路自检: 播放 seconds 秒 440Hz 提示音(走真实解码/播放通路) */
void ai_client_selftest_tts(int seconds);
/* 唤醒路径自检: 模拟一次唤醒命中, 顺带验证 wake_word_reset() 不再崩溃 */
void ai_client_test_wake(const char *word);
/* MCP 工具执行链路自检: 用伪造的 tools/call 直接验证 灯效/亮度/温湿度 工具能否执行 */
void ai_client_test_mcp(void);
/* WebSocket 是否已完成 hello(可以收发音频) */
bool ai_client_ws_ready(void);
/* 唤醒应答音开关 (排查喇叭杂音时关掉, 用来区分是提示音本身还是后续 WiFi 活动) */
bool ai_client_get_wake_beep(void);
void ai_client_set_wake_beep(bool on);
/* 最近一帧麦克风能量(均方根), 用来标定触发灵敏度 */
float ai_client_mic_rms(void);

/* 自定义服务端地址(自建 xiaozhi-server 用, 传 NULL 恢复官方) */
void ai_client_set_server(const char *ota_url, const char *ws_url, const char *token);
