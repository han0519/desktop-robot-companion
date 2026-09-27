/*
 * touch_sensor.c — 触摸传感器实现
 *
 * 硬件已只保留【头部】一路触摸 (GPIO15)。身体触摸(GPIO16)已拆除,
 * 原来的"按住说话/切灯色"改由头部触摸手势承担:
 *   单击/长按 = 撸猫, 双击 = 切换灯效 (见 main.c 的 sensor_task)。
 */
#include "touch_sensor.h"
#include "config.h"
#include "driver/gpio.h"

void touch_sensor_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << SENSOR_TOUCH_HEAD_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

bool touch_sensor_head_pressed(void)
{
    return gpio_get_level(SENSOR_TOUCH_HEAD_GPIO) == 1;
}

/* 身体触摸已拆除, 恒为未按下 (保留接口免得其它地方编译不过) */
bool touch_sensor_body_pressed(void)
{
    return false;
}

bool touch_sensor_pressed(void)
{
    return touch_sensor_head_pressed();
}
