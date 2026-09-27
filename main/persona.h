/*
 * persona.h — 人格引擎: 让摆件有"自己的状态"
 *
 * 没有这一层, 它只是"有问必答的喇叭": 服务器不说话它就是死的。
 * 这一层在设备本地维护四个会随时间演化的数值 ——
 *
 *     valence   心情   -100(很丧) ~ +100(很嗨)
 *     energy    精力   0(困)      ~ 100(精神)
 *     boredom   无聊   0(有事做)  ~ 100(没人理很久)
 *     intimacy  亲密   0(陌生)    ~ 100(很熟, 只涨不跌)
 *
 * 事件(被摸/对话/被认同/放歌)改变它们, 时间让它们慢慢回落;
 * 输出端再按数值驱动"自主转头/心情灯/表情"。
 *
 * 设计红线:
 *   1. 【绝不】在 WebSocket 回调里做任何阻塞动作(点头要等舵机到位几百 ms),
 *      所以事件只置标志, 由 persona 自己的低优先级任务执行。
 *   2. AI 正在说话/正在放音乐时, 自主行为全部让位 —— 不抢戏。
 *   3. 【灯环不归人格系统管】: 灯只受正常开关灯控制, 以及放歌时的
 *      音乐律动(见 light.h), persona 不写任何灯的状态。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float valence;    /* 心情  -100..100 */
    float energy;     /* 精力  0..100   */
    float boredom;    /* 无聊  0..100   */
    float intimacy;   /* 亲密  0..100   */
} persona_mood_t;

/* 初始化并启动人格任务 */
void persona_init(void);

/* ---- 事件(其他模块调用, 只改数值, 不阻塞) ---- */
void persona_event_touch(void);          /* 被摸头: 心情+ 亲密+ */
void persona_event_dialog_start(void);   /* 开始一轮对话: 不无聊了 */
void persona_event_dialog_end(void);     /* 一轮对话结束: 心情略+ */
void persona_event_music(bool on);       /* 开始/结束放歌 */

/* ---- 从 AI 的回复文本里识别"认同/不认同" → 排队点头/摇头 ----
 * 在收到 llm 消息(带回复文本)时调用。内部带 8 秒节流,
 * 一次回复最多做一次动作, 而且"好的/嗯"这类应答词不会触发。 */
void persona_scan_reply(const char *text);

/* ---- 读取(网页 /status 和 MCP 工具用) ---- */
const persona_mood_t *persona_get(void);
/* 一句话描述当前状态, 例: "挺开心的, 有点无聊, 精力 72%, 亲密度 13%" */
const char *persona_mood_str(void);
