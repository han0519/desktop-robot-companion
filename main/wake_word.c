/*
 * wake_word.c — esp-sr WakeNet 唤醒词实现
 *
 * 用的是官方 xiaozhi 固件(78/xiaozhi-esp32)同一套 API 与同一个模型
 * (CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS = 「你好小智」)。
 */
#include "wake_word.h"

#include <string.h>
#include "esp_log.h"

#include "model_path.h"      /* esp_srmodel_init / esp_srmodel_filter */
#include "esp_wn_iface.h"    /* esp_wn_iface_t / det_mode_t */
#include "esp_wn_models.h"   /* esp_wn_handle_from_name / ESP_WN_PREFIX */

static const char *TAG = "wakeword";

static srmodel_list_t       *s_models = NULL;
static const esp_wn_iface_t *s_iface  = NULL;
static model_iface_data_t   *s_data   = NULL;
static int                   s_chunk  = 0;      /* 每帧样本数 */
static int                   s_rate   = 0;
static char                  s_name[48] = "-";
static bool                  s_ready  = false;

/* 累积缓冲: 麦克风每轮给 60ms(960 样本), 而 WakeNet 一帧只要 512 样本(32ms).
 * 必须把余下的样本留到下一轮, 否则会丢音频、出现断点, 唤醒率大幅下降. */
#define WW_BUF_SAMPLES 2048
static int16_t s_buf[WW_BUF_SAMPLES];
static int     s_len = 0;

bool wake_word_init(void)
{
    if (s_ready) return true;

    /* 从名为 "model" 的分区加载模型列表 */
    s_models = esp_srmodel_init("model");
    if (s_models == NULL || s_models->num <= 0) {
        ESP_LOGE(TAG, "model 分区里没有模型 (num=%d)。"
                      "检查 partitions.csv 是否有 model 分区、"
                      "以及 sdkconfig 里的 CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y",
                 s_models ? s_models->num : -1);
        return false;
    }

    char *name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    if (name == NULL) {
        ESP_LOGE(TAG, "分区里有模型但没有 WakeNet(wn*) 模型");
        esp_srmodel_deinit(s_models);      /* ★ 失败路径别漏掉释放 */
        s_models = NULL;
        return false;
    }

    s_iface = esp_wn_handle_from_name(name);
    if (s_iface == NULL) {
        ESP_LOGE(TAG, "取不到 %s 的 WakeNet 接口", name);
        return false;
    }

    /* DET_MODE_95 = 激进模式, 命中率高一点, 官方也用它 */
    s_data = s_iface->create(name, DET_MODE_95);
    if (s_data == NULL) {
        ESP_LOGE(TAG, "创建唤醒词模型 %s 失败(内存不足?)", name);
        return false;
    }

    s_chunk = s_iface->get_samp_chunksize(s_data);
    s_rate  = s_iface->get_samp_rate(s_data);
    strncpy(s_name, name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    s_ready = true;

    ESP_LOGI(TAG, "唤醒词就绪: %s  采样率=%dHz  每帧=%d 样本  字数=%d",
             s_name, s_rate, s_chunk, s_iface->get_word_num(s_data));
    return true;
}

/* ★ 重建引擎(销毁模型实例再建一个)。
 *
 * 为什么需要它: 放音乐时整条 AI 音频链被挂起(几十秒不喂音频), 之后再恢复时,
 * 只调 wake_word_reset() 是【不够】的 —— 那个函数只清我们自己的累积缓冲,
 * 而模型内部的特征队列还停在暂停前那一刻的旧音频上。恢复后喂进去的声音,
 * 在它看来就是一次"跳变", 唤醒率会掉到几乎为 0 ——
 * 用户的感受正是: 【放完歌/暂停之后, 怎么喊「你好小智」都没反应】。
 * destroy + create 是官方给的正规重建路径(clean 会崩, 见 wake_word_reset 注释)。 */
void wake_word_restart(void)
{
    if (!s_ready || s_iface == NULL || s_data == NULL || s_name[0] == '-') return;
    char name[48];
    snprintf(name, sizeof(name), "%s", s_name);
    s_iface->destroy(s_data);
    s_data = s_iface->create(name, DET_MODE_95);
    s_len = 0;
    if (s_data) {
        ESP_LOGW(TAG, "唤醒词引擎已重建(清掉暂停期间的旧状态), 现在应该能正常喊醒");
    } else {
        s_ready = false;
        ESP_LOGE(TAG, "唤醒词引擎重建失败(内存不足), 退回能量 VAD");
    }
}

bool wake_word_ready(void)
{
    return s_ready;
}

int wake_word_chunk(void)
{
    return s_chunk;
}

const char *wake_word_name(void)
{
    return s_name;
}

const char *wake_word_feed(const int16_t *pcm, size_t samples)
{
    if (!s_ready || s_iface == NULL || s_data == NULL || s_chunk <= 0) return NULL;

    const char *hit = NULL;
    int off = 0;

    /* 1) 追加到累积缓冲 */
    for (size_t i = 0; i < samples; i++) {
        if (s_len >= WW_BUF_SAMPLES) break;      /* 理论到不了, 防御性截断 */
        s_buf[s_len++] = pcm[i];
    }

    /* 2) 把所有完整帧都送进引擎(不丢样本) */
    while (s_len - off >= s_chunk) {
        /* detect 的形参不是 const，这里安全地去 const（它只读输入） */
        wakenet_state_t r = s_iface->detect(s_data, s_buf + off);
        off += s_chunk;
        if (r == WAKENET_DETECTED) {
            hit = s_iface->get_word_name(s_data, r);   /* 索引从 1 开始 */
            break;
        }
    }

    /* 3) 余下的不足一帧的样本前移, 留到下一轮 */
    if (off > 0) {
        s_len -= off;
        if (s_len > 0) memmove(s_buf, s_buf + off, (size_t)s_len * sizeof(int16_t));
    }
    return hit;
}

void wake_word_reset(void)
{
    /* 只丢弃累积缓冲里剩余的样本。
     *
     * 千万不要在这里调 s_iface->clean(s_data) —— 那个接口是"销毁模型内部状态"
     * (WakeNet9 里最终走到 dl_convq_queue_bzero), 调用后 s_data 即失效。
     * 实测会在唤醒命中的瞬间抛 LoadProhibited 崩溃重启:
     *   EXCVADDR = 0x10  (对空指针解引用)
     *   wake_word_reset -> model_clean -> dl_convq_queue_bzero
     * 判定命中并重置后, WakeNet 内部状态机会自己回到初始态, 不需外部清理。 */
    s_len = 0;
}
