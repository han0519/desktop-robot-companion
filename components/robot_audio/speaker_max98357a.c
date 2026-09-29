/*
 * speaker_max98357a.c — MAX98357A I2S 功放实现 (I2S0)
 */
#include "speaker_max98357a.h"
#include "config.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "speaker";
static i2s_chan_handle_t s_tx_chan = NULL;
static bool s_muted = false;
static bool s_started = false;   /* I2S 发送通道是否已使能 */
/* 音量百分比 0~100。
 * ★ 默认值必须给够: 缩放用的是平方律, 20% 实际振幅只剩 0.2² = 0.04
 *   (约 -28dB) —— 实测就是"解码明明成功、电平也正常, 但一点声音都听不到"。
 *   70% → 0.49, 是正常能听清的水平。可用语音或网页再调。 */
static int  s_volume = 70;

/* ---------- 空闲关断功放 ----------
 * MAX98357A 的 SD 脚拉低即进入关断, 静态功耗和底噪都归零。
 * 之前 SD 一直拉高, 功放常开 —— 杜邦线接触不良/浮空输入/WiFi 射频/舵机线串扰
 * 拾到的干扰会被原样放大, 听起来就是"持续性的兹拉声"。
 * 策略: 最后一次写数据后 400ms 关断; 下次要出声前 2ms 唤醒。 */
/* 越小 -> 播完越早关断 -> WiFi 发射期功放已经关掉, 听不到串扰。
   代价是连续语音(如 TTS)中间若有 >该值的停顿会被切开, 120ms 对 TTS 足够(帧间隔 60ms)。 */
#define SPK_IDLE_MUTE_MS  120
static bool     s_amp_on = false;   /* 功放当前是否处于使能(非关断)状态 */
static uint32_t s_last_write_ms = 0;

/* ★ 持续出声期间把功放"钉住"。
 *
 * 空闲关断的判据是"距上次 speaker_write 超过 SPK_IDLE_MUTE_MS(120ms)"。
 * 放语音时每帧 60ms 一次写, 没问题; 但【播在线音乐】时 speaker_write()
 * 会因为等 I2S 的 DMA 空位而阻塞几百毫秒, 期间没有新的写入 ——
 * 空闲任务就误判成"空闲", 在播放中途把功放关掉, 声音直接断一块。
 * 实测日志: "首帧解码成功" 之后 840ms 就打了 "空闲 → 关断功放"。
 *
 * 所以播音乐时调 speaker_set_hold(true) 把功放钉住, 结束再放开。 */
static bool     s_hold = false;
static volatile bool s_writing = false;    /* 正在写数据(供空闲任务判断, 见 speaker_write) */

static void apply_sd(void)
{
    /* 用户静音 或 空闲 -> 关断 */
    gpio_set_level(AUDIO_I2S_OUT_SD_GPIO, (!s_muted && s_amp_on) ? 1 : 0);
}

/* FreeRTOS tick 计时(100Hz -> 10ms 精度) 够用, 且不必给组件加 esp_timer 依赖 */
static inline uint32_t now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS;
}

/* 当前播放采样率(语音 24kHz / 音乐 44.1kHz), 由 speaker_set_sample_rate 切换 */
static int s_rate = AUDIO_OUTPUT_SAMPLE_RATE;

int speaker_get_sample_rate(void) { return s_rate; }

int speaker_set_sample_rate(int hz)
{
    if (!s_tx_chan || hz <= 0) return ESP_ERR_INVALID_ARG;
    if (hz == s_rate) return ESP_OK;

    bool was = s_started;
    if (was) i2s_channel_disable(s_tx_chan);

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(hz);
    esp_err_t r = i2s_channel_reconfig_std_clock(s_tx_chan, &clk);
    if (r == ESP_OK) {
        s_rate = hz;
        ESP_LOGI(TAG, "播放采样率 -> %d Hz", hz);
    } else {
        ESP_LOGW(TAG, "切换采样率到 %d Hz 失败: %s", hz, esp_err_to_name(r));
    }
    if (was) {
        /* 重新使能后 DMA 里是旧数据, 先冲掉再预填静音, 避免"咔"一声 */
        i2s_channel_enable(s_tx_chan);
    }
    return r;
}

/* 把功放钉住(播在线音乐时用), 必须定义在 apply_sd / now_ms 之后 */
void speaker_set_hold(bool on)
{
    s_hold = on;
    if (on) {
        s_last_write_ms = now_ms();
        if (!s_amp_on) { s_amp_on = true; apply_sd(); }
    }
    ESP_LOGI(TAG, "功放保持: %s", on ? "开(播放中不再自动关断)" : "关");
}
bool speaker_get_hold(void) { return s_hold; }

static void speaker_idle_task(void *arg)
{
    while (1) {
        uint32_t now = now_ms();
        /* s_hold 期间绝不关断(播在线音乐时 speaker_write 会阻塞很久) */
        /* s_writing: 正在写数据(DMA 满时写入会阻塞几百 ms, 但那是"正在播")
           s_hold:    调用方显式要求钉住(短促的语音播报用, 帧间空档较大)
           否则按"最后一次写完 120ms 内仍算忙"来判断 */
        bool want = s_writing || s_hold || ((now - s_last_write_ms) < SPK_IDLE_MUTE_MS);
        if (want != s_amp_on) {
            s_amp_on = want;
            apply_sd();
            if (!want) ESP_LOGI(TAG, "空闲 → 关断功放 (消除静态底噪)");
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void speaker_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* 描述符数量必须留足余量: TTS 每帧一次写 960 样本(1920 字节),
       若缓冲总量正好等于 1920 字节, 一次写就会撑满并超时.
       8 x 240 帧 x 2 字节 = 3840 字节, 一帧有余。 */
    chan_cfg.dma_desc_num = 16;
    chan_cfg.dma_frame_num = 240;
    /* 关键: 默认 false 时, I2S 一旦没有数据可发就会「无限重播最后一个 DMA 缓冲」——
       提示音/TTS 播完后就变成持续的兹拉声。打开它让下溢时自动输出静音。 */
    chan_cfg.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_OUTPUT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_OUT_BCLK_GPIO,
            .ws   = AUDIO_I2S_OUT_LRC_GPIO,
            .dout = AUDIO_I2S_OUT_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));

    /* SD 引脚: 高电平使能, 低电平关断. 这里设为输出高电平常使能 */
    gpio_config_t sd_io = {
        .pin_bit_mask = (1ULL << AUDIO_I2S_OUT_SD_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&sd_io);
    gpio_set_level(AUDIO_I2S_OUT_SD_GPIO, 1);
    s_amp_on = true;      /* 与引脚实际状态保持一致, 空闲任务才能正确把它关掉 */

    ESP_LOGI(TAG, "MAX98357A init: BCLK=GPIO%d, LRC=GPIO%d, DOUT=GPIO%d, SD=GPIO%d",
             AUDIO_I2S_OUT_BCLK_GPIO, AUDIO_I2S_OUT_LRC_GPIO,
             AUDIO_I2S_OUT_DOUT_GPIO, AUDIO_I2S_OUT_SD_GPIO);

    /* 空闲关断功放的守护任务。
       栈必须给足: 实测 2048 字节时剩余水位只有 76 字(304 字节),
       ESP_LOGI 的格式化就会把栈踩穿 —— 表现为「一喊唤醒词就重启」 */
    xTaskCreatePinnedToCore(speaker_idle_task, "spk_idle", 4096, NULL, 2, NULL, 0);
}

void speaker_start(void)
{
    if (!s_tx_chan || s_started) return;

    /* 使能前把「整个 DMA 缓冲」都填成静音。
       不能只填一块: 缓冲是 malloc 出来的未初始化内存, 没填到的描述符会被当作
       音频播放, 听感就是一片兹拉声。同时预填也避免刚使能时首帧写入拿不到
       空描述符而 ESP_ERR_TIMEOUT。 */
    static int16_t silence[240] = {0};
    size_t total = 0;
    for (int i = 0; i < 16; i++) {         /* 16 = dma_desc_num */
        size_t n = 0;
        if (i2s_channel_preload_data(s_tx_chan, silence, sizeof(silence), &n) != ESP_OK) break;
        total += n;
        if (n < sizeof(silence)) break;
    }

    esp_err_t r = i2s_channel_enable(s_tx_chan);
    if (r == ESP_OK) {
        s_started = true;
        ESP_LOGI(TAG, "I2S 发送通道已使能 (预填静音 %u 字节)", (unsigned)total);
    } else {
        ESP_LOGE(TAG, "I2S 使能失败: %s", esp_err_to_name(r));
    }
}

size_t speaker_write(const int16_t *buf, size_t sample_count, int timeout_ms)
{
    if (!s_tx_chan || !buf || s_muted) return 0;
    /* ★ 标记"正在写数据": 空闲任务看到它就不会关功放。
       为什么需要: i2s_channel_write 在 DMA 满时会阻塞几百毫秒, 期间
       s_last_write_ms 一直不更新, 空闲任务会误判"空闲"而在播放中关掉功放
       (声音断一块)。以前是靠"整首歌期间 speaker_set_hold(true)"绕过去的,
       但那等于让功放【整首歌常开】—— 杜邦线/WiFi/舵机的串扰会被一直放大,
       就是那个"持续性兹拉声"。现在只在真正写数据的瞬间保持开启, 更干净。 */
    s_writing = true;
    /* 兜底: 通道没使能的话 i2s_channel_write 一个字节都写不进去 */
    if (!s_started) speaker_start();
    if (!s_started) return 0;

    /* 功放若处于空闲关断, 先唤醒并等它退出关断(约 1ms)再送数据 */
    if (!s_amp_on) {
        s_amp_on = true;
        apply_sd();
        esp_rom_delay_us(2000);
    }
    s_last_write_ms = now_ms();

    /* ★ 音量缩放: 之前 s_volume 只在提示音里生效, 播报(TTS)永远 100%,
       导致"调了音量却没变化"。这里统一缩放。
       · 100% 时直接透传, 不做无谓的搬运
       · 用静态缓冲而非栈: /beep 是从 httpd 任务(栈仅 4KB)调进来的,
         在栈上开 8KB 会溢出
       · 超过缓冲就放弃缩放(宁可响着, 也不能截断音频) */
    static int16_t vbuf[4096];
    const int16_t *src = buf;
    /* 始终搬进内部缓冲(不再"100% 时直接透传") ——
       I2S 的 DMA 只能访问内部 RAM, 而这里的 buf 可能来自 PSRAM
       (在线音乐的解码输出就在 PSRAM), 直接透传会踩坏内存。
       多一次 memcpy 远比偶发死机划算。 */
    if (sample_count <= sizeof(vbuf) / sizeof(vbuf[0])) {
        /* ★ 照搬官方 no_audio_codec.cc:217-245 —— 音量用「平方律」而非线性。
           线性缩放的听感是"前半程几乎没变化、后半程突然变很大", 因为人耳对
           响度的感知接近对数。pow(v/100, 2) 能把旋钮行程映射成接近线性的听感。
           MAX98357A 没有硬件音量寄存器, 只能软件缩放。 */
        const float g = (float)s_volume / 100.0f;
        const float factor = g * g;
        for (size_t i = 0; i < sample_count; i++) {
            int32_t v = (int32_t)((float)buf[i] * factor);
            if (v >  32767) v =  32767;      /* 饱和削波, 防溢出破音 */
            if (v < -32768) v = -32768;
            vbuf[i] = (int16_t)v;
        }
        src = vbuf;
    }

    size_t bytes_written = 0;
    esp_err_t r = i2s_channel_write(s_tx_chan, src, sample_count * sizeof(int16_t),
                                    &bytes_written, pdMS_TO_TICKS(timeout_ms));
    if (r != ESP_OK) {
        /* 刚使能/时钟刚起时偶发一次拿不到描述符, 等一下重试一次就好
           (实测只出现在开机第一帧)。
           ★ 但这条路径里有 15ms 的 vTaskDelay —— 如果它【频繁】触发,
             每次写入都会白白多花 15ms, 播放就会整体慢半拍、卡顿。
             所以这里必须打日志: 出现频繁就说明 i2s_channel_write 一直在失败。 */
        ESP_LOGW(TAG, "写入重试: %s (样本 %u, 已写 %u), 15ms 后重试",
                 esp_err_to_name(r), (unsigned)sample_count, (unsigned)bytes_written);
        vTaskDelay(pdMS_TO_TICKS(15));
        bytes_written = 0;
        r = i2s_channel_write(s_tx_chan, buf, sample_count * sizeof(int16_t),
                              &bytes_written, pdMS_TO_TICKS(timeout_ms));
    }
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "i2s write 失败: %s (写入 %u 字节)",
                 esp_err_to_name(r), (unsigned)bytes_written);
    }
    s_writing = false;
    return bytes_written / sizeof(int16_t);
}

void speaker_set_volume(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    ESP_LOGI(TAG, "提示音音量 -> %d%%", percent);
}

int speaker_get_volume(void) { return s_volume; }

void speaker_set_mute(bool mute)
{
    s_muted = mute;
    if (!mute) s_amp_on = true;   /* 解除静音时立刻唤醒 */
    apply_sd();
}

void speaker_beep(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!s_tx_chan || s_muted) return;
    speaker_start();

    if (duration_ms > 156) duration_ms = 156;   /* 与上面的静态缓冲容量一致 */
    /* 用"当前"采样率生成, 而不是编译期常量 ——
       播音乐时可能是 44.1kHz, 若仍按 24kHz 生成, 提示音音高会跑调 12 个半音。 */
    int rate = s_rate;
    size_t total_samples = (size_t)rate * duration_ms / 1000;
    /* 用【静态】缓冲, 绝不做 malloc/free。
       之前是 heap_caps_malloc + free: 提示音刚结束、内存被复用后,
       约 120ms 内就会踩坏堆 —— 实测表现为别的任务打日志时
       "assert failed: xQueueSemaphoreTake ... (pxQueue->uxItemSize == 0)"
       直接重启(唤醒"叮咚"响完就崩)。静态数组在内部 RAM, 天然 DMA 可访问。 */
    /* ★ 缓冲要省着用: 这是内部 RAM 里的静态数组, 而内部 RAM 还要留给
       WebSocket 任务的栈(必须连续分配, 不够就报 "Error create websocket task")。
       156ms @24kHz 足够覆盖所有提示音(实际用的最长的也就 90ms)。 */
    static int16_t buf[AUDIO_OUTPUT_SAMPLE_RATE * 156 / 1000];   /* 156ms @24kHz = 3744 样本 */
    if (total_samples > sizeof(buf) / sizeof(buf[0]))
        total_samples = sizeof(buf) / sizeof(buf[0]);

    float phase = 0;
    float phase_inc = 2.0f * 3.14159f * freq_hz / (float)rate;
    for (size_t i = 0; i < total_samples; i++) {
        /* 渐入渐出避免爆音 */
        float envelope = 1.0f;
        if (i < total_samples / 10) envelope = (float)i / (total_samples / 10);
        else if (i > total_samples * 9 / 10) envelope = (float)(total_samples - i) / (total_samples / 10);
        /* 音量已由 speaker_write() 统一处理, 这里只生成满幅波形 */
        buf[i] = (int16_t)(sinf(phase) * 32767.0f * envelope);
        phase += phase_inc;
        if (phase > 2.0f * 3.14159f) phase -= 2.0f * 3.14159f;
    }

    /* 一次性写完: 分块写会在块边界产生不连续, 听感上就是细碎的兹拉声 */
    size_t pos = 0;
    while (pos < total_samples) {
        size_t chunk = total_samples - pos;
        if (chunk > 3840) chunk = 3840;      /* 不超过半个 DMA 缓冲 */
        size_t w = speaker_write(buf + pos, chunk, 1000);
        if (w == 0) break;
        pos += w;
    }

    /* 尾巴补一段静音, 让最后一个描述符干净收尾(避免"啪"的爆音) */
    static int16_t tail[240] = {0};
    speaker_write(tail, 240, 500);
}
