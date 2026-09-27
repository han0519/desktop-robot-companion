/*
 * ir_obstacle.h — 红外避障传感器 FC-51 x2
 * 数字输出: 低电平=检测到障碍物/手靠近
 *
 * 需由调用方周期性调用 ir_obstacle_update() 喂时间戳,
 * 之后 *_detected() 返回的是消抖+迟滞后的稳定状态。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    IR_LEFT = 0,
    IR_RIGHT = 1,
} ir_channel_t;

/* 读取原始电平并更新所有通道的消抖状态 */
void ir_obstacle_update(uint32_t now_ms);

void ir_obstacle_init(void);
bool ir_obstacle_left_detected(void);   /* 左传感器 true=有东西 (消抖后) */
bool ir_obstacle_right_detected(void);  /* 右传感器 true=有东西 (消抖后) */
bool ir_obstacle_detected(void);        /* 任意一个检测到 */

bool ir_obstacle_left_raw(void);        /* 左原始值, 未消抖 */
bool ir_obstacle_right_raw(void);       /* 右原始值, 未消抖 */
