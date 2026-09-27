/*
 * persona.c — 人格引擎实现
 *
 * 一个 1Hz 的低优先级任务负责两件事:
 *   1. 状态演化(没互动就变无聊/变困, 心情慢慢回到平静)
 *   2. 自主转头(没人理的时候自己到处看看 / 精力见底就低头打盹)
 * 外加执行"点头/摇头"这类要阻塞等舵机的动作 —— 别的地方只排队。
 *
 * ★ 灯环【不在】人格系统里: 灯只受两种东西控制 ——
 *     · 正常的开灯/关灯指令(网页/AI/摸头双击)
 *     · 放歌时自动进入的音乐律动(可随时关)
 *   人格状态只驱动表情和头部动作, 不碰灯。
 */
#include "persona.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "config.h"
#include "face.h"
#include "servo.h"
#include "xiaozhi.h"    /* ai_client_is_busy() */
#include "music.h"      /* music_is_active()   */

static const char *TAG = "persona";

#define PERSONA_TASK_STACK  4096
#define PERSONA_TICK_MS     1000

/* 舵机参数(和 mcp.c 里 AI 动作同一套, 只是自主动作放慢一点, 像"自己发呆") */
#define P_PAN_CENTER   90
#define P_PAN_SWING    42
#define P_TILT_CENTER  90
#define P_SPEED_DPS    80

/* 演化速率(每秒): 数值是"调出来的手感", 不是精确模型 */
#define ENERGY_DECAY_PER_S   0.02f   /* 100 → 0 约 83 分钟 */
#define BOREDOM_RISE_PER_S   0.05f   /* 0 → 100 约 33 分钟没人理 */
#define VALENCE_RELAX        0.004f  /* 心情每秒向 0 回落 0.4% 的差距 */

static struct {
    float    valence;
    float    energy;
    float    boredom;
    float    intimacy;
    int      pending_gesture;   /* 1=点头 -1=摇头 0=无(排队等任务执行) */
    uint32_t last_agree_ms;     /* 上次点头/摇头的时刻(节流) */
    uint32_t next_head_ms;      /* 下一次自主转头的时刻 */
} S = {
    .energy = 100.0f,
};

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ==================== 事件 ==================== */

void persona_event_touch(void)
{
    S.valence  = clampf(S.valence + 8.0f, -100.0f, 100.0f);
    S.intimacy = clampf(S.intimacy + 0.6f, 0.0f, 100.0f);
    S.boredom  = 0.0f;
}

void persona_event_dialog_start(void) { S.boredom = 0.0f; }

void persona_event_dialog_end(void)
{
    S.valence = clampf(S.valence + 2.0f, -100.0f, 100.0f);
    S.boredom = 0.0f;
}

void persona_event_music(bool on)
{
    if (on) S.valence = clampf(S.valence + 5.0f, -100.0f, 100.0f);
    S.boredom = 0.0f;
}

/* ==================== 认同识别 ==================== */

/* 只挑【强】信号词。像"好的/嗯/可以"这种应答词故意不收 ——
 * 否则它每句话都点头, 就成了点头机器, 反而假。 */
static const char *const AGREE_WORDS[] = {
    "你说得对", "你说的是对的", "说得对", "你说得没错", "完全正确",
    "确实如此", "一点没错", "答对了", "对的", "没错", "确实是", "的确是",
};
static const char *const DISAGREE_WORDS[] = {
    "不对", "不是的", "错了", "并不是", "其实不是", "不完全对", "恰恰相反",
};

void persona_scan_reply(const char *text)
{
    if (!text || !text[0]) return;

    uint32_t now = now_ms();
    if (now - S.last_agree_ms < 8000) return;    /* 一次回复最多做一次 */

    int g = 0;
    for (size_t i = 0; i < sizeof(AGREE_WORDS) / sizeof(AGREE_WORDS[0]) && !g; i++)
        if (strstr(text, AGREE_WORDS[i])) g = 1;
    for (size_t i = 0; i < sizeof(DISAGREE_WORDS) / sizeof(DISAGREE_WORDS[0]) && !g; i++)
        if (strstr(text, DISAGREE_WORDS[i])) g = -1;
    if (!g) return;

    S.last_agree_ms   = now;
    S.pending_gesture = g;
    S.valence = clampf(S.valence + (g > 0 ? 3.0f : -1.0f), -100.0f, 100.0f);
    ESP_LOGI(TAG, "AI 回复%s → 排队%s", g > 0 ? "认同" : "不认同",
             g > 0 ? "点头" : "摇头");
}

/* ==================== 自主转头 ==================== */

static void idle_head_move(void)
{
    int r = (int)(esp_random() % 100);

    if (S.energy < 18.0f) {
        /* 精力见底: 低头打盹, 而且很久才动一次(别把舵机摇坏) */
        servo_set_target(SERVO_TILT, P_TILT_CENTER + 4, 30);
        face_set_emotion_hold(FACE_SLEEPING, 15000);
        S.next_head_ms = now_ms() + 60000 + (esp_random() % 60000);
        ESP_LOGI(TAG, "精力 %.0f → 打盹", S.energy);
        return;
    }

    if (S.boredom > 55.0f) {
        /* 没人理久了: 自己到处看, 像在等谁回来 */
        if      (r < 35) servo_set_target(SERVO_PAN, P_PAN_CENTER - P_PAN_SWING, P_SPEED_DPS);
        else if (r < 70) servo_set_target(SERVO_PAN, P_PAN_CENTER + P_PAN_SWING, P_SPEED_DPS);
        else             servo_set_target(SERVO_PAN, P_PAN_CENTER, P_SPEED_DPS);
        face_set_emotion_hold(r < 50 ? FACE_BORED : FACE_CURIOUS, 6000);
        S.next_head_ms = now_ms() + 20000 + (esp_random() % 30000);
        ESP_LOGI(TAG, "无聊 %.0f → 自己转头看看", S.boredom);
        return;
    }

    /* 不无聊: 只是偶尔轻轻换个方向, 更像活物而不是哨兵 */
    if (r < 45) servo_set_target(SERVO_PAN, P_PAN_CENTER - 18, P_SPEED_DPS);
    else        servo_set_target(SERVO_PAN, P_PAN_CENTER + 18, P_SPEED_DPS);
    S.next_head_ms = now_ms() + 25000 + (esp_random() % 35000);
}

/* ==================== 主任务 ==================== */

static void persona_task(void *arg)
{
    S.next_head_ms = now_ms() + 20000;      /* 开机先安静 20 秒 */

    for (;;) {
        /* AI 在说话或在放音乐: 有事做 → 不无聊, 也不自主行动(不抢戏) */
        bool busy = ai_client_is_busy() || music_is_active();

        if (busy) {
            S.boredom = 0.0f;
        } else {
            S.energy  -= ENERGY_DECAY_PER_S;  if (S.energy  < 0.0f)   S.energy  = 0.0f;
            S.boredom += BOREDOM_RISE_PER_S;  if (S.boredom > 100.0f) S.boredom = 100.0f;
            S.valence += (0.0f - S.valence) * VALENCE_RELAX;
        }

        /* 排队中的点头/摇头(点头内部要阻塞等舵机, 只能在这里做) */
        if (S.pending_gesture) {
            int g = S.pending_gesture;
            S.pending_gesture = 0;
            if (g > 0) {
                face_set_emotion_hold(FACE_HAPPY, 2000);
                servo_nod_head();
                ESP_LOGI(TAG, "认同 → 点头");
            } else {
                face_set_emotion_hold(FACE_SUSPICIOUS, 2000);
                servo_shake_head();
                ESP_LOGI(TAG, "不认同 → 摇头");
            }
        } else if (!busy && !servo_is_moving(SERVO_PAN) && !servo_is_moving(SERVO_TILT) &&
                   (int32_t)(now_ms() - S.next_head_ms) >= 0) {
            idle_head_move();
        }

        vTaskDelay(pdMS_TO_TICKS(PERSONA_TICK_MS));
    }
}

/* ==================== 对外 ==================== */

void persona_init(void)
{
    /* 栈放内部 RAM: 这个任务会调 servo/face 的函数, 保守一点(现在内部 RAM 有 28KB 富余) */
    if (xTaskCreatePinnedToCore(persona_task, "persona", PERSONA_TASK_STACK,
                                NULL, 3, NULL, 0) != pdPASS)
        ESP_LOGW(TAG, "人格任务创建失败");
    else
        ESP_LOGI(TAG, "人格引擎启动 (心情/精力/无聊/亲密 + 自主转头 + 心情灯)");
}

const persona_mood_t *persona_get(void)
{
    static persona_mood_t m;
    m.valence  = S.valence;
    m.energy   = S.energy;
    m.boredom  = S.boredom;
    m.intimacy = S.intimacy;
    return &m;
}

const char *persona_mood_str(void)
{
    static char buf[96];
    const char *m;
    if      (S.valence >  45.0f) m = "特别开心";
    else if (S.valence >  15.0f) m = "挺开心的";
    else if (S.valence > -25.0f) m = "心情平静";
    else if (S.valence > -60.0f) m = "有点低落";
    else                         m = "很不开心";

    const char *b;
    if      (S.boredom > 70.0f) b = "有点无聊了";
    else if (S.boredom > 35.0f) b = "稍微有点闲";
    else                        b = "有事做着呢";

    snprintf(buf, sizeof(buf), "%s, %s, 精力 %d%%, 亲密度 %d%%",
             m, b, (int)S.energy, (int)S.intimacy);
    return buf;
}
