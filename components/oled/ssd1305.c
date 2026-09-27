/**
 * @file ssd1305.c
 * @brief SSD1305 128x64 I2C OLED 驱动实现
 */
#include "ssd1305.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

static const char *TAG = "ssd1305";

static uint8_t s_fb[SSD1305_FB_SIZE];
static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

/* 保存引脚参数, 供总线自愈时重建 */
static i2c_port_t s_port  = 0;
static int        s_sda   = 0;
static int        s_scl   = 0;
static uint32_t   s_speed = 0;
/* 连续传输失败计数: 达到阈值就判定"总线锁死"并触发自愈 */
static int        s_fail  = 0;
static bool       s_in_recover = false;
/* 传输结果记账(定义在文件后面, 这里先声明, 因为 write_cmd 在它之前) */
static void note_result(esp_err_t e);

/* 总线互斥锁。
 * 表情渲染任务(face)和初始化流程会并发调用 i2c_master_transmit(),
 * 没有锁时两边挤在同一个 s_dev 上, 驱动直接返回 ESP_ERR_INVALID_STATE
 * (实测表现为 OLED 初始化失败 → 屏幕全黑)。 */
static SemaphoreHandle_t s_lock = NULL;

/* 统一的发送入口: 加锁 → 发送 → 解锁。
 * 注意 note_result() 必须在锁外调用, 否则它触发的总线自愈会死锁。 */
static esp_err_t xmit(const uint8_t *buf, size_t len)
{
    if (!s_dev) return ESP_FAIL;
    /* 递归锁: 总线自愈(内部会再调 write_cmd → xmit)需要重入 */
    if (s_lock && xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(300)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    /* 超时给足 200ms: 总线被舵机 PWM / WiFi 突发干扰时单次传输会明显变慢,
       超时太小会把还在正常进行的传输判成失败。 */
    esp_err_t e = i2c_master_transmit(s_dev, buf, len, pdMS_TO_TICKS(200));
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
    return e;
}
/* 一帧要发 8 页, 阈值取 10 = 容忍一整帧偶发失败, 连续两帧才自愈 */
#define SSD1305_FAIL_RECOVER_TH   10

/* ---------- 底层 I2C 发送 ---------- */

/**
 * @brief 向 SSD1305 发送一串命令
 * 控制字节 0x00 表示后续为命令流 (Co=0, D/C#=0)
 */
static esp_err_t ssd1305_write_cmd(const uint8_t *cmd, size_t len)
{
    if (!s_dev) return ESP_FAIL;
    /* 控制字节 + 命令 */
    uint8_t buf[1 + len];
    buf[0] = 0x00;
    memcpy(buf + 1, cmd, len);
    /* 别把超时改小! 之前改成 20ms 之后传输失败率明显上升, 屏幕数据写一半就
       中断, 直接变成雪花。I2C 总线被舵机 PWM / WiFi 突发干扰时本来就会慢,
       必须留足余量。 */
    esp_err_t e = xmit(buf, sizeof(buf));
    note_result(e);
    return e;
}

/**
 * @brief 向 SSD1305 发送指定页区间的显示数据（显存）
 * 控制字节 0x40 表示后续为数据流 (Co=0, D/C#=1)
 * 每页前先设置页地址和列地址
 */
static esp_err_t ssd1305_write_display_data(uint8_t page_start, uint8_t page_end)
{
    if (!s_dev) return ESP_FAIL;
    if (page_start >= SSD1305_PAGES) page_start = 0;
    if (page_end >= SSD1305_PAGES) page_end = SSD1305_PAGES - 1;
    if (page_end < page_start) return ESP_OK;

    esp_err_t first_err = ESP_OK;
    uint8_t header[3];
    for (uint8_t page = page_start; page <= page_end; page++) {
        uint8_t payload[1 + SSD1305_WIDTH];
        payload[0] = 0x40;
        memcpy(payload + 1, s_fb + page * SSD1305_WIDTH, SSD1305_WIDTH);

        /* ★ 单页最多重试 3 次。
           总线被舵机/WiFi 干扰时是"间歇性"失败 —— 不是连续 10 次,
           所以总线自愈那条路根本不会被触发(阈值是连续失败)。
           而失败的那一页偏偏常常是"眼睛所在页", 结果就是屏幕看着像没亮。
           重试一次的开销极小, 但能把绝大多数偶发失败吃掉。 */
        esp_err_t r = ESP_FAIL;
        for (int attempt = 0; attempt < 3; attempt++) {
            /* 设置页地址 */
            header[0] = 0xB0 | page;
            /* 设置列地址低4位 + 高4位 */
            header[1] = 0x00; /* 列低地址 = 0 */
            header[2] = 0x10; /* 列高地址 = 0 */
            r = ssd1305_write_cmd(header, 3);
            if (r == ESP_OK) r = xmit(payload, sizeof(payload));
            if (r == ESP_OK) break;
            esp_rom_delay_us(200);      /* 稍等再试, 给总线恢复时间 */
        }
        if (r != ESP_OK) {
            if (first_err == ESP_OK) first_err = r;
            note_result(r);
        }
    }
    return first_err;
}

/* ---------- 初始化序列 ---------- */

static const uint8_t s_init_cmds[] = {
    0xAE,          /* Display OFF */
    0xD5, 0x80,    /* Clock Divide Ratio / Osc Freq */
    0xA8, 0x3F,    /* Multiplex Ratio = 64-1 */
    0xD3, 0x00,    /* Display Offset = 0 */
    0x40,          /* Start Line = 0 */
    0x8D, 0x14,    /* Charge Pump Enable */
    0x20, 0x00,    /* Memory Mode = Horizontal */
    /* 0xA1 / 0xC8 (段重映射 + COM 扫描方向) 不在这里固定,
       改由 ssd1305_set_flip() 下发, 便于运行时切换屏幕方向 */
    0xDA, 0x12,    /* COM Pins Config = Alternative */
    0x81, 0x7F,    /* Contrast = 127 */
    0xD9, 0xF1,    /* Pre-charge Period */
    0xDB, 0x40,    /* VCOMH Deselect Level */
    0xA4,          /* Entire Display ON (follows RAM) */
    0xA6,          /* Normal Display (not inverted) */
    0xAF,          /* Display ON */
};

/* ---------- I2C 总线解锁与自愈 ----------
 * 参考小智官方 walle-s3 板的做法 (walle_s3_board.cc:373-416)。
 *
 * 上电或受干扰(舵机 PWM / WS2812 的 RMT / WiFi 突发)时, 从设备可能正卡在
 * 发送数据的半途, 把 SDA 死死拉低 —— 此后每一次 i2c_master_transmit 都超时。
 * 我们串口里刷屏的 "I2C software timeout" 和屏幕雪花就是这么来的:
 * 总线锁死后驱动再也拿不到 ACK, 显存只写进一半。
 *
 * 解法(硬件标准做法): 临时把 SCL/SDA 当普通开漏 GPIO, 翻转 SCL 若干次
 * 把从设备移位寄存器里剩下的位挤出去, 再补一个 STOP 条件让它回到空闲态。 */

static void i2c_bus_bitbang_recover(int sda_pin, int scl_pin)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << sda_pin) | (1ULL << scl_pin),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level((gpio_num_t)scl_pin, 1);
    gpio_set_level((gpio_num_t)sda_pin, 1);
    /* 内部上拉约 45kΩ, 要把总线电容充上去需要一点时间。
       延时太短会在上电瞬间误判成"被从设备拉低"(假阳性)。 */
    esp_rom_delay_us(200);

    if (gpio_get_level((gpio_num_t)sda_pin) == 0) {
        ESP_LOGW(TAG, "I2C 总线锁死(SDA 被拉低) → 翻转 SCL 解锁");
        for (int i = 0; i < 10; i++) {
            gpio_set_level((gpio_num_t)scl_pin, 0);
            esp_rom_delay_us(5);
            gpio_set_level((gpio_num_t)scl_pin, 1);
            esp_rom_delay_us(5);
        }
        /* STOP 条件: SCL 保持高时, SDA 由低变高 */
        gpio_set_level((gpio_num_t)sda_pin, 0);
        esp_rom_delay_us(5);
        gpio_set_level((gpio_num_t)scl_pin, 1);
        esp_rom_delay_us(5);
        gpio_set_level((gpio_num_t)sda_pin, 1);
        esp_rom_delay_us(5);
    }
    /* 把引脚还给 I2C 外设 */
    gpio_reset_pin((gpio_num_t)sda_pin);
    gpio_reset_pin((gpio_num_t)scl_pin);
    esp_rom_delay_us(10);
}

/* 完整重建总线: 拆设备 → 拆总线 → 位操作解锁 → 重建 → 重发初始化序列 */
static esp_err_t ssd1305_bus_reinit(void)
{
    if (s_in_recover) return ESP_FAIL;      /* 防递归 */
    s_in_recover = true;
    /* 持有总线锁: 不能在别的任务正在发送时把 s_dev 拆掉 */
    if (s_lock) xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(500));

    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    if (s_bus) { i2c_del_master_bus(s_bus);       s_bus = NULL; }

    i2c_bus_bitbang_recover(s_sda, s_scl);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port               = s_port,
        .sda_io_num             = s_sda,
        .scl_io_num             = s_scl,
        .clk_source             = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt      = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret == ESP_OK) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = SSD1305_I2C_ADDR,
            .scl_speed_hz    = s_speed,
        };
        ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    }
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        ret = ssd1305_write_cmd(s_init_cmds, sizeof(s_init_cmds));
    }

    ESP_LOGW(TAG, "I2C 总线自愈 %s", ret == ESP_OK ? "成功" : "失败");
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
    s_in_recover = false;
    return ret;
}

/* 传输结果记账: 连续失败到阈值就自愈。成功一次就清零。 */
static void note_result(esp_err_t e)
{
    if (e == ESP_OK) { s_fail = 0; return; }
    if (++s_fail >= SSD1305_FAIL_RECOVER_TH) {
        ESP_LOGW(TAG, "I2C 连续失败 %d 次 → 触发总线自愈", s_fail);
        s_fail = 0;
        ssd1305_bus_reinit();
    }
}

/* ---------- 公开 API ---------- */

esp_err_t ssd1305_init(i2c_port_t port, int sda_pin, int scl_pin, uint32_t clk_speed)
{
    ESP_LOGI(TAG, "init I2C port=%d sda=%d scl=%d speed=%lu",
             port, sda_pin, scl_pin, (unsigned long)clk_speed);

    /* 记住参数, 之后总线锁死时靠它们重建 */
    s_port = port; s_sda = sda_pin; s_scl = scl_pin; s_speed = clk_speed;

    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();

    /* 建总线之前先解锁: 上电瞬间从设备可能正把 SDA 拉低 */
    i2c_bus_bitbang_recover(sda_pin, scl_pin);

    /* 1. 创建 I2C Master 总线 */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = port,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create master bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. 添加 SSD1305 设备 */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SSD1305_I2C_ADDR,
        .scl_speed_hz = clk_speed,
    };
    ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. 探测设备是否在线(带重试: 上电瞬间屏幕可能还没准备好,
          或者上次掉电时卡在半途把 SDA 拉低了) */
    for (int attempt = 0; attempt < 3; attempt++) {
        ret = i2c_master_probe(s_bus, SSD1305_I2C_ADDR, pdMS_TO_TICKS(200));
        if (ret == ESP_OK) break;
        ESP_LOGW(TAG, "I2C probe 0x%02X 第 %d 次失败: %s → 解锁总线后重试",
                 SSD1305_I2C_ADDR, attempt + 1, esp_err_to_name(ret));
        ssd1305_bus_reinit();
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C probe 0x%02X 连续 3 次失败: %s (检查接线与供电!)",
                 SSD1305_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2C device 0x%02X found", SSD1305_I2C_ADDR);

    /* 4. 等待上电稳定 */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 5. 发送初始化命令序列 */
    ret = ssd1305_write_cmd(s_init_cmds, sizeof(s_init_cmds));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init cmd failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 6. 清屏并显示 */
    ssd1305_clear();
    ssd1305_display();

    ESP_LOGI(TAG, "SSD1305 initialized OK");
    return ESP_OK;
}

void ssd1305_clear(void)
{
    memset(s_fb, 0x00, sizeof(s_fb));
}

void ssd1305_fill(void)
{
    memset(s_fb, 0xFF, sizeof(s_fb));
}

void ssd1305_set_pixel(int x, int y, uint8_t color)
{
    if (x < 0 || x >= SSD1305_WIDTH || y < 0 || y >= SSD1305_HEIGHT) return;
    uint16_t idx = (y >> 3) * SSD1305_WIDTH + x;
    uint8_t bit = (uint8_t)(1 << (y & 0x07));
    if (color) s_fb[idx] |= bit;
    else       s_fb[idx] &= (uint8_t)~bit;
}

uint8_t ssd1305_get_pixel(int x, int y)
{
    if (x < 0 || x >= SSD1305_WIDTH || y < 0 || y >= SSD1305_HEIGHT) return 0;
    uint16_t idx = (y >> 3) * SSD1305_WIDTH + x;
    return (s_fb[idx] >> (y & 0x07)) & 0x01;
}

uint8_t *ssd1305_get_framebuffer(void)
{
    return s_fb;
}

esp_err_t ssd1305_display(void)
{
    return ssd1305_write_display_data(0, SSD1305_PAGES - 1);
}

esp_err_t ssd1305_display_pages(uint8_t page_start, uint8_t page_end)
{
    return ssd1305_write_display_data(page_start, page_end);
}

static bool s_flip_x = false;
static bool s_flip_y = false;

void ssd1305_set_flip(bool flip_x, bool flip_y)
{
    s_flip_x = flip_x;
    s_flip_y = flip_y;
    /* 0xA0 = 段重映射关闭(左右镜像), 0xA1 = 正常
       0xC0 = COM 扫描正向(上下镜像), 0xC8 = 反向 */
    uint8_t cmd[2] = { flip_x ? 0xA0 : 0xA1, flip_y ? 0xC0 : 0xC8 };
    ssd1305_write_cmd(cmd, 2);
    ESP_LOGI(TAG, "display flip: X=%d Y=%d", (int)flip_x, (int)flip_y);
}

void ssd1305_get_flip(bool *flip_x, bool *flip_y)
{
    if (flip_x) *flip_x = s_flip_x;
    if (flip_y) *flip_y = s_flip_y;
}

void ssd1305_reassert(void)
{
    if (!s_dev) return;
    /* 只发「非破坏性」配置命令, 不含 0xAE, 所以不会闪屏 */
    const uint8_t c[] = {
        0x20, 0x00,                          /* 水平寻址模式 */
        0xA4,                                /* 输出跟随 RAM */
        0xA6,                                /* 正常显示(非反白) */
        0xD3, 0x00,                          /* 显示偏移 = 0 */
        0x40,                                /* 起始行 = 0 */
        0xDA, 0x12,                          /* COM 引脚配置 */
        s_flip_x ? 0xA0 : 0xA1,              /* 段重映射(左右) */
        s_flip_y ? 0xC0 : 0xC8,              /* COM 扫描方向(上下) */
        0x8D, 0x14,                          /* 电荷泵开 */
        0x81, 0x7F,                          /* 对比度 */
        0xAF,                                /* 开显示 */
    };
    ssd1305_write_cmd(c, sizeof(c));
}

void ssd1305_set_display_on(bool on)
{
    uint8_t cmd = on ? 0xAF : 0xAE;
    ssd1305_write_cmd(&cmd, 1);
}

void ssd1305_set_contrast(uint8_t contrast)
{
    uint8_t cmd[2] = {0x81, contrast};
    ssd1305_write_cmd(cmd, 2);
}
