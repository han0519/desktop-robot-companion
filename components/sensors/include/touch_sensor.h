/*
 * touch_sensor.h — 触摸传感器 TTP223 x2
 * 数字输出: 高电平=触摸中
 */
#pragma once
#include <stdbool.h>

void touch_sensor_init(void);
bool touch_sensor_head_pressed(void);  /* 头部触摸 → 撸猫害羞 */
bool touch_sensor_body_pressed(void);  /* 身体触摸 → 切换灯色 */
bool touch_sensor_pressed(void);       /* 任意一个触摸 (兼容旧代码) */
