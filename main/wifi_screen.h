/*
 * wifi_screen.h — SSD1305 配网信息页
 *
 * 首次上电(未配过网)时在 128x64 单色屏上显示配网指引:
 *   - 热点名称 / 密码 / 配置网址
 * 配网成功后切回表情系统。
 *
 * 注意: 本组件直接操作 ssd1305 的显存, 与 face 组件共用同一块 buffer,
 *       所以两者不能同时刷新 —— 由 main 里的 s_provisioning 状态二选一。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 配网指引页: 显示热点名/密码/网址 */
void wifi_screen_provision(const char *ssid, const char *pass);

/** 已保存配置, 正在连接路由 */
void wifi_screen_connecting(void);

/** 配网成功: 显示拿到的 IP */
void wifi_screen_ok(const char *ip);

/** 连不上路由: 提示失败, 并把热点信息再列一遍方便重新配 */
void wifi_screen_failed(const char *ap_ssid, const char *ap_pass);

/**
 * @brief 由 UI 任务每帧调用: 画闪烁指示并刷新
 * @param now_ms 当前毫秒
 */
void wifi_screen_tick(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
