/*
 * music.h — 网易云在线音乐播放
 *
 * 走"自建代理"方案(与喵伴 MIAOBAN 的 ncm_proxy_v11.js 完全同一套接口):
 * 设备端不碰网易 Cookie、不做签名, 只跟代理说话。
 *
 *   GET {base}/api/search?keyword=<urlenc>&limit=10&lean=1
 *        → {"code":200,"data":[{"id","name","artist","dur"}]}
 *   GET {base}/api/stream?id=<songId>&br=128000
 *        → 200/206 + audio 流 (MP3, 通常 44.1kHz)
 *   GET {base}/api/lyric?id=<songId>          (可选)
 *        → {"code":200,"data":{"lrc":"..."}}
 *
 * 播放链路: esp_http_client 分块读 → esp_audio_simple_dec(自动识别 MP3/AAC/FLAC)
 *           → 立体声降单声道 → speaker_write(I2S)
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* 初始化(建任务)。在 main 的 app_main 里调用一次 */
void music_init(void);

/* 按歌名搜索并播放(异步: 立刻返回, 搜索/播放都在后台任务里做)。
   成功返回 true 表示"已受理"(不代表已经出声)。 */
bool music_play(const char *song);

/* 直接按网易云歌曲 id 播放 */
bool music_play_id(const char *song_id);

void music_pause(void);
void music_resume(void);
void music_stop(void);
void music_next(void);          /* 播放搜索结果里的下一首 */

/* 是否正在播放/暂停中(占着喇叭) */
bool music_is_active(void);
/* 音乐是否正在独占喇叭/CPU(暂停时返回 false) —— AI 让路判定用这个, 不要用
   music_is_active(), 否则"暂停后喊小智没反应"。 */
bool music_owns_audio(void);
bool music_is_playing(void);

/* 状态字符串, 给 MCP / 网页 / 串口看 */
const char *music_status_str(void);

/* ---------- 网页控制台专用 ----------
 * 只搜索、不播放(异步): 结果留在内部歌单里, 之后用 music_results_json 取,
 * 网页点某一首时再调 music_play_id()。 */
bool music_search_only(const char *keyword);
/* 按歌手名搜歌: 搜到歌手 → 热门歌曲装进列表(不自动播, 网页点选) */
bool music_search_artist_only(const char *name);
bool music_search_pending(void);   /* 搜索还在进行中 */
/* 把当前曲目列表(搜索结果 或 已加载的歌单)导成 JSON:
   {"count":n,"idx":i,"playing":bool,"busy":bool,"src":"来源名","items":[...]} */
int  music_results_json(char *out, size_t n);

/* ---------- 我的歌单(网易云账号里自己的歌单) ----------
 * 请求与结果都是异步的: 先 request, 再轮询 ready, 好了就能取 JSON。
 * 之所以异步: 拉歌单要走 HTTP + 解析 JSON, 而 httpd 任务只有 4KB 栈,
 * 放那里会栈溢出 —— 所以实际工作交给音乐任务空闲时做。 */
void music_playlists_request(void);
bool music_playlists_ready(void);
int  music_playlists_json(char *out, size_t n);         /* {"ready":b,"count":n,"items":[{id,name,count}]} */
/* 把某个歌单的歌装进曲目列表(之后用 music_results_json 取、music_play_id 点播) */
void music_playlist_songs_request(const char *playlist_id);
bool music_playlist_ready(void);

/* 代理地址(默认为空, 需在网页「音乐」页设置; 见 docs/MUSIC_PROXY.md), 存 NVS, 可改 */
void        music_set_base(const char *url);
const char *music_get_base(void);
