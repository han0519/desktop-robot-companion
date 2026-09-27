/**
 * @file face.h
 * @brief 机器人表情系统 v3  (SSD1305 128×64 单色 OLED)
 *
 * 特点:
 *   - 52 种情绪, 全部使用「四角独立圆角的圆角方块」眼睛, 无眼球/瞳孔
 *   - 左看右看: 弹簧跟随 (ζ≈0.84, 带轻微过冲) + 运动挤压
 *   - 眨眼: 上下眼睑对合 (上 55% / 下 45%), smoothstep 曲线
 *   - 全局呼吸层, 任何表情都不会「死住」
 *   - 情绪切换: 数值参数插值 + 缓动, 离散属性(嘴型/装饰/微动)中点切换
 *   - 渲染: SDF + 覆盖率抗锯齿 → 4×4 Bayer 抖动 → SSD1305 显存
 *   - 刷新: 逐帧比对显存, 只通过 I2C 发送有变化的页 (脏页局部刷新)
 */
#ifndef FACE_H
#define FACE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 情绪枚举（前 16 个索引与 v2 保持一致，老代码不受影响） */
typedef enum {
    FACE_NORMAL = 0,    /**< 平静 */
    FACE_HAPPY,         /**< 开心（笑眼） */
    FACE_SAD,           /**< 难过 */
    FACE_ANGRY,         /**< 生气（瞪眼） */
    FACE_SURPRISED,     /**< 惊讶 */
    FACE_SLEEPY,        /**< 困倦 */
    FACE_LOVE,          /**< 喜爱 */
    FACE_WINK,          /**< 眨眼（右眼单眨） */
    FACE_CRY,           /**< 哭泣 */
    FACE_DIZZY,         /**< 晕眩 */
    FACE_EXCITED,       /**< 兴奋 */
    FACE_COOL,          /**< 酷 */
    FACE_SHY,           /**< 害羞 */
    FACE_SMUG,          /**< 得意 */
    FACE_CONFUSED,      /**< 困惑 */
    FACE_THINKING,      /**< 思考 */
    /* ---------- v3 新增 ---------- */
    FACE_SMILE,         /**< 微笑 */
    FACE_LAUGH,         /**< 大笑 */
    FACE_ANNOYED,       /**< 不耐烦 */
    FACE_SHOCK,         /**< 震惊 */
    FACE_SCARED,        /**< 惊恐 */
    FACE_PANIC,         /**< 慌乱 */
    FACE_SLEEPING,      /**< 熟睡 */
    FACE_YAWN,          /**< 打哈欠 */
    FACE_TIRED,         /**< 疲惫 */
    FACE_KISS,          /**< 亲亲 */
    FACE_GRATEFUL,      /**< 感动 */
    FACE_WINK_L,        /**< 左眨 */
    FACE_WINK_R,        /**< 右眨 */
    FACE_PROUD,         /**< 骄傲 */
    FACE_CURIOUS,       /**< 好奇 */
    FACE_SUSPICIOUS,    /**< 怀疑 */
    FACE_BORED,         /**< 无聊 */
    FACE_RELIEVED,      /**< 松口气 */
    FACE_DETERMINED,    /**< 坚毅 */
    FACE_SERIOUS,       /**< 严肃 */
    FACE_SILLY,         /**< 蠢萌 */
    FACE_SICK,          /**< 不舒服 */
    FACE_DEAD,          /**< 离线 */
    FACE_HELLO,         /**< 打招呼 */
    FACE_BYE,           /**< 再见 */
    FACE_LISTENING,     /**< 聆听 */
    FACE_MUSIC,         /**< 听歌 */
    FACE_LOADING,       /**< 加载中 */
    FACE_ERROR,         /**< 错误 */
    FACE_COLD,          /**< 冷 */
    FACE_HOT,           /**< 热 */
    FACE_HUNGRY,        /**< 饿 */
    FACE_STARSTRUCK,    /**< 崇拜 */
    FACE_GUILTY,        /**< 心虚 */
    FACE_SUCCESS,       /**< 成功 */
    FACE_AWAKE,         /**< 醒来 */
    FACE_SPEAKING,      /**< 说话中(AI 播报时用): 两眼 + 嘟噜噜电流波形嘴 */
    FACE_COUNT          /**< 情绪总数 */
} face_emotion_t;

/** 表情系统初始化 */
void face_init(void);

/**
 * @brief 每帧更新并刷新 OLED（建议 30fps 调用）
 * @param dt_ms 距上一帧毫秒数
 */
void face_update(uint32_t dt_ms);

/** 手动设置当前情绪（触发平滑变形） */
void face_set_emotion(face_emotion_t e);

/** 获取当前情绪 */
face_emotion_t face_get_emotion(void);

/** 触发一次眨眼 */
void face_blink(void);

/**
 * @brief 设置情绪并保持指定时长（期间不自动切换），供 AI 对话驱动表情用
 * @param e       目标情绪
 * @param hold_ms 保持毫秒数；之后自动情绪循环恢复
 */
void face_set_emotion_hold(face_emotion_t e, uint32_t hold_ms);

/**
 * @brief 按名称设置情绪（大小写不敏感，支持英文标识或中文名）
 *        例: face_set_emotion_by_name("HAPPY") / ("开心")
 * @return true = 匹配成功
 */
bool face_set_emotion_by_name(const char *name);

/** 启用/禁用自动情绪循环 */
void face_set_auto_cycle(bool enable);

/** 覆盖所有情绪的停留时长（0 = 恢复各情绪自带时长） */
void face_set_emotion_duration(uint32_t ms);

/**
 * @brief 手动锁定视线方向（红外跟手可用）
 * @param offset 设计单位, -26(左) ~ +26(右) ≈ ±16px; 传 >=900 恢复自动扫视
 */
void face_set_look(float offset);

/**
 * @brief 开关「方向测试图」：显示一个大箭头朝上的三角 + 左上角方块 + 底部横条。
 *        用来判断屏幕方向装反了没有 —— 三角朝上、方块在左上角 = 方向正确。
 */
void face_set_test(bool on);

/** 是否处于方向测试模式 */
bool face_get_test(void);

/** 获取情绪名称（英文标识） */
/** @brief 强制下一帧整屏重绘(从别的画面切回表情时用, 避免残影) */
void face_force_full(void);

const char *face_emotion_name(face_emotion_t e);

/** 获取情绪中文名 */
const char *face_emotion_cn(face_emotion_t e);

/** 上一帧通过 I2C 实际发送的页数（0..8），用于性能观测 */
uint8_t face_last_dirty_pages(void);

#ifdef __cplusplus
}
#endif

#endif /* FACE_H */
