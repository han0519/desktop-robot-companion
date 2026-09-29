/*
 * mcp.c — 小智 MCP 服务端实现
 *
 * 协议要点(对照官方 78/xiaozhi-esp32 的 mcp_server.cc):
 *   · 外层信封 { session_id, type:"mcp", payload }
 *   · payload 是标准 JSON-RPC 2.0; id 必须原样回填
 *   · initialize 的 result 里要有 protocolVersion / capabilities.tools / serverInfo
 *   · tools/list 的 result.tools[] 每项含 name/description/inputSchema
 *   · tools/call 成功回 result.content=[{type:"text",text:"..."}], isError:false
 *   · 未知工具/缺参数回顶层 error {code:-32602,...}
 *   · 未实现的方法回 error {code:-32601,...}
 *   · ping 的 result 必须是空对象 {}
 */
#include "mcp.h"
#include "config.h"
#include "light.h"
#include "xiaozhi.h"      /* ai_client_set_tts_voice: 播报音色(本地变调) */
#include "dht11.h"
#include "speaker_max98357a.h"
#include "servo.h"
#include "music.h"
#include "persona.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdint.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "mcp";

/* 设备信息: 会出现在 initialize 的 serverInfo 里 */
#define MCP_SERVER_NAME   "robot-companion"
#define MCP_SERVER_VER    "1.0.0"
#define MCP_PROTO_VER     "2024-11-05"

/* ---------------- 小工具 ---------------- */

static char *rpc_ok(cJSON *id, cJSON *result_obj, const char *session_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *rpc  = cJSON_CreateObject();
    cJSON_AddStringToObject(rpc, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(rpc, "id", cJSON_Duplicate(id, 1));
    cJSON_AddItemToObject(rpc, "result", result_obj);

    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON_AddItemToObject(root, "payload", rpc);
    if (session_id && session_id[0]) cJSON_AddStringToObject(root, "session_id", session_id);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static char *rpc_error(cJSON *id, int code, const char *msg, const char *session_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *rpc  = cJSON_CreateObject();
    cJSON *err  = cJSON_CreateObject();
    cJSON_AddStringToObject(rpc, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(rpc, "id", cJSON_Duplicate(id, 1));
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", msg);
    cJSON_AddItemToObject(rpc, "error", err);

    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON_AddItemToObject(root, "payload", rpc);
    if (session_id && session_id[0]) cJSON_AddStringToObject(root, "session_id", session_id);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

/* tools/call 的成功结果: content 数组里放一条 text */
static char *rpc_tool_text(cJSON *id, const char *text, bool is_error, const char *session_id)
{
    cJSON *result  = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *item    = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "");
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddBoolToObject(result, "isError", is_error);
    return rpc_ok(id, result, session_id);
}

/* 解析 "#RRGGBB" 或颜色名 */
static bool parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!s || !s[0]) return false;

    if (s[0] == '#') s++;
    /* ★ 6 位必须【全部】是十六进制: 只查首尾的话 "12G45Z" 会被接受,
       非法位经 strtol 静默变 0(比如绿色分量悄悄变 0) */
    if (strlen(s) == 6 &&
        isxdigit((unsigned char)s[0]) && isxdigit((unsigned char)s[1]) &&
        isxdigit((unsigned char)s[2]) && isxdigit((unsigned char)s[3]) &&
        isxdigit((unsigned char)s[4]) && isxdigit((unsigned char)s[5])) {
        char buf[3] = {0};
        buf[0] = s[0]; buf[1] = s[1]; *r = (uint8_t)strtol(buf, NULL, 16);
        buf[0] = s[2]; buf[1] = s[3]; *g = (uint8_t)strtol(buf, NULL, 16);
        buf[0] = s[4]; buf[1] = s[5]; *b = (uint8_t)strtol(buf, NULL, 16);
        return true;
    }

    struct { const char *n; uint8_t r, g, b; } tbl[] = {
        { "红",     255,   0,   0 }, { "red",    255,   0,   0 },
        { "绿",       0, 255,   0 }, { "green",    0, 255,   0 },
        { "蓝",       0,   0, 255 }, { "blue",     0,   0, 255 },
        { "黄",     255, 255,   0 }, { "yellow", 255, 255,   0 },
        { "紫",     160,   0, 255 }, { "purple", 160,   0, 255 },
        { "青",       0, 255, 255 }, { "cyan",     0, 255, 255 },
        { "白",     255, 255, 255 }, { "white",  255, 255, 255 },
        { "橙",     255, 128,   0 }, { "orange", 255, 128,   0 },
        { "粉",     255, 105, 180 }, { "pink",   255, 105, 180 },
        { "暖白",   255, 200, 120 }, { "warm",   255, 200, 120 },
    };
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (strcasecmp(s, tbl[i].n) == 0) {
            *r = tbl[i].r; *g = tbl[i].g; *b = tbl[i].b;
            return true;
        }
    }
    return false;
}

/* ---------------- 工具表 ---------------- */

typedef struct {
    const char *name;
    const char *desc;
    const char *schema;     /* JSON Schema 文本 */
} tool_def_t;

static const tool_def_t TOOLS[] = {
{
    "self.light.set_effect",
    "Set the LED ring light effect (8-LED ring). Effects: 'off'(关灯), "
    "'mono'(单色常亮), 'breathe'(单色呼吸), 'rainbow'(彩虹环), "
    "'rainbow_breath'(彩虹呼吸), 'chase'(追光), 'twin'(双点对撞), "
    "'mirror'(镜像呼吸), 'pulse'(脉冲扩散), 'fire'(火焰), 'starry'(星空闪烁), "
    "'level'(电平环), 'music'(音乐律动,放歌时自动进), 'music_bands'(音乐频谱), "
    "'police'(警车爆闪).",
    "{\"type\":\"object\",\"properties\":{\"effect\":{\"type\":\"string\","
    "\"description\":\"off | mono | breathe | rainbow | rainbow_breath | chase | "
    "twin | mirror | pulse | fire | starry | level | music | music_bands | police, "
    "也接受中文如 呼吸/彩虹/追光/火焰/星空/律动/频谱/爆闪/关灯\"}},"
    "\"required\":[\"effect\"]}"
},
{
    "self.light.next_effect",
    "Switch to the next LED light effect. 切换到下一个灯效。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    "self.persona.mood",
    "Ask about the robot's current inner state: mood (valence), energy, boredom and "
    "intimacy. Call this when you want your reply to match how the robot currently "
    "feels, e.g. before saying something emotional. 查询机器人当前的心情/精力/无聊/亲密度, "
    "让回复更贴合它现在的状态。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    "self.light.set_brightness",
    "Set the LED ring brightness, 0-100. 设置灯光亮度。",
    "{\"type\":\"object\",\"properties\":{\"brightness\":"
    "{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
    "\"required\":[\"brightness\"]}"
},
{
    "self.light.set_color",
    "Set the LED color (and switch to solid color mode). Use a color name like "
    "red/green/blue/黄/紫 or a hex string like '#FF8000'. 设置灯光颜色。",
    "{\"type\":\"object\",\"properties\":{\"color\":{\"type\":\"string\"}},"
    "\"required\":[\"color\"]}"
},
{
    "self.light.get_state",
    "Get the current LED effect and brightness. 查询当前灯光状态。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    "self.sensor.get_temperature_humidity",
    "Read the ambient temperature (Celsius) and humidity (percent) from the "
    "on-board DHT11 sensor. 读取当前环境温度和湿度。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    /* 工具名与官方固件保持一致(self.audio_speaker.set_volume), 服务端/模型更好识别 */
    "self.audio_speaker.set_volume",
    "Set the volume of the audio speaker, 0-100. 设置音量大小。",
    "{\"type\":\"object\",\"properties\":{\"volume\":"
    "{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},"
    "\"required\":[\"volume\"]}"
},
{
    "self.audio_speaker.get_volume",
    "Get the current speaker volume, 0-100. 查询当前音量。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    /* ★ 本地音色: 服务端没有换声消息, 这是设备端对播报做的实时 DSP 变声 */
    "self.audio_voice.set",
    "Set the TTS speaking voice (device-side DSP voice changer). Voices: "
    "'original'(原声), 'metal'(机械金属), 'deep'(电子低沉), 'alien'(赛博外星), "
    "'elec'(电流音), 'strong'(强电流), 'radio'(对讲机), 'space'(深空回声), "
    "'elec2'(电流音2). 切换说话的音色。",
    "{\"type\":\"object\",\"properties\":{\"voice\":"
    "{\"type\":\"string\",\"description\":\"original | metal | deep | alien | "
    "elec | strong | radio | space | elec2, 也接受中文: 原声/机械金属/电子低沉/"
    "赛博外星/电流音/强电流/对讲机/深空回声/电流音2, 或 0~8\"}},"
    "\"required\":[\"voice\"]}"
},
{
    "self.audio_voice.get",
    "Get the current TTS voice. 查询当前音色。",
    "{\"type\":\"object\",\"properties\":{}}"
},
/* ---------------- 舵机: 让 AI 能"转头看" ----------------
 * 之前一个舵机工具都没有, 所以"根据语言控制左右转头"根本无从触发 ——
 * AI 想调用也找不到工具。这里按跟手追踪的角度约定补齐:
 *   0 = 最左, 90 = 正前, 180 = 最右 (跟手用的是 pan ∈ [48,132]) */
{
    "self.servo.set_pan",
    "Turn the head left/right. angle: 0 = far left, 90 = straight ahead, "
    "180 = far right. 左右转头: 0=最左, 90=正前, 180=最右。",
    "{\"type\":\"object\",\"properties\":{\"angle\":{\"type\":\"integer\","
    "\"minimum\":0,\"maximum\":180,\"description\":\"0=最左, 90=正前, 180=最右\"}},"
    "\"required\":[\"angle\"]}"
},
{
    "self.servo.set_tilt",
    "Tilt the head (mechanical limit: 85-95 only). 上下点头, 只有 85~95 度可用。",
    "{\"type\":\"object\",\"properties\":{\"angle\":{\"type\":\"integer\","
    "\"minimum\":85,\"maximum\":95}},"
    "\"required\":[\"angle\"]}"
},
{
    "self.servo.look_left",
    "Turn the head to the left. 把头转向左边。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    "self.servo.look_right",
    "Turn the head to the right. 把头转向右边。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    "self.servo.look_center",
    "Turn the head back to the front. 把头转回正前方。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    "self.servo.nod",
    "Nod the head (means yes / agree). 点头, 表示同意。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    "self.servo.shake_head",
    "Shake the head (means no / disagree). 摇头, 表示否定。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    "self.servo.get_state",
    "Get the current head angles in degrees. 查询头部当前角度。",
    "{\"type\":\"object\",\"properties\":{}}"
},
/* ---------------- 网易云音乐 ---------------- */
{
    "self.music.play",
    "Search a song by name on NetEase Cloud Music and start playing it. "
    "按歌名搜索并播放网易云音乐, 例: '播放周杰伦的晴天'。",
    "{\"type\":\"object\",\"properties\":{\"song\":{\"type\":\"string\","
    "\"description\":\"歌名, 可带歌手, 例: 晴天 或 周杰伦 晴天\"}},"
    "\"required\":[\"song\"]}"
},
{
    "self.music.pause",
    "Pause the music. 暂停播放。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    "self.music.resume",
    "Resume the music. 继续播放。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    "self.music.stop",
    "Stop the music. 停止播放(关掉音乐)。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    "self.music.next",
    "Play the next song from the search results. 播放下一首。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    "self.music.status",
    "Get current music playback status. 查询当前音乐播放状态。",
    "{\"type\":\"object\",\"properties\":{}}"
},
{
    "self.music.set_server",
    "Set the NetEase proxy server base URL (e.g. http://192.168.1.5:3000). "
    "设置网易云代理服务器地址。",
    "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},"
    "\"required\":[\"url\"]}"
},
};

/* AI 转头速度与摆幅(与跟手追踪保持一致) */
#define SERVO_AI_SPEED_DPS  160
#define SERVO_AI_PAN_CENTER 90
#define SERVO_AI_PAN_SWING  42

/* ★ MCP 工具是在 WebSocket 接收回调里执行的, 绝不能在里面阻塞 ——
 *   servo_nod_head()/servo_shake_head() 内部会阻塞几百毫秒(等舵机到位),
 *   直接调用会把音频收发整个卡住。所以丢给一个一次性任务去跑。 */
static volatile int s_servo_anim = 0;      /* 0=空闲, 1=点头中, 2=摇头中 */

static void servo_anim_task(void *arg)
{
    int kind = (int)(intptr_t)arg;
    if (kind == 1) servo_nod_head();
    else           servo_shake_head();
    s_servo_anim = 0;
    vTaskDelete(NULL);
}

static bool servo_start_anim(int kind)
{
    if (s_servo_anim) return false;        /* 一次只跑一个动作 */
    s_servo_anim = kind;
    if (xTaskCreate(servo_anim_task, "sv_anim", 3072,
                    (void *)(intptr_t)kind, 4, NULL) != pdPASS) {
        s_servo_anim = 0;
        return false;
    }
    return true;
}

#define TOOL_COUNT (sizeof(TOOLS) / sizeof(TOOLS[0]))

static char *build_tools_list(cJSON *id, const char *session_id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *arr    = cJSON_CreateArray();

    for (size_t i = 0; i < TOOL_COUNT; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", TOOLS[i].name);
        cJSON_AddStringToObject(t, "description", TOOLS[i].desc);
        cJSON *schema = cJSON_Parse(TOOLS[i].schema);
        if (schema) cJSON_AddItemToObject(t, "inputSchema", schema);
        cJSON_AddItemToArray(arr, t);
    }
    cJSON_AddItemToObject(result, "tools", arr);
    return rpc_ok(id, result, session_id);
}

/* ---------------- 工具执行 ---------------- */

/* 返回给模型的文本; 需要报错时 is_err=true */
static const char *exec_tool(const char *name, cJSON *args, bool *is_err)
{
    static char out[192];
    *is_err = false;

    if (strcmp(name, "self.light.set_effect") == 0) {
        cJSON *e = args ? cJSON_GetObjectItem(args, "effect") : NULL;
        if (!e || !cJSON_IsString(e)) { *is_err = true; return "missing 'effect'"; }
        light_effect_t ef;
        if (!light_effect_from_name(e->valuestring, &ef)) {
            *is_err = true;
            snprintf(out, sizeof(out), "unknown effect '%s'", e->valuestring);
            return out;
        }
        light_set_effect(ef);
        snprintf(out, sizeof(out), "灯效已切换为 %s (%s)",
                 light_effect_cn(ef), light_effect_name(ef));
        return out;
    }

    if (strcmp(name, "self.light.next_effect") == 0) {
        light_next_effect();
        snprintf(out, sizeof(out), "灯效已切换为 %s", light_effect_cn(light_get_effect()));
        return out;
    }

    if (strcmp(name, "self.light.set_brightness") == 0) {
        cJSON *b = args ? cJSON_GetObjectItem(args, "brightness") : NULL;
        if (!b || !cJSON_IsNumber(b)) { *is_err = true; return "missing 'brightness'"; }
        light_set_brightness(b->valueint);
        snprintf(out, sizeof(out), "亮度已设为 %d%%", light_get_brightness());
        return out;
    }

    if (strcmp(name, "self.light.set_color") == 0) {
        cJSON *c = args ? cJSON_GetObjectItem(args, "color") : NULL;
        if (!c || !cJSON_IsString(c)) { *is_err = true; return "missing 'color'"; }
        uint8_t r, g, b;
        if (!parse_color(c->valuestring, &r, &g, &b)) {
            *is_err = true;
            snprintf(out, sizeof(out), "unknown color '%s'", c->valuestring);
            return out;
        }
        light_set_color(r, g, b);
        snprintf(out, sizeof(out), "颜色已设为 (%u,%u,%u)", r, g, b);
        return out;
    }

    if (strcmp(name, "self.light.get_state") == 0) {
        uint8_t r, g, b;
        light_get_color(&r, &g, &b);
        snprintf(out, sizeof(out), "当前灯效 %s, 亮度 %d%%, 主色 (%u,%u,%u)",
                 light_effect_cn(light_get_effect()), light_get_brightness(), r, g, b);
        return out;
    }

    if (strcmp(name, "self.sensor.get_temperature_humidity") == 0) {
        dht11_data_t d;
        if (!dht11_read(&d)) {
            *is_err = true;
            return "DHT11 读取失败(可能是上电首次读取或接线问题), 请稍后再试";
        }
        /* 给模型的文本里带上完整读数, 让它直接念出来 */
        snprintf(out, sizeof(out),
                 "当前温度 %.1f 摄氏度, 湿度 %.1f%%",
                 d.temperature, d.humidity);
        return out;
    }

    if (strcmp(name, "self.audio_speaker.set_volume") == 0) {
        cJSON *v = args ? cJSON_GetObjectItem(args, "volume") : NULL;
        if (!v || !cJSON_IsNumber(v)) { *is_err = true; return "missing 'volume'"; }
        speaker_set_volume(v->valueint);
        snprintf(out, sizeof(out), "音量已设为 %d%%", speaker_get_volume());
        return out;
    }

    if (strcmp(name, "self.audio_speaker.get_volume") == 0) {
        snprintf(out, sizeof(out), "当前音量 %d%%", speaker_get_volume());
        return out;
    }

    /* ---------------- 播报音色(本地变调) ---------------- */
    if (strcmp(name, "self.audio_voice.set") == 0) {
        cJSON *v = args ? cJSON_GetObjectItem(args, "voice") : NULL;
        if (!v || !cJSON_IsString(v)) { *is_err = true; return "missing 'voice'"; }
        if (!ai_client_set_tts_voice(v->valuestring)) {
            *is_err = true;
            snprintf(out, sizeof(out), "unknown voice '%s' (normal/cute/uncle/robot)",
                     v->valuestring);
            return out;
        }
        snprintf(out, sizeof(out), "音色已切换: %s", ai_client_tts_voice_cn());
        return out;
    }
    if (strcmp(name, "self.audio_voice.get") == 0) {
        snprintf(out, sizeof(out), "当前音色: %s", ai_client_tts_voice_cn());
        return out;
    }

    /* ---------------- 舵机 ---------------- */
    if (strcmp(name, "self.servo.set_pan") == 0) {
        cJSON *a = args ? cJSON_GetObjectItem(args, "angle") : NULL;
        if (!a || !cJSON_IsNumber(a)) { *is_err = true; return "missing 'angle'"; }
        int ang = a->valueint;
        if (ang < 0) ang = 0;
        if (ang > 180) ang = 180;
        /* 非阻塞: 只设目标 + 限速, 由后台伺服任务插值 */
        servo_set_target(SERVO_PAN, ang, SERVO_AI_SPEED_DPS);
        snprintf(out, sizeof(out), "头已转到 %d 度 (0=最左, 90=正前, 180=最右)", ang);
        return out;
    }

    if (strcmp(name, "self.servo.set_tilt") == 0) {
        cJSON *a = args ? cJSON_GetObjectItem(args, "angle") : NULL;
        if (!a || !cJSON_IsNumber(a)) { *is_err = true; return "missing 'angle'"; }
        int ang = a->valueint;
        if (ang < SERVO_TILT_PHYS_MIN) ang = SERVO_TILT_PHYS_MIN;
        if (ang > SERVO_TILT_PHYS_MAX) ang = SERVO_TILT_PHYS_MAX;
        servo_set_target(SERVO_TILT, ang, SERVO_AI_SPEED_DPS);
        snprintf(out, sizeof(out), "抬头/低头已设为 %d 度", ang);
        return out;
    }

    if (strcmp(name, "self.servo.look_left") == 0 ||
        strcmp(name, "self.servo.look_right") == 0 ||
        strcmp(name, "self.servo.look_center") == 0) {
        int ang = SERVO_AI_PAN_CENTER;
        const char *where = "正前方";
        if (strstr(name, "left"))  { ang = SERVO_AI_PAN_CENTER - SERVO_AI_PAN_SWING; where = "左边"; }
        if (strstr(name, "right")) { ang = SERVO_AI_PAN_CENTER + SERVO_AI_PAN_SWING; where = "右边"; }
        servo_set_target(SERVO_PAN, ang, SERVO_AI_SPEED_DPS);
        snprintf(out, sizeof(out), "头已转向%s", where);
        return out;
    }

    if (strcmp(name, "self.servo.nod") == 0) {
        if (!servo_start_anim(1)) { *is_err = true; return "正在做别的动作, 稍后再试"; }
        return "正在点头";
    }

    if (strcmp(name, "self.servo.shake_head") == 0) {
        if (!servo_start_anim(2)) { *is_err = true; return "正在做别的动作, 稍后再试"; }
        return "正在摇头";
    }

    if (strcmp(name, "self.servo.get_state") == 0) {
        snprintf(out, sizeof(out), "左右 %d 度 (90=正前), 上下 %d 度",
                 servo_get_angle(SERVO_PAN), servo_get_angle(SERVO_TILT));
        return out;
    }

    /* ---------------- 人格: 让 AI 知道"我现在是什么心情" ---------------- */
    if (strcmp(name, "self.persona.mood") == 0) {
        const persona_mood_t *m = persona_get();
        snprintf(out, sizeof(out), "%s (心情值 %d, 精力 %d, 无聊 %d, 亲密度 %d)",
                 persona_mood_str(),
                 (int)m->valence, (int)m->energy, (int)m->boredom, (int)m->intimacy);
        return out;
    }

    /* ---------------- 网易云音乐 ----------------
       注意: music_play() 是异步的(搜索+播放都在 music 任务里),
       这里立刻返回, 不要让 MCP 回调等 —— 它跑在 WebSocket 接收回调里,
       阻塞会把音频收发卡住。 */
    if (strcmp(name, "self.music.play") == 0) {
        cJSON *s = args ? cJSON_GetObjectItem(args, "song") : NULL;
        if (!s || !cJSON_IsString(s) || !s->valuestring[0]) {
            *is_err = true; return "missing 'song'";
        }
        if (!music_play(s->valuestring)) { *is_err = true; return "播放请求失败"; }
        /* ★ 不能对用户保证"已经在播放": 网易云大量歌曲是 VIP/版权受限,
           代理会返回 404 拿不到播放链接(实测《孤勇者》就是)。
           固件会自动改试搜索结果里的其他版本, 所以只能说"正在搜索并尝试" ——
           否则 AI 会笃定地告诉用户"正在播放", 而实际一点声音都没有。 */
        snprintf(out, sizeof(out),
                 "正在搜索「%s」并尝试播放(若该版本无版权会自动换其他版本, 结果稍后见串口日志)",
                 s->valuestring);
        return out;
    }

    if (strcmp(name, "self.music.pause") == 0)  { music_pause();  return "已暂停音乐"; }
    if (strcmp(name, "self.music.resume") == 0) { music_resume(); return "已继续播放"; }
    if (strcmp(name, "self.music.stop") == 0)   { music_stop();   return "已停止音乐"; }
    if (strcmp(name, "self.music.next") == 0)   { music_next();   return "已切到下一首"; }

    if (strcmp(name, "self.music.status") == 0) {
        snprintf(out, sizeof(out), "%s (代理 %s)", music_status_str(), music_get_base());
        return out;
    }

    if (strcmp(name, "self.music.set_server") == 0) {
        cJSON *u = args ? cJSON_GetObjectItem(args, "url") : NULL;
        if (!u || !cJSON_IsString(u) || !u->valuestring[0]) {
            *is_err = true; return "missing 'url'";
        }
        music_set_base(u->valuestring);
        snprintf(out, sizeof(out), "网易云代理已设为 %s", music_get_base());
        return out;
    }

    *is_err = true;
    snprintf(out, sizeof(out), "unknown tool: %s", name);
    return out;
}

/* ---------------- 入口 ---------------- */

char *mcp_handle_payload(const char *payload_json, const char *session_id)
{
    /* ★ cJSON_Parse 内部会对入参做 strlen(NULL) —— 直接崩。调用方确实可能传空。 */
    if (!payload_json) {
        ESP_LOGW(TAG, "payload 为空");
        return rpc_error(NULL, -32700, "parse error", session_id);
    }
    cJSON *req = cJSON_Parse(payload_json);
    if (!req) {
        ESP_LOGW(TAG, "payload 不是合法 JSON");
        return rpc_error(NULL, -32700, "parse error", session_id);
    }

    cJSON *id = cJSON_GetObjectItem(req, "id");
    cJSON *m  = cJSON_GetObjectItem(req, "method");
    const char *method = (m && cJSON_IsString(m)) ? m->valuestring : "";

    /* 把收到的请求原样打一条(截断), 排查"工具没被调用"时最有用 */
    ESP_LOGI(TAG, "MCP <- %s | %.180s", method, payload_json ? payload_json : "");

    char *resp = NULL;

    /* 通知类没有 id, 不需要回复 */
    if (strncmp(method, "notifications/", 14) == 0) {
        cJSON_Delete(req);
        return NULL;
    }

    /* ping: result 必须是空对象 */
    if (strcmp(method, "ping") == 0) {
        resp = rpc_ok(id, cJSON_CreateObject(), session_id);
        cJSON_Delete(req);
        return resp;
    }

    if (strcmp(method, "initialize") == 0) {
        /* ★ 原样回填服务端提议的 protocolVersion。
           固定回 "2024-11-05" 会让协商落到旧版本, 有些服务端据此就不再下发
           工具调用 —— 表现就是"AI 能聊天但 MCP 工具永远调不起来"。 */
        cJSON *params = cJSON_GetObjectItem(req, "params");
        cJSON *pv = params ? cJSON_GetObjectItem(params, "protocolVersion") : NULL;
        const char *proto = (pv && cJSON_IsString(pv) && pv->valuestring[0])
                          ? pv->valuestring : MCP_PROTO_VER;

        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "protocolVersion", proto);
        ESP_LOGI(TAG, "initialize: 协商版本 %s", proto);
        cJSON *caps  = cJSON_AddObjectToObject(result, "capabilities");
        cJSON_AddObjectToObject(caps, "tools");          /* 工具详情走 tools/list */
        cJSON *info  = cJSON_AddObjectToObject(result, "serverInfo");
        cJSON_AddStringToObject(info, "name", MCP_SERVER_NAME);
        cJSON_AddStringToObject(info, "version", MCP_SERVER_VER);
        cJSON_AddStringToObject(result, "instructions",
                                "桌面机器人: 可控制灯环灯效/亮度/颜色, 可读温湿度, "
                                "可控制头部左右转头/上下点头/点头/摇头, "
                                "可播放/暂停/停止网易云音乐。");
        resp = rpc_ok(id, result, session_id);
        cJSON_Delete(req);
        ESP_LOGI(TAG, "MCP -> initialize 已回复");
        return resp;
    }

    if (strcmp(method, "tools/list") == 0) {
        resp = build_tools_list(id, session_id);
        cJSON_Delete(req);
        ESP_LOGI(TAG, "MCP -> tools/list (%u 个工具)", (unsigned)TOOL_COUNT);
        /* 把工具清单原文打出来(几 KB, 每次连接都打会拖慢日志 —— 要看时开 DEBUG) */
        ESP_LOGD(TAG, "tools/list 原文: %s", resp ? resp : "(null)");
        return resp;
    }

    if (strcmp(method, "tools/call") == 0) {
        cJSON *params = cJSON_GetObjectItem(req, "params");
        cJSON *nm     = params ? cJSON_GetObjectItem(params, "name") : NULL;
        cJSON *args   = params ? cJSON_GetObjectItem(params, "arguments") : NULL;
        if (!nm || !cJSON_IsString(nm)) {
            resp = rpc_error(id, -32602, "missing params.name", session_id);
            cJSON_Delete(req);
            return resp;
        }
        bool is_err = false;
        /* ★ exec_tool 用 static 缓冲装返回文本: 两个调用方 —— WS 回调任务
           (AI 工具调用)和 httpd 任务(网页 /mcp) —— 并发进来会互相覆盖,
           AI 和网页各自收到两句话的混合体。串行化(工具本身可能阻塞几秒,
           串行正是我们想要的语义)。静态创建 mutex, 不依赖初始化顺序。 */
        static StaticSemaphore_t s_tool_mux_mem;
        static SemaphoreHandle_t s_tool_mux;
        if (!s_tool_mux) s_tool_mux = xSemaphoreCreateMutexStatic(&s_tool_mux_mem);
        xSemaphoreTake(s_tool_mux, portMAX_DELAY);
        const char *text = exec_tool(nm->valuestring, args, &is_err);
        ESP_LOGI(TAG, "MCP -> tools/call %s => %s", nm->valuestring, text);
        resp = rpc_tool_text(id, text, is_err, session_id);   /* text 已拷入 resp */
        xSemaphoreGive(s_tool_mux);
        cJSON_Delete(req);
        return resp;
    }

    /* 其余方法(资源/提示等)本设备不支持 —— 必须按协议回 -32601 */
    ESP_LOGW(TAG, "MCP 未实现的方法: %s", method);
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "Method not found: %s", method);
        resp = rpc_error(id, -32601, msg, session_id);
    }
    cJSON_Delete(req);
    return resp;
}
