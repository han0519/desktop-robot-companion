/*
 * main.c — 机器人伴侣项目主程序
 *
 * 功能:
 *   - OLED 表情系统 (16种表情, 眨眼, 扫视, 微动画)
 *   - 3路舵机: 左右转头 / 上下点头 / 长臂台灯
 *   - WS2812 灯环 (彩虹渐变 / 台灯模式) + 板载RGB
 *   - 传感器: 红外避障(手靠近追踪) / 触摸(抚摸害羞) / DHT11温湿度
 *   - I2S 音频: INMP441麦克风 + MAX98357A功放
 *   - 小智 AI 对话 (WiFi, 语音识别, TTS, 动作指令)
 *   - BOOT 键切换表情
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"

#include "config.h"
#include "ssd1305.h"
#include "face.h"
#include "servo.h"
#include "ws2812.h"
#include "light.h"
#include "mcp.h"
#include "ir_obstacle.h"
#include "touch_sensor.h"
#include "music.h"
#include "dht11.h"
#include "mic_inmp441.h"
#include "speaker_max98357a.h"
#include "xiaozhi.h"
#include "persona.h"
#include "web_page.h"
#include "prov_page.h"
#include "wifi_screen.h"

static const char *TAG = "main";

/* ========== 全局状态 ========== */
static struct {
    bool tracking;             /* 是否正在追踪手 */
    int pan_target;            /* 转头目标角度 */
    int tilt_target;           /* 点头目标角度 */
    dht11_data_t env;          /* 温湿度 */
    bool env_valid;
    uint32_t last_env_read;    /* 上次温湿度读取时间 */
    bool boot_pressed;
    uint32_t boot_press_time;
    bool auto_cycle;           /* 表情自动播放开关 */
} s_state;
/* 灯的状态(灯效/亮度/主色)全部由 light.c 的灯效引擎持有,
   这里不再保留任何灯相关字段 —— 身体触摸已拆除, 改由 MCP + 触摸双击控制。 */

/* ========== 开机流程状态 ==========
 * 标准小智流程:
 *   未配过网 → 配网模式(只开热点, 屏幕上显示热点名/密码/网址)
 *            → 手机连热点 → 打开 192.168.4.1 填 WiFi
 *            → 连接成功 → 显示 IP 2 秒 → 进入表情系统
 *   配过网   → 直接进表情系统, 后台连路由
 *
 * 屏幕只有一块: 配网阶段画配网页, 其余时间归表情引擎, 二选一, 不能同时刷。 */
static bool     s_provisioning = true;    /* true = 屏幕归配网页 */
static bool     s_provisioned  = false;   /* NVS 里是否已有 WiFi 配置 */
static uint32_t s_prov_ok_ms   = 0;       /* 配网成功时刻(0=未成功) */
static uint32_t s_wifi_fail_ms = 0;       /* STA 开始连不上的时刻(0=未在重试) */
static char     s_target_ssid[33];        /* 当前要连的 SSID (只用于日志) */
#define WIFI_OK_HOLD_MS 2000              /* 成功后停留展示 IP 的时长 */
#define WIFI_CONNECT_TIMEOUT_MS 30000     /* 连不上这么久 → 退回配网模式 */

/* 热点名/密码: 配网页要显示, 所以声明在 face_task 之前 */
#define AP_SSID  "RobotFace"
#define AP_PASS  "12345678"

/* STA 联网状态 (小智 AI 需要外网) */
static bool s_sta_connected = false;
static char s_sta_ip[16] = "-";

/* NVS 配置读写 (真正实现在下面, 这里前置声明供 /reprov 等使用) */
static size_t cfg_get_str(const char *key, char *out, size_t max);
static void   cfg_set_str(const char *key, const char *val);

/* ========== 下行链路开机自检 ==========
 * 走的是和服务端下行音频完全相同的通路, 验证完把 TTS_SELFTEST_ON_BOOT 置 0 即可。 */
#define TTS_SELFTEST_ON_BOOT 0

/* 自检只在"烧录后的第一次开机"跑一次(标志存 NVS), 之后永久跳过 ——
 * 既能自动验证下行链路, 又不会每次开机都响。
 * 想再验一次: 把 NVS 里的 stest 命名空间清掉, 或改这个命名空间名。 */
static bool selftest_first_boot(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("stest", NVS_READWRITE, &h) != ESP_OK) return true;
    if (nvs_get_u8(h, "done7", &v) != ESP_OK || v == 0) {
        v = 1;
        nvs_set_u8(h, "done7", v);
        nvs_commit(h);
        nvs_close(h);
        return true;
    }
    nvs_close(h);
    return false;
}
static void selftest_task(void *arg)
{
    /* 只跑唤醒路径(提示音 → 上报唤醒词 → 上行 → 静音结束), 这正是之前崩溃的那条路。
       下行解码/播放已由 /ttstest 单独验证过, 不在这里跑(那需要更大的栈)。 */
    ESP_LOGI(TAG, "==== 唤醒路径自检开始 (应听到叮咚) ====");
    ai_client_test_wake("你好小智");
    ESP_LOGI(TAG, "==== 唤醒路径自检结束 ====");
    vTaskDelete(NULL);
}

/* 小栈看门狗: 必须等 WS 就绪后才创建自检任务。
 * 自检任务的栈较大, 如果开机就建好并一直等 WS, 会长期占住内部 RAM,
 * 把 WebSocket 任务要的 24KB 挤掉 → "Error create websocket task" → 死锁。
 * 自检栈本身放到 PSRAM(它只跑 CPU 代码, 不碰 DMA), 不占内部 RAM。 */
static void selftest_watch_task(void *arg)
{
    if (!selftest_first_boot()) {
        ESP_LOGI(TAG, "自检: NVS 标志显示本次开机已跑过, 跳过");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "自检: 看门狗已启动, 等 WS 就绪后跑唤醒路径自检");
    for (int i = 0; i < 300 && !ai_client_ws_ready(); i++)
        vTaskDelay(pdMS_TO_TICKS(1000));
    if (!ai_client_ws_ready()) {
        ESP_LOGW(TAG, "自检: WS 一直未就绪, 取消");
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    /* 栈必须放内部 RAM! 自检会走 zx_send_json → cJSON/TLS,
       这条链上任何 flash 操作(cache 关闭)都会让 PSRAM 栈不可访问,
       从而踩坏内存 —— 实测表现为别处任务报 stdout 锁断言 / 栈金丝雀被破坏。 */
    if (xTaskCreatePinnedToCore(selftest_task, "selftest", 8192, NULL, 2, NULL, 1) != pdPASS) {
        ESP_LOGW(TAG, "自检: 创建任务失败");
    }
    vTaskDelete(NULL);
}

/* ========== 任务栈水位监视 ==========
 * usStackHighWaterMark = 该任务历史最小剩余栈(单位:字, 1 字 = 4 字节)。
 * 低于约 200 字就非常危险 —— 需要调大对应任务的栈。
 * 这是定位「栈溢出崩溃」最直接的手段。 */
static void stack_mon_task(void *arg)
{
    static TaskStatus_t st[28];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        UBaseType_t n = uxTaskGetSystemState(st, 28, NULL);
        ESP_LOGI(TAG, "---- 任务栈剩余(字) 低于200危险 ----");
        for (UBaseType_t i = 0; i < n; i++) {
            if (st[i].usStackHighWaterMark < 512) {
                ESP_LOGW(TAG, "  %-12s free=%5u  <=== 偏低",
                         st[i].pcTaskName, (unsigned)st[i].usStackHighWaterMark);
            } else {
                ESP_LOGI(TAG, "  %-12s free=%5u",
                         st[i].pcTaskName, (unsigned)st[i].usStackHighWaterMark);
            }
        }
    }
}

/* ========== 表情渲染任务 ========== */
static void face_task(void *arg)
{
    uint32_t last_tick = esp_timer_get_time() / 1000;
    while (1) {
        uint32_t now = esp_timer_get_time() / 1000;
        uint32_t dt = now - last_tick;
        last_tick = now;
        if (dt > 100) dt = 33;  /* 防止异常大跳 */

        if (s_provisioning) {
            /* 配网阶段: 只刷配网页 */
            wifi_screen_tick(now);
            if (s_prov_ok_ms && (now - s_prov_ok_ms) >= WIFI_OK_HOLD_MS) {
                s_prov_ok_ms   = 0;
                s_provisioning = false;
                face_set_auto_cycle(true);
                face_set_emotion(FACE_HELLO);
                face_force_full();      /* 整屏覆盖配网页, 不留残影 */
                ESP_LOGI(TAG, "配网完成 → 进入表情系统, 说「你好小智」即可对话");
            }
        } else {
            face_update(dt);

            /* 已配网但一直连不上(密码错/路由不在): 超时后退回配网页, 否则用户永远
               回不到配网流程。路由恢复后会自动重连并自动切回表情, 不用重启。 */
            if (s_sta_connected) {
                s_wifi_fail_ms = 0;
            } else {
                if (s_wifi_fail_ms == 0) s_wifi_fail_ms = now;
                if (now - s_wifi_fail_ms >= WIFI_CONNECT_TIMEOUT_MS) {
                    s_wifi_fail_ms = 0;
                    s_prov_ok_ms   = 0;
                    s_provisioning = true;
                    face_set_auto_cycle(false);
                    wifi_screen_failed(AP_SSID, AP_PASS);
                    ESP_LOGW(TAG, "连不上 WiFi「%s」→ 退回配网模式; 手机连 %s 打开 192.168.4.1 重新填",
                             s_target_ssid, AP_SSID);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(33));  /* ~30fps */
    }
}

/* ========== WS2812 灯环任务 ========== */
/* 灯环已改由 light.c 的灯效引擎统一驱动(40Hz, 支持呼吸/渐变/彩虹/爆闪),
   原来这个每 50ms 手动刷彩虹的 led_task 会和灯效引擎抢同一个 RMT 通道,
   导致画面互相打断 —— 所以删掉, 不要再建。 */

/* ========== 红外跟手: 连续随动状态 ========== */
static float    s_track_offset = 0.0f;   /* 当前头部偏离中位角 (-MAX ~ +MAX) */
static uint32_t s_track_last_ms = 0;     /* 上一次跟手解算时间 */
static uint32_t s_hand_on_since = 0;     /* 手持续出现的起始时间 */
static uint32_t s_hand_off_since = 0;    /* 手持续消失的起始时间 */
static bool     s_ir_last_l = false;
static bool     s_ir_last_r = false;

/* 浮点取整 */
static int track_roundf(float v)
{
    return (int)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

/* 双红外 → 连续头部偏角. 每 SENSOR_TASK_PERIOD_MS 调用一次 */
static void ir_track_update(uint32_t now)
{
    bool left  = ir_obstacle_left_detected();
    bool right = ir_obstacle_right_detected();
    bool any   = left || right;

    /* 只在状态翻转时打印, 避免周期性日志占用串口、拖慢实时循环 */
    if (left != s_ir_last_l || right != s_ir_last_r) {
        s_ir_last_l = left;
        s_ir_last_r = right;
        ESP_LOGI(TAG, "IR L=%d R=%d (raw L=%d R=%d) tracking=%d",
                 left, right, ir_obstacle_left_raw(), ir_obstacle_right_raw(),
                 s_state.tracking);
    }

    /* --- 进入追踪 --- */
    if (any && !s_state.tracking) {
        if (s_hand_on_since == 0) s_hand_on_since = now;
        if (now - s_hand_on_since >= TRACK_ENTER_MS) {
            s_state.tracking = true;
            s_hand_off_since = 0;
            s_track_last_ms  = now;
            /* 从当前实际角度接管, 避免进入瞬间头部跳一下 */
            int cur = servo_get_angle(SERVO_PAN);
            if (cur < TRACK_PAN_CENTER - TRACK_OFFSET_MAX) cur = TRACK_PAN_CENTER - TRACK_OFFSET_MAX;
            if (cur > TRACK_PAN_CENTER + TRACK_OFFSET_MAX) cur = TRACK_PAN_CENTER + TRACK_OFFSET_MAX;
            s_track_offset = (float)(cur - TRACK_PAN_CENTER);
            ESP_LOGI(TAG, "hand IN → tracking start (pan=%d)", cur);
            face_set_auto_cycle(false);
            face_set_emotion(FACE_SURPRISED);
            /* 非阻塞: 抬头交后台伺服任务执行 */
            servo_set_target(SERVO_TILT, TRACK_TILT_LOOK_UP, 200);
        }
    } else if (!any) {
        s_hand_on_since = 0;
    }

    if (!s_state.tracking) return;

    /* --- 期望偏角: 侧单眼看到手=偏向该侧; 双眼都看到=回正; 短暂丢失=保持 --- */
    float desired;
    if (left && !right)          desired = -(float)TRACK_OFFSET_MAX;
    else if (right && !left)     desired =  (float)TRACK_OFFSET_MAX;
    else if (left && right)      desired = 0.0f;
    else                         desired = s_track_offset;  /* 短暂丢失保持当前 */

    /* --- 一阶平滑逼近 + 限速, 与轮询周期无关 --- */
    float dt = (float)(now - s_track_last_ms);
    s_track_last_ms = now;
    if (dt < 5.0f)   dt = 5.0f;
    if (dt > 100.0f) dt = 100.0f;

    float alpha = dt / ((float)TRACK_TAU_MS + dt);   /* 0~1, 时间常数≈TRACK_TAU_MS */
    float vel   = (desired - s_track_offset) * alpha;
    float vmax  = (float)TRACK_VEL_MAX_DPS * dt / 1000.0f;
    if (vel >  vmax) vel =  vmax;
    if (vel < -vmax) vel = -vmax;
    s_track_offset += vel;

    if (s_track_offset < -(float)TRACK_OFFSET_MAX) s_track_offset = -(float)TRACK_OFFSET_MAX;
    if (s_track_offset >  (float)TRACK_OFFSET_MAX) s_track_offset =  (float)TRACK_OFFSET_MAX;

    int pan = TRACK_PAN_CENTER + track_roundf(s_track_offset);
    if (pan < TRACK_PAN_CENTER - TRACK_OFFSET_MAX) pan = TRACK_PAN_CENTER - TRACK_OFFSET_MAX;
    if (pan > TRACK_PAN_CENTER + TRACK_OFFSET_MAX) pan = TRACK_PAN_CENTER + TRACK_OFFSET_MAX;

    s_state.pan_target = pan;
    servo_set_target(SERVO_PAN, pan, TRACK_VEL_MAX_DPS);   /* 非阻塞, 不卡本循环 */

    /* --- 手消失一段时间 → 退出并回正 --- */
    if (!any) {
        if (s_hand_off_since == 0) s_hand_off_since = now;
        if (now - s_hand_off_since >= TRACK_EXIT_MS) {
            ESP_LOGI(TAG, "hand OUT → release (pan=%d)", servo_get_angle(SERVO_PAN));
            s_state.tracking = false;
            s_hand_off_since  = 0;
            s_track_offset    = 0.0f;
            s_state.pan_target = TRACK_PAN_CENTER;
            face_set_emotion(FACE_NORMAL);
            face_set_auto_cycle(s_state.auto_cycle);
            servo_set_target(SERVO_PAN,  TRACK_PAN_CENTER, TRACK_RETURN_DPS);
            servo_set_target(SERVO_TILT, 90,               TRACK_RETURN_DPS);
        }
    } else {
        s_hand_off_since = 0;
    }
}

/* ========== DHT11 温湿度任务 (独立任务) ==========
 * DHT11 单次读取会有 ~25ms 的忙等阻塞, 从跟手循环里拆出来,
 * 避免每 2 秒一次的 25ms 卡顿打断跟手轨迹。 */
static void env_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (dht11_read(&s_state.env)) {
            s_state.env_valid = true;
            s_state.last_env_read = esp_timer_get_time() / 1000;
            ESP_LOGD(TAG, "temp=%.1fC hum=%.1f%%", s_state.env.temperature, s_state.env.humidity);
        }
    }
}

/* ========== 传感器监控任务 ========== */
/* 头部触摸手势参数 */
#define TOUCH_TAP_MAX_MS     350     /* 短于这个算"点一下" */
#define TOUCH_LONG_MS        800     /* 超过这个算长按 */
#define TOUCH_DOUBLE_GAP_MS  380     /* 两次点击的最大间隔(连击窗口) */
#define PURR_HOLD_MS         2500    /* 撸猫表情保持时长 */
#define TOUCH_DEBOUNCE_MS    60      /* 电平稳定多久才算真的按下/松开(抗干扰) */
#define TOUCH_BOOT_IGNORE_MS 5000    /* 开机这段时间内不认手势(上电抖动会把灯打开) */
#define TOUCH_GESTURE_GAP_MS 350     /* 一次手势之后的冷却: 防连发, 又不能太大
                                        否则会觉得"点了没反应"(之前 700ms 太迟钝) */

/* 撸猫反馈: 害羞表情 + 人格引擎记一笔(心情/亲密度+, 无聊清零) */
static void purr(void)
{
    face_set_emotion_hold(FACE_SHY, PURR_HOLD_MS);
    persona_event_touch();
}

static void sensor_task(void *arg)
{
    bool last_head_touch = false;
    bool long_fired  = false;        /* 本次按压是否已触发长按 */
    /* 连击计数: 支持 1/2/3 连击。
       连击窗口是"距最后一次点击"计算的, 所以三连击也能正确等到全部点完:
       点① 点② 点③ 之后静默 380ms → 按 3 连击生效。 */
    int      click_cnt    = 0;
    uint32_t last_click_ms = 0;
    uint32_t head_touch_start = 0;
    /* 防抖状态 */
    bool     db_level = false;       /* 稳定后的电平 */
    bool     db_last_raw = false;    /* 上一次读到的原始电平 */
    uint32_t db_change_ms = 0;       /* 原始电平开始变化的时刻 */
    uint32_t last_gesture_ms = 0;    /* 上次手势触发时刻(冷却用) */
    TickType_t last_wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "sensor task started @%dms", SENSOR_TASK_PERIOD_MS);

    while (1) {
        /* 固定周期轮询, 不受循环体耗时影响 */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SENSOR_TASK_PERIOD_MS));
        uint32_t now = esp_timer_get_time() / 1000;

        /* --- 头部触摸手势 ---
         *   单击 (<=350ms)      → 撸猫
         *   长按 (>=800ms)      → 撸猫
         *   双击 (两次点间隔<380ms) → 切换灯效
         * 因为"单击"和"双击"要区分, 单击必须等一个双击窗口才能确定,
         * 所以松手后最多延迟 380ms 才生效 —— 这是手势识别的固有权衡。 */
        /* ★ 防抖: 触摸脚被 WS2812 的 RMT 信号 / 舵机 PWM / WiFi 突发干扰时会抖。
           不防抖的话, 一次真实触摸会被判成好几次手势 —— 实测日志里 6 秒刷了
           6 次(双击/单击混着来), 结果就是灯效乱切、表情乱跳。
           这里要求电平连续稳定 TOUCH_DEBOUNCE_MS 才算真的变化。 */
        bool raw = touch_sensor_head_pressed();
        if (raw != db_last_raw) { db_last_raw = raw; db_change_ms = now; }
        bool head_touched = db_level;
        if (raw != db_level && (now - db_change_ms) >= TOUCH_DEBOUNCE_MS) {
            db_level = raw;                       /* 稳定了, 确认这一次变化 */
            head_touched = raw;
        } else {
            head_touched = db_level;
        }

        /* ★ 只在【播报中】忽略头部触摸, 不要在【聆听中】也忽略。
           原因: 播报时功放电流大、喇叭线会耦合出干扰, 触摸脚会被打成假触发
           (表现为"正说着话突然撸猫/切灯效"), 所以播报时要屏蔽。
           但连续对话时设备几乎一直处于"聆听"状态 ——
           若在这里用 is_busy()(聆听+播报都算), 触摸就会永久失效:
           双击不切灯、单击不撸猫, 用户会以为触摸坏了。 */
        bool ai_busy = ai_client_is_speaking();

        /* 手势冷却: 一次手势之后短暂屏蔽, 免得一次触摸连出好几个动作 */
        bool gesture_locked = ai_busy ||
                             ((now - last_gesture_ms) < TOUCH_GESTURE_GAP_MS);

        if (head_touched && !last_head_touch && !gesture_locked) {
            head_touch_start = now;
            long_fired = false;
        }
        /* 长按: 按住的当下就撸猫, 不用等松手 */
        if (head_touched && !long_fired &&
            (now - head_touch_start) >= TOUCH_LONG_MS) {
            long_fired  = true;
            click_cnt   = 0;                  /* 长按吃掉已累计的点击 */
            ESP_LOGI(TAG, "头部触摸: 长按 %ums → 撸猫", (unsigned)(now - head_touch_start));
            last_gesture_ms = now;
            purr();
        }
        if (!head_touched && last_head_touch) {
            uint32_t dur = now - head_touch_start;
            if (!long_fired && dur <= TOUCH_TAP_MAX_MS) {
                click_cnt++;
                last_click_ms = now;
                ESP_LOGI(TAG, "头部触摸: 第 %d 下 (%ums)", click_cnt, (unsigned)dur);
            } else if (!long_fired && dur > TOUCH_TAP_MAX_MS) {
                /* 中等等待(350~800ms): 也当撸猫处理 */
                click_cnt = 0;
                ESP_LOGI(TAG, "头部触摸: 按住 %ums → 撸猫", (unsigned)dur);
                last_gesture_ms = now;
                purr();
            }
        }
        last_head_touch = head_touched;

        /* ★ 连击判定超时: 距【最后一次】点击超过窗口还没等到下一击 →
           按累计的击数生效。因为窗口是从最后一次点击算起的, 三连击也能
           正确等到全部点完 (点①②③ 之后静默 380ms → 按 3 连击生效)。
             1 下 → 撸猫
             2 下 → 切换灯效
             3 下 → 音乐 暂停/继续 */
        if (click_cnt > 0 && (now - last_click_ms) > TOUCH_DOUBLE_GAP_MS) {
            int c = click_cnt;
            click_cnt = 0;
            last_gesture_ms = now;
            if (c == 1) {
                ESP_LOGI(TAG, "头部触摸: 单击 → 撸猫");
                purr();
            } else if (c == 2) {
                ESP_LOGI(TAG, "头部触摸: 双击 → 切换灯效");
                light_next_effect();
                face_set_emotion_hold(FACE_EXCITED, 900);
            } else {
                if (music_is_playing()) {
                    ESP_LOGI(TAG, "头部触摸: %d 连击 → 暂停音乐", c);
                    music_pause();
                    face_set_emotion_hold(FACE_HAPPY, 1200);
                } else if (music_is_active()) {
                    ESP_LOGI(TAG, "头部触摸: %d 连击 → 继续播放音乐", c);
                    music_resume();
                    face_set_emotion_hold(FACE_EXCITED, 1200);
                } else {
                    ESP_LOGW(TAG, "头部触摸: %d 连击 → 当前没有在放音乐", c);
                    face_set_emotion_hold(FACE_NORMAL, 900);
                }
            }
        }

        /* --- 双红外: 手位置连续追踪 --- */
        ir_obstacle_update(now);
        ir_track_update(now);
    }
}

/* ========== BOOT 键检测 (主循环) ========== */
static void boot_button_task(void *arg)
{
    int current_emotion = 0;
    bool last_level = true;  /* BOOT 默认高电平 */

    while (1) {
        bool level = (gpio_get_level(BOOT_BUTTON_GPIO) == 1);

        /* 下降沿: 按下 */
        if (!level && last_level) {
            s_state.boot_press_time = esp_timer_get_time() / 1000;
            s_state.boot_pressed = true;
        }
        /* 上升沿: 松开 */
        if (level && last_level == false && s_state.boot_pressed) {
            uint32_t duration = esp_timer_get_time() / 1000 - s_state.boot_press_time;
            s_state.boot_pressed = false;

            if (duration < 50) {
                /* 抖动, 忽略 */
            } else if (duration < 1000) {
                /* 短按: 切换表情 */
                current_emotion = (current_emotion + 1) % FACE_COUNT;
                face_set_emotion((face_emotion_t)current_emotion);
                ESP_LOGI(TAG, "BOOT short press → emotion=%s",
                         face_emotion_name((face_emotion_t)current_emotion));
                /* 表情联动舵机 */
                switch (current_emotion) {
                    case FACE_HAPPY:
                        servo_nod_head();
                        break;
                    case FACE_ANGRY:
                        servo_smooth_to(SERVO_TILT, 115, 200);
                        break;
                    case FACE_SAD:
                        servo_smooth_to(SERVO_TILT, 110, 300);
                        break;
                    case FACE_SURPRISED:
                        servo_smooth_to(SERVO_TILT, 70, 200);
                        break;
                    case FACE_SLEEPY:
                        servo_smooth_to(SERVO_TILT, 120, 400);
                        break;
                    default:
                        servo_smooth_to(SERVO_TILT, 90, 200);
                        servo_smooth_to(SERVO_PAN, 90, 200);
                        break;
                }
            } else {
                /* 长按: 切换到下一个灯效 (长臂台灯舵机已拆除) */
                light_next_effect();
                ESP_LOGI(TAG, "BOOT 长按 → 灯效 %s",
                         light_effect_cn(light_get_effect()));
            }
        }
        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ========== WiFi AP + HTTP 服务器 ========== */
static httpd_handle_t s_httpd = NULL;

/* 屏幕方向: 位0 = 左右翻转, 位1 = 上下翻转 (掉电保存到 NVS) */
static uint8_t s_flip = 0;

/* 根路径: 没配过网就给"配网专用页"(和小智官方一致的样式), 配好之后给控制台 */
static esp_err_t http_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const char *page = s_provisioning ? PROV_PAGE : WEB_PAGE;
    httpd_resp_send(req, page, strlen(page));
    return ESP_OK;
}

/* /prov 强制显示配网页(想重新配网时用它) */
static esp_err_t http_prov(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, PROV_PAGE, strlen(PROV_PAGE));
    return ESP_OK;
}

static esp_err_t http_status(httpd_req_t *req)
{
    /* static: 网页那边要一次性拿全(灯光/音乐/音量都塞进来了), 缓冲区到 1KB,
       放栈上会挤压 httpd 任务(默认 4KB)。httpd 的请求处理是串行的, 静态安全。 */
    static char buf[1024];
    uint8_t lr = 0, lg = 0, lb = 0;
    light_get_color(&lr, &lg, &lb);
    int len = snprintf(buf, sizeof(buf),
        "{\"emotion\":%d,\"temp\":%.1f,\"hum\":%.1f,\"env_valid\":%s,"
        "\"touch_head\":%s,\"ir_left\":%s,\"ir_right\":%s,"
        "\"tracking\":%s,\"pan\":%d,\"pan_target\":%d,\"tilt\":%d,\"auto\":%s,"
        "\"flip_x\":%d,\"flip_y\":%d,\"test\":%s,\"sta\":%s,\"ip\":\"%s\","
        "\"ai\":\"%s\",\"act\":\"%s\",\"tx\":%u,\"rx\":%u,\"mic\":%.0f,\"vad\":%s,\"wake\":\"%s\","
        /* --- 灯光 --- */
        "\"light\":%d,\"light_br\":%d,\"lr\":%d,\"lg\":%d,\"lb\":%d,"
        /* --- 音量 / 提示音 --- */
        "\"vol\":%d,\"wbeep\":%s,"
        /* --- 音乐 --- */
        "\"mus_on\":%s,\"mus_playing\":%s,\"mus\":\"%s\"}",
        (int)face_get_emotion(),
        s_state.env.temperature, s_state.env.humidity,
        s_state.env_valid ? "true" : "false",
        touch_sensor_head_pressed() ? "true" : "false",
        ir_obstacle_left_detected() ? "true" : "false",
        ir_obstacle_right_detected() ? "true" : "false",
        s_state.tracking ? "true" : "false",
        servo_get_angle(SERVO_PAN),
        s_state.pan_target,
        servo_get_angle(SERVO_TILT),
        s_state.auto_cycle ? "true" : "false",
        s_flip & 1, (s_flip >> 1) & 1,
        face_get_test() ? "true" : "false",
        s_sta_connected ? "true" : "false", s_sta_ip,
        ai_client_state_str(), ai_client_activation_code(),
        (unsigned)ai_client_tx_frames(), (unsigned)ai_client_rx_frames(),
        (double)ai_client_mic_rms(), ai_client_get_vad() ? "true" : "false",
        ai_client_wake_name(),
        (int)light_get_effect(), light_get_brightness(), lr, lg, lb,
        speaker_get_volume(), ai_client_get_wake_beep() ? "true" : "false",
        music_is_active() ? "true" : "false",
        music_is_playing() ? "true" : "false",
        music_status_str());

    /* 歌名里若带引号会把 JSON 弄坏(整页刷新就全挂), 这里统一换成单引号。
       buf 已成形, 只需把 "mus" 那段里的引号替换掉 —— 结构引号不会被误伤,
       因为它们后面紧跟的是字段名或逗号, 而歌名里的引号两边是中文/字母。 */
    {
        char *m = strstr(buf, "\"mus\":\"");
        if (m) {
            m += 7;                                  /* 指向值内容 */
            char *e = strchr(m, '"');
            for (char *p = m; p && *p && (!e || p < e); p++)
                if (*p == '"') *p = '\'';
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t http_emotion(httpd_req_t *req)
{
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(q, "e", val, sizeof(val)) == ESP_OK) {
            int e = atoi(val);
            if (e >= 0 && e < FACE_COUNT) {
                face_set_emotion((face_emotion_t)e);
                s_state.auto_cycle = false;
                face_set_auto_cycle(false);
            }
        }
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t http_servo(httpd_req_t *req)
{
    char q[128];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(q, "pan", val, sizeof(val)) == ESP_OK)
            servo_smooth_to(SERVO_PAN, atoi(val), 200);
        if (httpd_query_key_value(q, "tilt", val, sizeof(val)) == ESP_OK)
            servo_smooth_to(SERVO_TILT, atoi(val), 200);
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t http_auto(httpd_req_t *req)
{
    char q[32];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char val[4];
        if (httpd_query_key_value(q, "enable", val, sizeof(val)) == ESP_OK) {
            s_state.auto_cycle = (atoi(val) != 0);
            face_set_auto_cycle(s_state.auto_cycle);
        }
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* ========== 屏幕方向 (掉电保存到 NVS) ==========
   位0 = 左右翻转, 位1 = 上下翻转; 默认值取 config.h 里的 CONFIG_OLED_FLIP_* */
static void flip_apply(uint8_t v, bool save)
{
    s_flip = (uint8_t)(v & 0x03);
    ssd1305_set_flip((s_flip & 1) != 0, (s_flip & 2) != 0);
    if (save) {
        nvs_handle_t h;
        if (nvs_open("face", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "flip", s_flip);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

static uint8_t flip_load(void)
{
    uint8_t v = (uint8_t)((CONFIG_OLED_FLIP_X ? 1 : 0) | (CONFIG_OLED_FLIP_Y ? 2 : 0));
    nvs_handle_t h;
    if (nvs_open("face", NVS_READONLY, &h) == ESP_OK) {
        uint8_t tmp = 0;
        if (nvs_get_u8(h, "flip", &tmp) == ESP_OK) v = tmp;
        nvs_close(h);
    }
    return (uint8_t)(v & 0x03);
}

/* /flip?x=0&y=1 */
static esp_err_t http_flip(httpd_req_t *req)
{
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char vx[4] = "0", vy[4] = "0";
        httpd_query_key_value(q, "x", vx, sizeof(vx));
        httpd_query_key_value(q, "y", vy, sizeof(vy));
        flip_apply((uint8_t)((atoi(vx) ? 1 : 0) | (atoi(vy) ? 2 : 0)), true);
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* /beep?f=880&ms=300  播放测试音, 用来验证功放链路 (不用重启) */
static esp_err_t http_beep(httpd_req_t *req)
{
    char q[48];
    char v[8];
    int f = 880, ms = 300;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "f", v, sizeof(v)) == ESP_OK)  f  = atoi(v);
        if (httpd_query_key_value(q, "ms", v, sizeof(v)) == ESP_OK) ms = atoi(v);
    }
    if (f < 100 || f > 8000) f = 880;
    if (ms < 20 || ms > 3000) ms = 300;
    ESP_LOGI(TAG, "测试音: %d Hz / %d ms", f, ms);
    speaker_beep((uint32_t)f, (uint32_t)ms);
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* /wakebeep?on=0|1  唤醒应答音开关 (排查喇叭杂音用) */
static esp_err_t http_wakebeep(httpd_req_t *req)
{
    char q[32], v[4] = "0";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "on", v, sizeof(v));
    ai_client_set_wake_beep(atoi(v) != 0);
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* /vol?x=0..100  提示音音量, 设完立刻响一声听效果 */
static esp_err_t http_vol(httpd_req_t *req)
{
    char q[32], v[8];
    int x = speaker_get_volume();
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "x", v, sizeof(v)) == ESP_OK) {
        x = atoi(v);
    }
    speaker_set_volume(x);
    speaker_beep(880, 200);
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* URL 解码(定义在下面 http_music 之前, 这里先声明) */
static void http_url_decode(char *s);

/* ============ 灯光控制 (网页与 AI 同一套能力) ============
   /light?effect=0..5      切换灯效 (0关灯 1单色 2呼吸 3渐变 4彩虹 5警车)
   /light?next=1           下一个灯效
   /light?br=0..100        亮度
   /light?r=255&g=0&b=0    主色(会自动切到单色常亮)                        */
static esp_err_t http_light(httpd_req_t *req)
{
    char q[96], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "effect", v, sizeof(v)) == ESP_OK) {
            int e = atoi(v);
            if (e < 0 || e >= LIGHT_COUNT) e = LIGHT_OFF;
            light_set_effect((light_effect_t)e);
        } else if (httpd_query_key_value(q, "next", v, sizeof(v)) == ESP_OK) {
            light_next_effect();
        } else if (httpd_query_key_value(q, "br", v, sizeof(v)) == ESP_OK) {
            light_set_brightness(atoi(v));
        } else if (httpd_query_key_value(q, "r", v, sizeof(v)) == ESP_OK) {
            int r = atoi(v), g = 0, b = 0;
            if (httpd_query_key_value(q, "g", v, sizeof(v)) == ESP_OK) g = atoi(v);
            if (httpd_query_key_value(q, "b", v, sizeof(v)) == ESP_OK) b = atoi(v);
            light_set_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* ============ 万能入口: 网页直接调用任意 MCP 工具 ============
 *
 *   /mcp?name=self.servo.nod
 *   /mcp?name=self.light.set_brightness&args=%7B%22brightness%22%3A80%7D
 *
 * 走的是【和 AI 完全相同】的那条代码路径(mcp_handle_payload), 所以
 * AI 能做的每一件事, 网页都能做到 —— 不用为每个功能单独写一个接口,
 * 以后新增 MCP 工具网页自动就有。这就是"把所有功能同步到网页"的最省事做法。
 *
 * 注意 args 里的 JSON 必须做 URL 编码(否则 & 会被当成参数分隔符),
 * 网页端用 encodeURIComponent 处理。                                      */
static esp_err_t http_mcp(httpd_req_t *req)
{
    static char q[512], name[96], args[256], payload[512];
    name[0] = '\0';
    args[0] = '\0';
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "name", name, sizeof(name));
        httpd_query_key_value(q, "args", args, sizeof(args));
    }
    http_url_decode(name);
    http_url_decode(args);
    if (!name[0]) {
        httpd_resp_send(req, "{\"ok\":false,\"err\":\"missing name\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    snprintf(payload, sizeof(payload),
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
             name, args[0] ? args : "{}");

    char *resp = mcp_handle_payload(payload, NULL);
    if (resp) {
        ESP_LOGI(TAG, "网页调用 MCP: %s", name);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        free(resp);
    } else {
        httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

/* /reprov  清除 WiFi 配置并立即回到配网模式
 * 热点是常开的, 所以手机随时都能调这个接口重新配网, 不用等 30 秒超时 */
static esp_err_t http_reprov(httpd_req_t *req)
{
    cfg_set_str("ssid", "");
    cfg_set_str("pass", "");
    s_target_ssid[0] = '\0';
    s_provisioned  = false;
    s_prov_ok_ms   = 0;
    s_wifi_fail_ms = 0;
    s_provisioning = true;
    face_set_auto_cycle(false);
    wifi_screen_provision(AP_SSID, AP_PASS);
    ESP_LOGW(TAG, "手动重新配网: 已清除 WiFi 配置, 屏幕回到配网页");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* /vad?on=1  免触摸对话开关 (本地语音活动检测) */
static esp_err_t http_vad(httpd_req_t *req)
{
    char q[32];
    char v[4] = "1";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "on", v, sizeof(v));
    ai_client_set_vad(atoi(v) != 0);
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* /music?q=歌名        播放网易云音乐   (q 可省略 -> 返回当前状态)
   /music?serve=http://ip:3000   改代理地址
   例: http://192.168.1.7/music?q=%E6%99%B4%E5%A4%A9                    */
static void http_url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char h[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            p += 2;
        } else if (*p == '+') {
            *o++ = ' ';
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

static esp_err_t http_music(httpd_req_t *req)
{
    char q[192];
    char v[128];
    /* ★ 只搜索不播放: 网页拿结果列表给用户点选 (见 music_search_only) */
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "search", v, sizeof(v)) == ESP_OK) {
        http_url_decode(v);
        bool ok = music_search_only(v);
        ESP_LOGI(TAG, "HTTP 请求搜索: %s", v);
        httpd_resp_send(req, ok ? "OK" : "busy", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    /* ★ 导出当前曲目列表(搜索结果 或 已加载的歌单)给网页。
       静态缓冲: 30 首 × ~180 字节 ≈ 5.4KB, 放栈上会撑爆 httpd 的 4KB 栈。 */
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "list", v, sizeof(v)) == ESP_OK) {
        static char js[6144];
        int n = music_results_json(js, sizeof(js));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, js, n);
        return ESP_OK;
    }
    /* ★ 我的歌单: 【只读】当前缓存的歌单列表。
       触发拉取用 plsreq(见下) —— 分开写是因为网页每 1.5 秒轮询一次,
       如果这里每次都重新拉, 就会把代理打爆。 */
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "pls", v, sizeof(v)) == ESP_OK) {
        static char js[6144];
        int n = music_playlists_json(js, sizeof(js));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, js, n);
        return ESP_OK;
    }
    /* ★ 我的歌单: 触发一次拉取(异步, 结果由 pls 读) */
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "plsreq", v, sizeof(v)) == ESP_OK) {
        music_playlists_request();
        httpd_resp_send(req, "OK", 2);
        return ESP_OK;
    }
    /* ★ 我的歌单: 把某个歌单的歌装进曲目列表(异步) */
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "plsload", v, sizeof(v)) == ESP_OK) {
        http_url_decode(v);
        music_playlist_songs_request(v);
        ESP_LOGI(TAG, "HTTP 加载歌单: %s", v);
        httpd_resp_send(req, "OK", 2);
        return ESP_OK;
    }
    /* ★ 点播歌单里的某一首(按网易云 id) */
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "play", v, sizeof(v)) == ESP_OK) {
        http_url_decode(v);
        bool ok = music_play_id(v);
        ESP_LOGI(TAG, "HTTP 点播 id=%s -> %s", v, ok ? "OK" : "失败");
        httpd_resp_send(req, ok ? "OK" : "fail", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "q", v, sizeof(v)) == ESP_OK) {
        http_url_decode(v);
        music_play(v);
        ESP_LOGI(TAG, "HTTP 请求播放: %s", v);
        httpd_resp_send(req, "OK", 2);
        return ESP_OK;
    }
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "serve", v, sizeof(v)) == ESP_OK) {
        http_url_decode(v);
        music_set_base(v);
        httpd_resp_send(req, "OK", 2);
        return ESP_OK;
    }
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "ctl", v, sizeof(v)) == ESP_OK) {
        if (!strcmp(v, "pause"))  music_pause();
        if (!strcmp(v, "resume")) music_resume();
        if (!strcmp(v, "stop"))   music_stop();
        if (!strcmp(v, "next"))   music_next();
        httpd_resp_send(req, "OK", 2);
        return ESP_OK;
    }
    const char *st = music_status_str();
    httpd_resp_send(req, st, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* /ttstest?sec=2  下行音频链路自检: 走真实解码/播放通路放一段 440Hz 提示音 */
static esp_err_t http_ttstest(httpd_req_t *req)
{
    char q[32];
    char v[8] = "2";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "sec", v, sizeof(v));
    int sec = atoi(v);
    if (sec < 1) sec = 1;
    if (sec > 10) sec = 10;
    ai_client_selftest_tts(sec);
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* /test?on=1  方向测试图: 三角朝上 + 方块在左上角 = 方向正确 */
static esp_err_t http_test(httpd_req_t *req)
{
    char q[32];
    char v[4] = "0";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK)
        httpd_query_key_value(q, "on", v, sizeof(v));
    face_set_test(atoi(v) != 0);
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* ---------- NVS 配置读写 (WiFi 账号等) ---------- */
static size_t cfg_get_str(const char *key, char *out, size_t max)
{
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return 0;
    size_t len = max;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = '\0';
    nvs_close(h);
    return strlen(out);
}

static void cfg_set_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

/* 把 NVS 里的 WiFi 账号填进 STA 配置 (没配过就用 config.h 的占位值) */
static void wifi_fill_sta_cfg(wifi_config_t *sta)
{
    char ssid[33] = {0}, pass[65] = {0};
    if (cfg_get_str("ssid", ssid, sizeof(ssid)) == 0) {
        strncpy(ssid, WIFI_SSID, sizeof(ssid) - 1);
        strncpy(pass, WIFI_PASSWORD, sizeof(pass) - 1);
        ESP_LOGW(TAG, "WiFi 未配置, 用 config.h 占位值 (去 http://192.168.4.1 配)");
    } else {
        cfg_get_str("pass", pass, sizeof(pass));
    }
    memset(sta, 0, sizeof(*sta));
    strncpy((char *)sta->sta.ssid, ssid, sizeof(sta->sta.ssid) - 1);
    strncpy((char *)sta->sta.password, pass, sizeof(sta->sta.password) - 1);
    sta->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_LOGI(TAG, "STA 目标: %s", ssid);
}

/* 运行中换 WiFi: 断链 → 改配置 → 重连 (网页改完立即生效, 不用重启) */
static void wifi_reconnect_sta(void)
{
    wifi_config_t sta;
    wifi_fill_sta_cfg(&sta);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "web client connected");
    } else if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        strcpy(s_sta_ip, "-");
        ai_client_notify_online(false);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_sta_connected = true;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ai_client_notify_online(true);
        ESP_LOGI(TAG, "STA 已联网, IP=%s", s_sta_ip);

        /* 配网阶段连上了: 屏幕上显示 IP, 停留 2 秒后由 face_task 切进表情系统 */
        s_wifi_fail_ms = 0;
        if (s_provisioning) {
            s_provisioned = true;
            wifi_screen_ok(s_sta_ip);
            s_prov_ok_ms = esp_timer_get_time() / 1000;
        }
    }
}

/* NVS 必须在读 WiFi 配置之前初始化好 (坏了就擦掉重建) */
static void nvs_setup(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

/* ==================== 强制门户 (captive portal) ====================
 *
 * 目标: 手机连上设备热点「RobotFace」之后, 系统【自动弹出】控制页,
 *       不用手输 192.168.4.1。
 *
 * 为什么光有 302 兜底不够:
 *   手机连上热点后会先自己探测"这网能不能上网", 探测用的域名是
 *   connectivitycheck.gstatic.com(安卓) / captive.apple.com(iOS) 等。
 *   如果这些域名解析不了, 手机只会认定"这网没网", 压根不会去请求我们的
 *   HTTP, 也就永远不会弹控制页。
 *
 * 所以必须补上 DNS: 这里跑一个"来者不拒"的 DNS 服务器 ——
 * 任何域名都回答 192.168.4.1。手机于是把探测请求发到我们这里,
 * 我们(兜底路由)回 302 → 系统识别为"需要登录的网络" → 自动弹出控制页。
 *
 * 注意: 安卓的 generate_204 期望 204, 我们回 302 正好让它判定为门户;
 *       iOS 的 hotspot-detect.html 期望 "Success", 我们回 302 也会触发弹窗。 */
static void dns_hijack_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { ESP_LOGE(TAG, "DNS: socket 创建失败"); vTaskDelete(NULL); return; }

    struct sockaddr_in a = {0};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(53);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGE(TAG, "DNS: 绑定 53 端口失败");
        close(s);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS 劫持已启动: 任何域名都解析到 192.168.4.1 (强制门户)");

    static uint8_t q[512], r[600];
    while (1) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, q, sizeof(q), 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        /* 只处理标准查询(QR=0)且只有 1 个问题 */
        if ((q[2] & 0x80) || (((q[4] << 8) | q[5]) != 1)) continue;

        /* 跳过域名, 找到问题段的结尾 */
        int p = 12;
        while (p < n) {
            if (q[p] == 0) { p++; break; }
            if ((q[p] & 0xC0) == 0xC0) { p += 2; break; }
            p += q[p] + 1;
        }
        if (p + 4 > n) continue;
        int qend = p + 4;
        uint16_t qtype = (uint16_t)((q[qend - 4] << 8) | q[qend - 3]);

        memcpy(r, q, (size_t)qend);
        r[2] = 0x81; r[3] = 0x80;                 /* 标准响应, 期望递归 */
        r[8] = r[9] = r[10] = r[11] = 0;          /* NS/AR 计数清零 */
        int o = qend;

        if (qtype == 1) {                         /* A 查询 → 答 192.168.4.1 */
            r[6] = 0; r[7] = 1;                   /* ANCOUNT = 1 */
            r[o++] = 0xC0; r[o++] = 0x0C;         /* 名字指针指向问题里的域名 */
            r[o++] = 0x00; r[o++] = 0x01;         /* TYPE  A */
            r[o++] = 0x00; r[o++] = 0x01;         /* CLASS IN */
            r[o++] = 0x00; r[o++] = 0x00; r[o++] = 0x00; r[o++] = 0x3C;  /* TTL 60s */
            r[o++] = 0x00; r[o++] = 0x04;         /* RDLENGTH 4 */
            r[o++] = 192;  r[o++] = 168; r[o++] = 4; r[o++] = 1;
        } else {
            /* AAAA 等其他类型: 回一个"无应答"的空响应, 让客户端自己去试 A 记录。
               直接乱答反而会被当成格式错误而多等一轮超时。 */
            r[6] = 0; r[7] = 0;
        }
        sendto(s, r, (size_t)o, 0, (struct sockaddr *)&from, fl);
    }
}

/* AP 给手机连(网页控制台); STA 连路由(小智 AI 上网)
 * 未配过网时只开 AP: 不做无意义的 STA 重连, 免得日志刷屏 */
static void start_wifi(bool provisioned)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wcfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t ap_cfg = {0};
    strcpy((char*)ap_cfg.ap.ssid, AP_SSID);
    strcpy((char*)ap_cfg.ap.password, AP_PASS);
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    /* STA 参数必须在 start 之前配好: STA_START 事件会立刻发起连接,
       start 之后再 set_config 会报 "sta is connecting, cannot set config" */
    wifi_config_t sta_cfg;
    wifi_fill_sta_cfg(&sta_cfg);

    if (provisioned) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    } else {
        /* 配网模式: 只开热点, 不连路由 */
        esp_wifi_set_mode(WIFI_MODE_AP);
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    }
    /* ★★★ 强制门户: 让手机连上热点后【自动弹出】控制页 ★★★
     *
     * 三件事缺一不可:
     *   ① 把 AP 下发的 DNS 指向自己 —— 我们跑了个"来者不拒"的 DNS
     *      (dns_hijack_task), 任何域名都解析到 192.168.4.1。这样手机
     *      发往 connectivitycheck.gstatic.com / captive.apple.com 的
     *      联网探测请求才会落到我们身上。
     *   ② 打开 DHCP 的"下发 DNS"开关(该选项本身只是个布尔值)。
     *   ③ 下发 DHCP 选项 114(RFC 8910 强制门户 URI) —— iOS 14+ / 安卓 11+
     *      会直接拿这个地址弹页, 比只靠 HTTP 302 更干脆。
     *
     * 三者齐了之后: 手机连「RobotFace」→ 系统识别为"需要登录的 Wi-Fi"
     * → 自动弹出 / 控制页。不齐的话, 手机只会认为"这网没网"然后悄悄用流量。 */
    {
        static const char CAP_URI[] = "http://192.168.4.1/";   /* 必须常驻, API 只存指针 */
        esp_netif_dns_info_t dns = {0};
        dns.ip.type            = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = htonl(0xC0A80401);            /* 192.168.4.1 */
        esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);

        uint8_t offer_dns = 1;
        esp_err_t e1 = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                              ESP_NETIF_DOMAIN_NAME_SERVER,
                                              &offer_dns, sizeof(offer_dns));
        esp_err_t e2 = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                                              ESP_NETIF_CAPTIVEPORTAL_URI,
                                              (void *)CAP_URI, sizeof(CAP_URI));
        ESP_LOGI(TAG, "强制门户: DNS下发=%s, 门户URI(114)=%s",
                 e1 == ESP_OK ? "开" : esp_err_to_name(e1),
                 e2 == ESP_OK ? "开" : esp_err_to_name(e2));

        /* DNS 劫持任务(53 端口) —— 太耗栈反而没必要, 4096 足够 */
        xTaskCreatePinnedToCore(dns_hijack_task, "dns_hijack", 4096, NULL, 3, NULL, 0);
    }

    esp_wifi_start();   /* STA_START 事件里会自动 esp_wifi_connect() */

    /* ★★★ 关掉 WiFi 省电模式 —— 音频流/在线音乐的关键 ★★★
     *
     * ESP-IDF 默认是 WIFI_PS_MIN_MODEM(省电开启): 设备在两次 beacon 之间
     * 让射频休眠, 于是 AP 会把下行的数据包【先缓存起来, 等下一个 beacon
     * 才一起下发】。后果是每个下行包都带 100~300ms 的突发延迟 ——
     *   · 在线音乐: 读网络时反复停下来等数据(实测每轮 6ms, 就是卡顿残渣)
     *   · AI 对话: 服务端音频/文本的到达时刻忽早忽晚, 抖动缓冲被白白吃掉
     * 关掉之后 WiFi 射频常开, 包一到就交给协议栈, 延迟稳定。
     * 代价只是待机功耗略高, 对本项目(常供电)完全可接受。 */
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi 省电模式已关闭 (WIFI_PS_NONE) -> %s",
             ps == ESP_OK ? "成功" : esp_err_to_name(ps));

    ESP_LOGI(TAG, "WiFi AP: %s (密码 %s)  ->  http://192.168.4.1", AP_SSID, AP_PASS);
}

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *o++ = ' ';
            s++;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

/* /wifi?ssid=xxx&pass=yyy  保存到 NVS 并立即重连 */
static esp_err_t http_wifi(httpd_req_t *req)
{
    char q[256];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char ssid[40] = {0}, pass[70] = {0};
        if (httpd_query_key_value(q, "ssid", ssid, sizeof(ssid)) == ESP_OK) {
            httpd_query_key_value(q, "pass", pass, sizeof(pass));
            url_decode(ssid);
            url_decode(pass);
            if (ssid[0]) {
                cfg_set_str("ssid", ssid);
                cfg_set_str("pass", pass);
                strncpy(s_target_ssid, ssid, sizeof(s_target_ssid) - 1);
                s_wifi_fail_ms = 0;
                ESP_LOGI(TAG, "WiFi 配置已保存: %s", ssid);
                if (s_provisioning) {
                    /* 配网模式原本只开了 AP, 先切到 APSTA 才能连路由 */
                    wifi_screen_connecting();
                    esp_wifi_set_mode(WIFI_MODE_APSTA);
                    vTaskDelay(pdMS_TO_TICKS(200));   /* 等模式切换生效 */
                }
                wifi_reconnect_sta();
            }
        }
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/* 兜底: 所有没被上面注册的路径都 302 到控制页。
 *
 * ★ 这里同时也是"强制门户"的落点:
 *   手机连上热点后会去 GET connectivitycheck.gstatic.com/generate_204
 *   (或 captive.apple.com/hotspot-detect.html) 来判断这网能不能上网。
 *   DNS 劫持把这些域名解析到我们, 请求打到这, 我们回 302 → 系统识别为
 *   "需要登录的 Wi-Fi" → 自动弹出控制页。
 *
 * 从局域网(STA 侧, 比如 192.168.1.100)误输入路径时, 要跳回它自己的地址,
 * 不能一律往 192.168.4.1 甩 —— 那个地址在局域网里是打不开的。 */
static esp_err_t http_catchall(httpd_req_t *req)
{
    char host[80] = {0};
    char loc[112];
    bool probe = false;
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK && host[0]) {
        /* 联网探测用的域名 → 统一引到 AP 网关(标准门户做法) */
        static const char *probe_domains[] = { "gstatic", "apple", "msft", "connecttest",
                                               "qualcomm", "android", "google" };
        for (size_t i = 0; i < sizeof(probe_domains) / sizeof(probe_domains[0]); i++)
            if (strstr(host, probe_domains[i])) { probe = true; break; }
    }
    if (probe || !host[0]) snprintf(loc, sizeof(loc), "http://192.168.4.1/");
    else                    snprintf(loc, sizeof(loc), "http://%s/", host);

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, "<h1>302 Found</h1>", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;   /* 之前 16 个槽位不够, 最后几个注册会失败 */
    /* ★★★ 必须显式指定通配符匹配函数 ★★★
     *
     * httpd_config 默认 uri_match_fn = NULL, 此时 ESP-IDF 用的是
     * httpd_uri_match_simple() —— 那是【精确字符串比较】, 也就是说
     * 下面注册的那条星号兜底路由永远不可能匹配上任何路径!
     * 实测表现: 未知路径一律返回 httpd 自带的 404, 强制门户的
     * "302 跳到控制页" 那一半从来没生效过(手机因此不会弹页)。
     * (见 esp_http_server/src/httpd_uri.c 的 httpd_find_uri_handler) */
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &cfg) == ESP_OK) {
        httpd_uri_t r_root  = {.uri = "/",        .method = HTTP_GET, .handler = http_root};
        httpd_uri_t r_prov  = {.uri = "/prov",    .method = HTTP_GET, .handler = http_prov};
        httpd_uri_t r_stat  = {.uri = "/status",  .method = HTTP_GET, .handler = http_status};
        httpd_uri_t r_emo   = {.uri = "/emotion", .method = HTTP_GET, .handler = http_emotion};
        httpd_uri_t r_servo = {.uri = "/servo",   .method = HTTP_GET, .handler = http_servo};
        httpd_uri_t r_auto  = {.uri = "/auto",    .method = HTTP_GET, .handler = http_auto};
        httpd_uri_t r_flip  = {.uri = "/flip",    .method = HTTP_GET, .handler = http_flip};
        httpd_uri_t r_test  = {.uri = "/test",    .method = HTTP_GET, .handler = http_test};
        httpd_uri_t r_vad   = {.uri = "/vad",     .method = HTTP_GET, .handler = http_vad};
        httpd_uri_t r_ttst  = {.uri = "/ttstest", .method = HTTP_GET, .handler = http_ttstest};
        httpd_uri_t r_mus   = {.uri = "/music",   .method = HTTP_GET, .handler = http_music};
        httpd_uri_t r_reprov= {.uri = "/reprov",  .method = HTTP_GET, .handler = http_reprov};
        httpd_uri_t r_beep  = {.uri = "/beep",    .method = HTTP_GET, .handler = http_beep};
        httpd_uri_t r_vol   = {.uri = "/vol",     .method = HTTP_GET, .handler = http_vol};
        httpd_uri_t r_wb    = {.uri = "/wakebeep",.method = HTTP_GET, .handler = http_wakebeep};
        httpd_uri_t r_wifi  = {.uri = "/wifi",    .method = HTTP_GET, .handler = http_wifi};
        httpd_uri_t r_light = {.uri = "/light",   .method = HTTP_GET, .handler = http_light};
        httpd_uri_t r_mcp   = {.uri = "/mcp",     .method = HTTP_GET, .handler = http_mcp};
        httpd_register_uri_handler(s_httpd, &r_root);
        httpd_register_uri_handler(s_httpd, &r_prov);
        httpd_register_uri_handler(s_httpd, &r_stat);
        httpd_register_uri_handler(s_httpd, &r_emo);
        httpd_register_uri_handler(s_httpd, &r_servo);
        httpd_register_uri_handler(s_httpd, &r_auto);
        httpd_register_uri_handler(s_httpd, &r_flip);
        httpd_register_uri_handler(s_httpd, &r_test);
        httpd_register_uri_handler(s_httpd, &r_vad);
        httpd_register_uri_handler(s_httpd, &r_ttst);
        httpd_register_uri_handler(s_httpd, &r_mus);
        httpd_register_uri_handler(s_httpd, &r_reprov);
        httpd_register_uri_handler(s_httpd, &r_beep);
        httpd_register_uri_handler(s_httpd, &r_vol);
        httpd_register_uri_handler(s_httpd, &r_wb);
        httpd_register_uri_handler(s_httpd, &r_wifi);
        httpd_register_uri_handler(s_httpd, &r_light);
        httpd_register_uri_handler(s_httpd, &r_mcp);
        /* ★ 兜底必须【最后】注册。
           匹配是按注册顺序找第一个命中的, 而星号兜底会命中任何路径 ——
           注册早了就把上面所有具体路径全抢掉。 */
        httpd_uri_t r_catch = {.uri = "/*", .method = HTTP_GET, .handler = http_catchall};
        httpd_register_uri_handler(s_httpd, &r_catch);
        ESP_LOGI(TAG, "HTTP server started at http://192.168.4.1");
    }
}

#if 0  /* 旧的自建 HTTP 动作分发, 已被小智官方 WebSocket 协议取代, 保留备查 */
/* ========== 执行 AI 动作指令 ========== */
static void execute_ai_actions(ai_response_t *resp)
{
    for (int i = 0; i < resp->action_count; i++) {
        switch (resp->actions[i]) {
            case ACTION_SHAKE_HEAD:
                servo_shake_head();
                face_set_emotion(FACE_SAD);
                break;
            case ACTION_NOD_HEAD:
                servo_nod_head();
                face_set_emotion(FACE_HAPPY);
                break;
            case ACTION_LOOK_LEFT:
                servo_smooth_to(SERVO_PAN, 60, 300);
                break;
            case ACTION_LOOK_RIGHT:
                servo_smooth_to(SERVO_PAN, 120, 300);
                break;
            case ACTION_LOOK_UP:
                servo_smooth_to(SERVO_TILT, 70, 300);
                break;
            case ACTION_LOOK_DOWN:
                servo_smooth_to(SERVO_TILT, 110, 300);
                break;
            case ACTION_LAMP_ON:
                light_set_effect(LIGHT_MONO);
                break;
            case ACTION_LAMP_OFF:
                light_set_effect(LIGHT_OFF);
                break;
            case ACTION_HAPPY:
                face_set_emotion(FACE_HAPPY);
                break;
            case ACTION_SAD:
                face_set_emotion(FACE_SAD);
                break;
            case ACTION_ANGRY:
                face_set_emotion(FACE_ANGRY);
                break;
            case ACTION_SURPRISED:
                face_set_emotion(FACE_SURPRISED);
                break;
            case ACTION_SLEEPY:
                face_set_emotion(FACE_SLEEPY);
                break;
            case ACTION_SHY:
                face_set_emotion(FACE_SHY);
                break;
            default:
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ========== AI 对话任务 (可选, 需配置WiFi和API) ========== */
static void ai_task(void *arg)
{
    /* 等待 STA 联网 */
    int wait = 0;
    while (!ai_client_online() && wait < 30) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        wait++;
    }

    if (!ai_client_online()) {
        ESP_LOGW(TAG, "STA 未联网, AI 任务空转 (去 http://192.168.4.1 配 WiFi)");
        /* WiFi 未连接时此任务空转, 不影响其他功能 */
        while (1) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    ESP_LOGI(TAG, "AI client ready");

    /* AI 对话触发方式: 可通过触摸传感器长按、或语音唤醒
       当前为框架, 用户可根据需要添加触发逻辑 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* 示例: 检测到触摸超过3秒触发对话 (取消注释启用)
        if (touch_sensor_pressed()) {
            uint32_t t0 = esp_timer_get_time()/1000;
            while (touch_sensor_pressed() && (esp_timer_get_time()/1000 - t0) < 3000)
                vTaskDelay(pdMS_TO_TICKS(100));
            if (touch_sensor_pressed()) {
                // 录音 → 识别 → 对话 → TTS
                // 1. mic_start(); 录音...
                // 2. ai_client_speech_to_text(audio, samples, text, sizeof(text));
                // 3. ai_client_chat(text, temp, hum, &resp);
                // 4. execute_ai_actions(&resp);
                // 5. ai_client_text_to_speech(resp.text, audio, max);
                // 6. speaker_write(audio, samples, 5000);
            }
        }
        */
    }
}

#endif /* 旧 HTTP 版 AI 代码 */

/* ========== 主入口 ========== */
void app_main(void)
{
    ESP_LOGI(TAG, "=== Robot Companion v1.0 ===");
    ESP_LOGI(TAG, "ESP32-S3 N16R8 + SSD1305 + 3xSG90 + WS2812 + Sensors + I2S Audio + AI");

    memset(&s_state, 0, sizeof(s_state));
    s_state.pan_target = 90;
    s_state.tilt_target = 90;
    s_state.auto_cycle = true;

    /* --- NVS 先就绪, 再判断有没有配过网 --- */
    nvs_setup();
    char saved_ssid[33] = {0};
    s_provisioned  = (cfg_get_str("ssid", saved_ssid, sizeof(saved_ssid)) > 0);
    s_provisioning = !s_provisioned;
    if (s_provisioned) strncpy(s_target_ssid, saved_ssid, sizeof(s_target_ssid) - 1);

    /* --- 初始化 WiFi (配网模式只开 AP) + HTTP 服务器 --- */
    start_wifi(s_provisioned);
    start_http_server();

    /* --- 初始化 OLED --- */
    esp_err_t ret = ssd1305_init(CONFIG_I2C_PORT, CONFIG_I2C_SDA_GPIO,
                                  CONFIG_I2C_SCL_GPIO, CONFIG_I2C_FREQ_HZ);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OLED init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "OLED init OK (SDA=%d SCL=%d)", CONFIG_I2C_SDA_GPIO, CONFIG_I2C_SCL_GPIO);
        /* 面板方向: 优先用 NVS 里上次网页设置的, 没有才用 config.h 默认值 */
        flip_apply(flip_load(), false);
    }

    /* --- 初始化表情系统 / 或进入配网模式 --- */
    face_init();
    if (s_provisioning) {
        /* 屏幕让给配网页, 不自动播表情 */
        face_set_auto_cycle(false);
        wifi_screen_provision(AP_SSID, AP_PASS);
        ESP_LOGW(TAG, "==================== 未配网, 进入配网模式 ====================");
        ESP_LOGW(TAG, " 1) 手机搜索并连接热点: %s   (密码 %s)", AP_SSID, AP_PASS);
        ESP_LOGW(TAG, " 2) 浏览器打开: http://192.168.4.1  填入你家 WiFi 账号密码");
        ESP_LOGW(TAG, " 3) 连上后会显示 IP, 2 秒后自动进入表情系统");
        ESP_LOGW(TAG, "============================================================");
    } else {
        face_set_auto_cycle(true);  /* 自动播放表情 */
        face_set_emotion(FACE_NORMAL);
        ESP_LOGI(TAG, "已配网(%s) → 直接进入表情系统", saved_ssid);
    }
    ESP_LOGI(TAG, "face init: %d emotions", FACE_COUNT);

    /* --- 初始化舵机 --- */
    servo_init();
    servo_set_angle(SERVO_PAN, 90);
    servo_set_angle(SERVO_TILT, 90);
    /* 长臂台灯舵机已拆除, 不再初始化 SERVO_ARM */

    /* --- 灯环: 交给灯效引擎初始化(ws2812_init 在 light_init 里) --- */
    /* (灯效任务在下面 xTaskCreate 区一起创建) */

    /* --- 初始化传感器 --- */
    ir_obstacle_init();
    touch_sensor_init();
    dht11_init();
    ESP_LOGI(TAG, "sensors: IR_L=%d IR_R=%d TOUCH_HEAD=%d DHT11=%d",
             SENSOR_IR_LEFT_GPIO, SENSOR_IR_RIGHT_GPIO,
             SENSOR_TOUCH_HEAD_GPIO, SENSOR_DHT11_GPIO);

    /* --- 初始化 BOOT 键 --- */
    gpio_config_t boot_io = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_io);

    /* --- 初始化音频 (可选, 不影响核心功能) --- */
    mic_init();
    mic_start();           /* 必须显式使能 I2S 通道, 否则 mic_read 永远返回 0 */
    speaker_init();
    speaker_start();       /* 同理: 不使能的话 speaker_write 写不进去, 一点声音都没有 */
    ESP_LOGI(TAG, "audio init: mic(I2S1) + speaker(I2S0)");
    /* 开机提示音: 能听到就说明 I2S 功放链路是通的 */
    speaker_beep(1046, 90);
    speaker_beep(1318, 130);

    /* --- 初始化小智 AI 客户端 (联网后自动激活 + 连 WebSocket) --- */
    ai_client_init();

    /* --- 创建任务 --- */
    /* 新表情引擎的 SDF 光栅调用层次更深(嘴/装饰件内有几十个 float 的顶点数组),
       栈必须留够, 否则会栈溢出导致画面花掉 */
    /* 以下各栈都按"实测水位 + 富余"给 —— 内部 RAM 要省出来给
       WebSocket 任务的 24KB(它必须能连续分配, 否则报
       "Error create websocket task" 永远连不上服务器)。
       实测用量: face 2.9K, led 1.1K, sensor 2.0K, boot 0.8K, dns 2.0K */
    /* ★ 表情任务的栈放到 PSRAM。
       它只做 SDF 渲染 + I2C 刷屏, 不碰 flash(NVS 只在 /flip 的 HTTP 回调里写),
       所以 PSRAM 栈是安全的。放 PSRAM 能腾出 8KB 内部 RAM ——
       内部 RAM 要留给 WebSocket 任务的栈(必须连续分配), 而播音乐时
       音乐模块会占掉约 14KB, 导致 WS 建不出来、连不上服务器。 */
    if (xTaskCreatePinnedToCoreWithCaps(face_task, "face", 8192, NULL, 5, NULL, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS)
        ESP_LOGW(TAG, "face 任务创建失败, 回退到内部 RAM");
    else
        ESP_LOGI(TAG, "face 任务栈: PSRAM (腾出 8KB 内部 RAM 给 WebSocket)");
    /* 栈水位监视(诊断用): 平时不启用, 需要时把下面这行放开即可 */
    /* xTaskCreatePinnedToCore(stack_mon_task, "stackmon", 3072, NULL, 1, NULL, 0); */
#if TTS_SELFTEST_ON_BOOT
    if (xTaskCreatePinnedToCore(selftest_watch_task, "selftest_w", 3072, NULL, 2, NULL, 0) != pdPASS)
        ESP_LOGW(TAG, "自检: 看门狗任务创建失败(内部 RAM 不足)");
#endif
    /* led_task 已删除: 灯环交给 light_init() 的灯效引擎 */
    light_init();
    music_init();   /* 网易云在线音乐(走自建代理) */
    persona_init(); /* 人格引擎: 心情/精力/无聊/亲密 → 自主转头 + 心情灯 */
    xTaskCreatePinnedToCore(sensor_task, "sensor", 3072, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(env_task, "env", 3072, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(boot_button_task, "boot", 2048, NULL, 5, NULL, 0);
    /* 注意: DNS 劫持任务在 start_wifi() 里创建, 那里才有 AP 的上下文 */

    ESP_LOGI(TAG, "Ready! 短按BOOT=切表情, 长按BOOT=台灯开关");
    ESP_LOGI(TAG, "触摸头部=害羞, 手靠近=连续跟手追踪");
    ESP_LOGI(TAG, "AI 对话: 直接说「你好小智」, 不用按键");
}
