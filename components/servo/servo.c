/*
 * servo.c — 3路舵机驱动实现
 *
 * 轨迹由后台任务以 SERVO_TASK_PERIOD_MS 周期生成: 比例逼近(ease-out) + 速度上限,
 * 因此调用方可以高频刷新目标而不会阻塞自身循环。
 */
#include "servo.h"
#include "config.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "servo";

#define SERVO_NUM 3

static const struct {
    int gpio;
    ledc_channel_t ch;
} s_servo_map[3] = {
    [SERVO_PAN]  = { SERVO_PAN_GPIO,  SERVO_CH_PAN },
    [SERVO_TILT] = { SERVO_TILT_GPIO, SERVO_CH_TILT },
    [SERVO_ARM]  = { SERVO_ARM_GPIO,  SERVO_CH_ARM },
};

/* ========== 运动状态 ========== */
static int   s_angle[SERVO_NUM]  = {90, 90, 90};  /* 对外返回的整数角度 */
static float s_pos[SERVO_NUM];                    /* 内部浮点位置 */
static float s_target[SERVO_NUM];                 /* 目标位置 */
static float s_speed[SERVO_NUM];                  /* 最大角速度 (°/s), <=0 不限速 */

/* 避免依赖 libm */
static int fi2i(float v)
{
    return (int)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

static float fabs_own(float v)
{
    return (v >= 0.0f) ? v : -v;
}

static bool idx_ok(servo_index_t idx)
{
    return (idx >= 0 && idx < SERVO_NUM);
}

/* 角度转 duty: 0°=500us, 180°=2500us, 20ms周期, 13位分辨率(0~8191) */
static uint32_t angle_to_duty(int angle)
{
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    uint32_t us = 500 + (uint32_t)angle * 2000 / 180;
    return us * 8192 / 20000;
}

/* 应用舵机方向反转 (TILT) */
static int apply_invert(servo_index_t idx, int angle)
{
#if SERVO_TILT_INVERT
    if (idx == SERVO_TILT) angle = 180 - angle;
#endif
    return angle;
}

/* 机械限位: 防止舵机超过结构允许范围导致堵转 */
static int clamp_angle(servo_index_t idx, int angle)
{
    if (angle < SERVO_ANGLE_MIN) angle = SERVO_ANGLE_MIN;
    if (angle > SERVO_ANGLE_MAX) angle = SERVO_ANGLE_MAX;
    if (idx == SERVO_TILT) {
        if (angle < SERVO_TILT_PHYS_MIN) angle = SERVO_TILT_PHYS_MIN;
        if (angle > SERVO_TILT_PHYS_MAX) angle = SERVO_TILT_PHYS_MAX;
    }
    return angle;
}

/* 写入 LEDC 硬件 */
static void servo_write(servo_index_t idx, float pos)
{
    int angle = fi2i(pos);
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    int phys = apply_invert(idx, angle);
    ledc_set_duty(SERVO_MODE, s_servo_map[idx].ch, angle_to_duty(phys));
    ledc_update_duty(SERVO_MODE, s_servo_map[idx].ch);
    s_angle[idx] = angle;
}

/* ========== 后台伺服任务 ========== */
static void servo_control_task(void *arg)
{
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(SERVO_TASK_PERIOD_MS);

    while (1) {
        vTaskDelayUntil(&last_wake, period);

        for (int i = 0; i < SERVO_NUM; i++) {
            float diff = s_target[i] - s_pos[i];

            if (fabs_own(diff) < SERVO_ARRIVE_EPS) {
                if (s_pos[i] != s_target[i]) {
                    s_pos[i] = s_target[i];
                    servo_write(i, s_pos[i]);
                }
                continue;
            }

            /* 比例逼近 (天然 ease-out) */
            float step = diff * SERVO_EASE_GAIN;

            /* 速度上限 */
            float vmax = (s_speed[i] > 0.0f)
                       ? (s_speed[i] * (float)SERVO_TASK_PERIOD_MS / 1000.0f)
                       : 180.0f;
            if (step >  vmax) step =  vmax;
            if (step < -vmax) step = -vmax;

            /* 最小步进: 避免尾段无限逼近导致肉眼可见的停顿 */
            if (step > 0.0f && step < SERVO_MIN_STEP_DEG) step = SERVO_MIN_STEP_DEG;
            if (step < 0.0f && step > -SERVO_MIN_STEP_DEG) step = -SERVO_MIN_STEP_DEG;

            if (fabs_own(step) > fabs_own(diff)) step = diff;

            s_pos[i] += step;
            servo_write(i, s_pos[i]);
        }
    }
}

void servo_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = SERVO_MODE,
        .timer_num  = SERVO_TIMER,
        .freq_hz    = SERVO_FREQ_HZ,
        .duty_resolution = SERVO_RESOLUTION,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    for (int i = 0; i < SERVO_NUM; i++) {
        /* 长臂台灯舵机(SERVO_ARM)已从硬件上拆除, 不再配置它的 PWM 通道,
           免得 GPIO14 上一直挂着 50Hz 信号。左右/点头两个照常工作。 */
        if (i == SERVO_ARM) {
            s_pos[i] = s_target[i] = 90.0f;
            s_speed[i] = 0.0f;
            s_angle[i] = 90;
            continue;
        }
        ledc_channel_config_t ch = {
            .gpio_num   = s_servo_map[i].gpio,
            .speed_mode = SERVO_MODE,
            .channel    = s_servo_map[i].ch,
            .timer_sel  = SERVO_TIMER,
            .duty       = angle_to_duty(90),
            .hpoint     = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
        s_pos[i]    = 90.0f;
        s_target[i] = 90.0f;
        s_speed[i]  = 0.0f;
        s_angle[i]  = 90;
        servo_write(i, s_pos[i]);
    }
    ESP_LOGI(TAG, "servo init: PAN=GPIO%d, TILT=GPIO%d (长臂已拆除, 未启用)",
             SERVO_PAN_GPIO, SERVO_TILT_GPIO);

    xTaskCreatePinnedToCore(servo_control_task, "servo_task",
                            SERVO_TASK_STACK, NULL,
                            SERVO_TASK_PRIO, NULL, SERVO_TASK_CORE);
    ESP_LOGI(TAG, "servo control task started @%dms", SERVO_TASK_PERIOD_MS);
}

void servo_set_angle(servo_index_t idx, int angle)
{
    if (!idx_ok(idx)) return;
    angle = clamp_angle(idx, angle);
    s_target[idx] = (float)angle;
    s_speed[idx]  = 0.0f;
    s_pos[idx]    = (float)angle;
    servo_write(idx, s_pos[idx]);
}

void servo_set_target(servo_index_t idx, int angle, int speed_dps)
{
    if (!idx_ok(idx)) return;
    angle = clamp_angle(idx, angle);
    s_target[idx] = (float)angle;
    s_speed[idx]  = (float)speed_dps;
}

void servo_smooth_to(servo_index_t idx, int target_angle, int duration_ms)
{
    if (!idx_ok(idx)) return;
    target_angle = clamp_angle(idx, target_angle);

    int start = s_angle[idx];
    int delta = target_angle - start;

    /* 死区: 小于2度不产生运动 */
    if (delta > -2 && delta < 2) {
        s_target[idx] = (float)target_angle;
        return;
    }

    if (duration_ms <= 0) duration_ms = 1;

    /* 轨迹是渐近收敛的, 给速度留出余量以保证在 duration 内基本到位 */
    float dps = (float)delta * 1000.0f / (float)duration_ms * SERVO_SMOOTH_SPEEDUP;
    if (dps < 0.0f) dps = -dps;
    if (dps < SERVO_MIN_SPEED_DPS) dps = SERVO_MIN_SPEED_DPS;

    s_target[idx] = (float)target_angle;
    s_speed[idx]  = dps;

    /* 保持原有阻塞语义: 等待到位或超时 */
    TickType_t t0 = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(duration_ms + 400);
    while ((xTaskGetTickCount() - t0) < timeout) {
        if (!servo_is_moving(idx)) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

int servo_get_angle(servo_index_t idx)
{
    if (!idx_ok(idx)) return 90;
    return s_angle[idx];
}

int servo_get_target(servo_index_t idx)
{
    if (!idx_ok(idx)) return 90;
    return fi2i(s_target[idx]);
}

bool servo_is_moving(servo_index_t idx)
{
    if (!idx_ok(idx)) return false;
    return fabs_own(s_target[idx] - s_pos[idx]) >= SERVO_ARRIVE_EPS;
}

void servo_shake_head(void)
{
    /* 快速左右摇头 2 次 */
    int center = s_angle[SERVO_PAN];
    for (int i = 0; i < 2; i++) {
        servo_smooth_to(SERVO_PAN, center - 25, 150);
        servo_smooth_to(SERVO_PAN, center + 25, 150);
    }
    servo_smooth_to(SERVO_PAN, center, 150);
}

void servo_nod_head(void)
{
    /* 快速上下点头 2 次 */
    int center = s_angle[SERVO_TILT];
    for (int i = 0; i < 2; i++) {
        servo_smooth_to(SERVO_TILT, center - 20, 120);
        servo_smooth_to(SERVO_TILT, center + 20, 120);
    }
    servo_smooth_to(SERVO_TILT, center, 120);
}
