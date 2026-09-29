/*
 * ne_client.h — 网易云音乐【直连】客户端 (不经过任何服务器)
 *
 * 为什么要有它: 开源项目里别人没有服务器, 做不了代理中转。所以设备自己直连网易云:
 *   · 加密参数(AES-CBC + RSA + MD5)在设备上用 mbedTLS 现算
 *   · HTTPS 直连 music.163.com / interface3.music.163.com
 *   · 登录: 在【设备自己的网页控制台】里显示二维码, 手机扫码 → Cookie 存进 NVS
 *
 * ★ 接口契约和原来的代理完全一致(同样是 {code,data:[{id,name,artist}]}),
 *   所以 music.c 里那套解析/播放/断点续传逻辑一行都不用改, 只换数据来源。
 *
 * 实测(ESP32-S3, 240MHz):
 *   加密 13ms; 一次完整 HTTPS 请求约 2.2s; TLS 会话占约 6.1KB 内部 RAM
 *   收发期间需要把 AI 音频链短暂挂起(AES 硬件加速要 DMA 内存), 见 ne_post()
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 初始化: 从 NVS 读回上次登录的 Cookie */
void ne_client_init(void);

/* 是否已登录(有有效 Cookie) */
bool ne_client_logged_in(void);

/* 当前登录账号昵称(未登录返回空串) */
const char *ne_client_nick(void);

/* ---------- 四个业务接口, 返回的 JSON 与原代理逐字兼容 ---------- */
/* 搜索: {"code":200,"data":[{"id","name","artist"}]} */
bool ne_api_search(const char *keyword, int limit, char *out, size_t out_sz);
/* 按歌手搜歌: ① 搜歌手拿 id ② 取该歌手热门歌曲(输出与 ne_api_search 同构) */
bool ne_api_search_artist_id(const char *name, char *aid, size_t aid_sz,
                             char *aname, size_t aname_sz);
bool ne_api_artist_hot(const char *aid, int limit, char *out, size_t out_sz);
/* 我的歌单: {"code":200,"data":[{"id","name","count"}]} */
bool ne_api_playlists(char *out, size_t out_sz);
/* 歌单歌曲: {"code":200,"name":"歌单名","data":[{"id","name","artist"}]} */
bool ne_api_playlist_songs(const char *id, int limit, char *out, size_t out_sz);
/* 取播放地址(需要登录): 成功把 CDN 地址写进 out_url, 返回 true */
bool ne_api_song_url(const char *id, int br, char *out_url, size_t out_sz);

/* ---------- 扫码登录(供设备网页控制台用) ---------- */
/* 取二维码 key; 成功后网页据此渲染二维码: https://music.163.com/login?codekey=<key> */
bool ne_login_qr_key(char *key_out, size_t key_sz);
/* 轮询: 返回网易云的 code(801 等待扫码 / 802 待确认 / 803 成功 / 800 过期)。
   803 时会把 Cookie 存进 NVS, 昵称写进 nick_out */
int  ne_login_qr_check(const char *key, char *nick_out, size_t nick_sz);
/* 退出登录(清掉 NVS 里的 Cookie) */
void ne_login_logout(void);
/* 网页用: 一行 JSON 描述登录状态 {"login":b,"nick":"..."} */
void ne_login_status_json(char *out, size_t out_sz);

/* ---- 以下给设备网页控制台的"扫码登录"卡片用 ----
 * 因为 TLS 握手要 16KB 栈, 不能放在 httpd 回调里同步执行, 所以拆成:
 *   网页每 2 秒请求一次 → 触发一次后台轮询(不阻塞) → 立刻返回当前状态 */
void ne_login_worker_start(void);                   /* 开机建一次: 常驻轮询任务 */
void ne_login_request_key(void);                    /* 网页点登录/刷新 → 去取一张新二维码 */
/* 手动粘贴 Cookie 登录(兜底路径): 形如 "MUSIC_U=xxx; __csrf=yyy" */
bool ne_login_set_cookie(const char *ck);
/* 校验当前 Cookie 在网易云那边是否真的有效(-1 未验/0 失效/1 有效) */
bool ne_client_verify(char *nick_out, size_t nick_sz);
int  ne_client_cookie_ok(void);
/* 登录态落盘: 打标记 / 由"内部 RAM 栈"的任务执行(见 ne_client.c 注释) */
bool ne_cookie_save_pending(void);
void ne_cookie_save_now(void);
void ne_login_reset(void);                          /* 重新取二维码(过期后点"刷新") */
void ne_login_state_json(char *out, size_t out_sz); /* {"login","nick","key","code","busy"} */
