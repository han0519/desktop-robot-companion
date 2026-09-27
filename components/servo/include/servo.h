/*
 * servo.h — 3路舵机驱动 (LEDC 50Hz)
 *
 * 运动由后台伺服任务每 SERVO_TASK_PERIOD_MS 毫秒插值生成:
 *   - servo_set_target()  非阻塞, 只设目标 + 限速 (跟手/联动推荐)
 *   - servo_smooth_to()   阻塞版, 设置目标后等待到位 (动作序列用)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SERVO_PAN = 0,   /* 左右转头 */
    SERVO_TILT = 1,  /* 上下点头 */
    SERVO_ARM = 2    /* 长臂台灯 */
} servo_index_t;

/* 初始化所有舵机(全部转到中位90°)并启动后台伺服任务 */
void servo_init(void);

/* 设置指定舵机角度 (立即跳变, 无插值) */
void servo_set_angle(servo_index_t idx, int angle);

/* 非阻塞设置目标角度, speed_dps 为最大角速度(度/秒), <=0 表示不限速 */
void servo_set_target(servo_index_t idx, int angle, int speed_dps);

/* 平滑转到目标角度并等待到位 (duration_ms 为标称总时长) */
void servo_smooth_to(servo_index_t idx, int target_angle, int duration_ms);

/* 获取当前角度 */
int servo_get_angle(servo_index_t idx);

/* 获取目标角度 */
int servo_get_target(servo_index_t idx);

/* 是否仍在运动 */
bool servo_is_moving(servo_index_t idx);

/* 摇头动作 (左右摆动, 用于"否定"语义) */
void servo_shake_head(void);

/* 点头动作 (上下摆动, 用于"肯定"语义) */
void servo_nod_head(void);
