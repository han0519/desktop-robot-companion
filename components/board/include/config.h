/*
 * config.h — 机器人伴侣项目全局配置 (v2 接线表, 锁定)
 * 所有引脚、参数集中在此，修改引脚只改这一个文件
 */
#pragma once

/* ============ I2C (OLED) ============ */
#define CONFIG_I2C_SDA_GPIO       17
#define CONFIG_I2C_SCL_GPIO       18
#define CONFIG_I2C_PORT           I2C_NUM_0
/* SSD1305 支持 400kHz, 但我们的线是杜邦线 + 旁边有舵机 PWM/WiFi 突发,
   400kHz 下实测间歇性传输失败(而且是"眼睛那一页"失败, 屏幕看着像没亮)。
   降到 200kHz: 一帧(8页 ~1KB) 也只要 ~60ms, 平时只发脏页(1~3页)更快,
   换来传输稳定得多。别降到 100kHz —— 那时整屏刷新会到 100ms 量级, 明显卡。 */
#define CONFIG_I2C_FREQ_HZ        200000
#define CONFIG_OLED_ADDR          0x3C
/* 屏幕方向: 1 = 翻转该轴。这里只是「首次上电的默认值」, 之后在网页上改的
   会存进 NVS 并优先生效, 不用重新编译烧录。 */
#define CONFIG_OLED_FLIP_X        0
#define CONFIG_OLED_FLIP_Y        0
#define CONFIG_OLED_WIDTH         128
#define CONFIG_OLED_HEIGHT        64

/* ============ 舵机 (3路 PWM, LEDC) ============ */
#define SERVO_PAN_GPIO            12   /* 舵机1: 左右转头 (180°) */
#define SERVO_TILT_GPIO           13   /* 舵机2: 上下点头 (180°) */
#define SERVO_ARM_GPIO            14   /* 舵机3: 长臂台灯 (预留, 暂不接) */

#define SERVO_TIMER               LEDC_TIMER_0
#define SERVO_MODE                LEDC_LOW_SPEED_MODE
#define SERVO_FREQ_HZ             50
#define SERVO_RESOLUTION          LEDC_TIMER_13_BIT
#define SERVO_CH_PAN              LEDC_CHANNEL_0
#define SERVO_CH_TILT             LEDC_CHANNEL_1
#define SERVO_CH_ARM              LEDC_CHANNEL_2

#define SERVO_ANGLE_MIN           0
#define SERVO_ANGLE_MAX           180
#define SERVO_ANGLE_CENTER        90
#define SERVO_TILT_INVERT         1   /* 1=反转上下方向(低头<->抬头), 0=正常 */
/* 上下点头机械限位 (根据实际结构调整, 当前结构约±5度) */
#define SERVO_TILT_PHYS_MIN       85
#define SERVO_TILT_PHYS_MAX       95

/* ============ WS2812 灯环 (8灯, RMT, 预留) ============ */
#define WS2812_LED_RING_GPIO      3    /* 灯环数据脚 (设备未到, 预留) */
#define WS2812_LED_RING_NUM       8
#define WS2812_RMT_RESOLUTION_HZ  10000000

/* 板载 WS2812 (固定 GPIO48) */
#define WS2812_ONBOARD_GPIO       48
#define WS2812_ONBOARD_NUM        1

/* ============ 传感器 ============ */
/* 红外避障 FC-51 x2 (低电平=检测到) */
#define SENSOR_IR_LEFT_GPIO       38   /* 左侧红外 */
#define SENSOR_IR_RIGHT_GPIO      39   /* 右侧红外 */

/* 触摸传感器 TTP223 x2 (高电平=触摸) */
#define SENSOR_TOUCH_HEAD_GPIO    15   /* 头部触摸 → 撸猫害羞 */
#define SENSOR_TOUCH_BODY_GPIO    16   /* 身体触摸 → 切换灯色 */

/* DHT11 温湿度 (单总线) */
#define SENSOR_DHT11_GPIO         8

/* ============ I2S0: MAX98357A 音频功放 (输出) ============
 * 依据 docs/wiring_final_v2.md 的锁定接线:
 *   GPIO4 = SD(关断) / GPIO5 = BCLK / GPIO6 = LRC / GPIO7 = DIN
 * 不要再乱动这四个 —— 它们和 INMP441(GPIO9/10/11) 是两套独立的 I2S。 */
#define AUDIO_I2S_OUT_BCLK_GPIO   5
#define AUDIO_I2S_OUT_LRC_GPIO    6
#define AUDIO_I2S_OUT_DOUT_GPIO   7
#define AUDIO_I2S_OUT_SD_GPIO     4

/* ============ I2S1: INMP441 麦克风 (输入) ============ */
#define AUDIO_I2S_IN_BCLK_GPIO    9
#define AUDIO_I2S_IN_WS_GPIO      10
#define AUDIO_I2S_IN_DIN_GPIO     11

/* 上行(麦克风)必须是 16000 —— 服务端 hello 就这么要求, 而且 esp-sr 的
   WakeNet9「你好小智」模型只吃 16kHz, 改了唤醒词会直接失效。 */
#define AUDIO_SAMPLE_RATE         16000

/* 下行(喇叭)用 24000: 服务端下发的 Opus 本身就是 24kHz。
   若按 16kHz 播放, Opus 要先把 24k 降到 16k, 高频上限只剩 8kHz ——
   听感就是"声音糊/发闷"。24kHz 能保留到 12kHz, 明显清亮。
   (喵伴 MIAOBAN 用的就是 24kHz 输出) */
#define AUDIO_OUTPUT_SAMPLE_RATE  24000

#define AUDIO_BITS_PER_SAMPLE     16

/* ============ BOOT 键 ============ */
#define BOOT_BUTTON_GPIO          0

/* ============ WiFi / AI (用户自行填入) ============ */
#define WIFI_SSID                 "YOUR_WIFI_SSID"
#define WIFI_PASSWORD             "YOUR_WIFI_PASSWORD"
#define XIAOZHI_API_KEY           "YOUR_API_KEY"
#define XIAOZHI_API_URL           "https://api.example.com/v1/chat"

/* ============ 行为参数 ============ */
#define FACE_NORMAL_DURATION_MS   12000
#define FACE_OTHER_DURATION_MS    5000
#define BLINK_INTERVAL_MIN_MS     2000
#define BLINK_INTERVAL_MAX_MS     5000

/* ============ 舵机伺服 (后台插值任务) ============ */
#define SERVO_TASK_PERIOD_MS      20     /* 伺服周期 (50Hz 轨迹规划) */
#define SERVO_TASK_STACK          3072
#define SERVO_TASK_PRIO           6      /* 高于 sensor 任务, 保证换相及时 */
#define SERVO_TASK_CORE           0
#define SERVO_EASE_GAIN           0.40f  /* 每周期向目标逼近比例 (ease-out) */
#define SERVO_MIN_STEP_DEG        0.20f  /* 最小步进, 消除渐近尾巴卡顿 */
#define SERVO_ARRIVE_EPS          0.35f  /* 到位判定阈值 (度) */
#define SERVO_SMOOTH_SPEEDUP      1.6f   /* smooth_to 的速度补偿系数 */
#define SERVO_MIN_SPEED_DPS       15     /* smooth_to 最小角速度 */

/* ============ 双红外跟手 (连续随动) ============ */
#define SENSOR_TASK_PERIOD_MS     20     /* 传感器轮询周期 */
#define IR_DETECT_CONFIRM_MS      20     /* 检测到手需稳定的时间 (消抖) */
#define IR_RELEASE_CONFIRM_MS     100    /* 手离开需稳定的时间 (释放迟滞, 防边界抖动) */

#define TRACK_ENTER_MS            15     /* 手出现 → 进入追踪(越小越灵敏) */
#define TRACK_EXIT_MS             250    /* 手消失 → 退出追踪 */
#define TRACK_PAN_CENTER          90     /* 追踪中心角 */
#define TRACK_OFFSET_MAX          42     /* 最大偏离量 → pan ∈ [48,132] */
#define TRACK_TAU_MS              70     /* 跟随平滑时间常数: 越小越灵敏(原 130 偏钝) */
#define TRACK_VEL_MAX_DPS         200    /* 跟随时舵机最大角速度 */
#define TRACK_RETURN_DPS          120    /* 退出追踪回中角速度 */
#define TRACK_TILT_LOOK_UP        85     /* 追踪时的抬头角度 (受 TILT_PHYS 限位约束) */

/* ============ 小智免触摸对话 (本地 VAD 语音活动检测) ============
 * 没有 esp-sr 唤醒词时, 用麦克风能量判断「有人在说话」.
 * 一帧 60ms, 同时也是 Opus 帧长, 不额外增加延迟.
 *
 * 实测标定 (INMP441 @16kHz, 安静房间):
 *   静止噪声底 rms ≈ 100~140, 偶发环境峰值 ≈ 290
 *   正常说话   rms ≈ 700 以上
 * 所以阈值取 420: 高于环境峰值 1.4 倍, 又明显低于说话电平.
 * 误触发多 → 调大 VAD_MIN_RMS; 小声说话不响应 → 调小. */
#define VAD_START_FRAMES          3      /* 连续 3 帧(180ms)人声 → 开始对话 */
/* 连续静音多久 → 认为说完了。
   ★ 必须给够! 实测: 用户喊完"你好小智"后会自然停顿(等叮咚再说话),
     800ms 会把这个停顿当成"说完了"立刻发 listen stop, 结果服务端
     每次只收到"你好小智", 后面的问题全被切掉 —— 表现为"它只会打招呼,
     根本不回答我的问题", 大模型也因此没有机会调用 MCP 工具。
     2 秒足够覆盖正常说话中的停顿, 又不会让人觉得拖沓。 */
#define VAD_STOP_MS               2000
#define VAD_MARGIN                3.0f   /* 触发阈值 = 噪声底 × 该倍数 */
#define VAD_MIN_RMS               420.0f /* 触发阈值绝对下限 */
#define VAD_COOLDOWN_MS           600    /* 播报结束后冷却, 防扬声器余音自触发 */
