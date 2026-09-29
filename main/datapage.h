/*
 * datapage.h — 数据页(时间 + 环境温湿度)
 *
 * BOOT 短按在「表情页 ↔ 数据页」之间切换。数据页显示:
 *   · 当前时间(SNTP 对时, Asia/Shanghai, 每秒刷新)
 *   · DHT11 温湿度(后台任务 3 秒一读, 页面跟随刷新)
 * 布局样式可由网页推送选择(/datapage?style=0..4), 存 NVS 掉电不丢。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* 样式数(与 docs/data_preview.html 里的 A~E 一一对应) */
#define DP_STYLE_COUNT 5

void datapage_init(void);             /* 启动 SNTP 对时 + 读取上次样式(NVS) */
void datapage_toggle(void);           /* BOOT 短按: 表情页 ↔ 数据页 */
bool datapage_active(void);           /* 当前是否显示数据页 */
void datapage_force_redraw(void);     /* 退出数据页时调用, 让表情页立刻重画 */

/* 主循环调用: 数据页激活时由它接管屏幕(内部 1Hz 重画, 秒变化才刷) */
void datapage_update(uint32_t dt_ms);

/* 网页推送样式 0..4, 存 NVS */
void datapage_set_style(int style);
int  datapage_get_style(void);
const char *datapage_style_name(int style);

/* DHT11 任务把最新读数喂进来(数据页直接显示) */
void datapage_set_env(float temp_c, float hum_pct, bool valid);

/* SNTP 是否已对上时(没对上时数据页时间显示 --:--:--) */
bool datapage_time_synced(void);
