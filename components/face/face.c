/**
 * @file face.c
 * @brief 机器人表情系统 v3 (SSD1305 128×64 单色)
 *
 * 渲染管线:
 *   1. 清空 8bit luma 缓冲 (128×64)
 *   2. SDF 光栅化各图元, 覆盖率抗锯齿, 直接写入 luma (max/min 合成)
 *   3. 4×4 Bayer 抖动 → 1bit 显存
 *   4. 与当前显存逐字节比对, 只把有变化的页通过 I2C 发出去
 *
 * 参数全部沿用 240×240 设计坐标系, KP 为设计单位→像素的缩放。
 */
#include "face.h"
#include "ssd1305.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "face";

#define OLED_W   128
#define OLED_H   64
#define KP       0.62f
#define PI       3.14159265f
#define TAU      (2.0f * PI)

/* 眉毛总开关: 0 = 不画眉毛(当前), 1 = 恢复眉毛
 * 表情表里的 bL/bR 参数一直保留, 改这个宏即可原样恢复 */
#define FACE_DRAW_BROWS  0

/* ---------- 锚点(像素) ----------
 * 无眉毛后整张脸略微上移 2.5px, 避免顶部留大片空白 */
#define EYE_CX0  42.0f
#define EYE_CX1  86.0f
#define EYE_CY   25.5f
#define MTH_CX   64.0f
#define MTH_CY   52.0f
#define TILT_CX  64.0f
#define TILT_CY  38.8f

/* ---------- 装饰件锚点(设计单位) ---------- */
#define A_TRX    171.0f
#define A_TRY    26.0f
/* 右侧装饰锚点: 原来 190, 声波(5根柱)会排到 x>128 出界, 收到 172 */
#define A_RX     172.0f
#define A_RY     50.0f
#define A_SPX    103.0f
#define A_SPY    24.0f
#define A_SPR    21.0f
#define A_WAX    182.0f
#define A_WAY    46.0f
#define A_SZ     0.85f
#define A_SY     0.55f

/* ======================= luma 缓冲 ======================= */
static uint8_t s_luma[OLED_W * OLED_H];

static inline void lw(int x, int y, float cov, float sc)   /* 画白 */
{
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    if (cov <= 0.0f) return;
    if (cov > 1.0f) cov = 1.0f;
    int v = (int)(cov * sc * 255.0f);
    if (v > 255) v = 255;
    uint8_t *p = &s_luma[y * OLED_W + x];
    if (v > *p) *p = (uint8_t)v;
}

static inline void lb(int x, int y, float cov, float sc)   /* 画黑(挖掉) */
{
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    if (cov <= 0.0f) return;
    if (cov > 1.0f) cov = 1.0f;
    int v = (int)((1.0f - cov * sc) * 255.0f);
    if (v < 0) v = 0;
    uint8_t *p = &s_luma[y * OLED_W + x];
    if (v < *p) *p = (uint8_t)v;
}

/* ======================= SDF ======================= */
static float sd_rr4(float px, float py, float x, float y, float w, float h,
                    float rtl, float rtr, float rbr, float rbl)
{
    float cx = x + w * 0.5f, cy = y + h * 0.5f, r;
    if (py < cy) r = (px < cx) ? rtl : rtr;
    else         r = (px < cx) ? rbl : rbr;
    float rmax = (w < h ? w : h) * 0.5f;
    if (r > rmax) r = rmax;
    if (r < 0.0f) r = 0.0f;
    float qx = fabsf(px - cx) - (w * 0.5f - r);
    float qy = fabsf(py - cy) - (h * 0.5f - r);
    float ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0;
    float mn = qx > qy ? qx : qy;
    if (mn > 0) mn = 0;
    return sqrtf(ax * ax + ay * ay) + mn - r;
}

static float sd_ell(float px, float py, float cx, float cy, float rx, float ry)
{
    if (rx < 0.01f || ry < 0.01f) return 1e6f;
    float dx = (px - cx) / rx, dy = (py - cy) / ry;
    return (sqrtf(dx * dx + dy * dy) - 1.0f) * (rx < ry ? rx : ry);
}

static float sd_seg(float px, float py, float x0, float y0, float x1, float y1)
{
    float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy, t = 0.0f;
    if (l2 > 0.0f) { t = ((px - x0) * dx + (py - y0) * dy) / l2; t = t < 0 ? 0 : (t > 1 ? 1 : t); }
    float ax = px - (x0 + t * dx), ay = py - (y0 + t * dy);
    return sqrtf(ax * ax + ay * ay);
}

static float sd_poly_dist(float px, float py, const float *P, int n)
{
    float d = 1e6f;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float t = sd_seg(px, py, P[j * 2], P[j * 2 + 1], P[i * 2], P[i * 2 + 1]);
        if (t < d) d = t;
    }
    return d;
}

static float sd_poly(float px, float py, const float *P, int n)
{
    int in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float xi = P[i * 2], yi = P[i * 2 + 1], xj = P[j * 2], yj = P[j * 2 + 1];
        if (((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) in = !in;
    }
    float d = sd_poly_dist(px, py, P, n);
    return in ? -d : d;
}

static float sd_heart(float px, float py, float cx, float cy, float s)
{
    float r = 0.46f * s;
    float d1 = sd_ell(px, py, cx - 0.36f * s, cy - 0.34f * s, r, r);
    float d2 = sd_ell(px, py, cx + 0.36f * s, cy - 0.34f * s, r, r);
    float tri[6] = { cx - 0.82f * s, cy - 0.14f * s, cx + 0.82f * s, cy - 0.14f * s, cx, cy + 0.82f * s };
    float d3 = sd_poly(px, py, tri, 3);
    float m = d1 < d2 ? d1 : d2;
    return m < d3 ? m : d3;
}

/* ======================= 光栅循环 ======================= */
#define RAST(SDF)                                                                  \
    int ix0 = (int)floorf(bx0), iy0 = (int)floorf(by0);                            \
    int ix1 = (int)ceilf(bx1), iy1 = (int)ceilf(by1);                              \
    if (ix0 < 0) ix0 = 0;                                                          \
    if (iy0 < 0) iy0 = 0;                                                          \
    if (ix1 > OLED_W - 1) ix1 = OLED_W - 1;                                        \
    if (iy1 > OLED_H - 1) iy1 = OLED_H - 1;                                        \
    if (ix1 < ix0 || iy1 < iy0) return;                                            \
    float ca = 1.0f, sa = 0.0f;                                                    \
    if (rot != 0.0f) { float a = -rot * PI / 180.0f; ca = cosf(a); sa = sinf(a); } \
    for (int py = iy0; py <= iy1; py++) {                                          \
        float sy = (float)py + 0.5f;                                               \
        for (int px = ix0; px <= ix1; px++) {                                      \
            float sx = (float)px + 0.5f;                                           \
            float qx = sx, qy = sy;                                                \
            if (rot != 0.0f) {                                                     \
                float ddx = sx - rcx, ddy = sy - rcy;                              \
                qx = rcx + ddx * ca - ddy * sa;                                    \
                qy = rcy + ddx * sa + ddy * ca;                                    \
            }                                                                      \
            float cov = 0.5f - (SDF);                                              \
            if (cov > 0.0f) {                                                      \
                if (dark) lb(px, py, cov, sc); else lw(px, py, cov, sc);           \
            }                                                                      \
        }                                                                          \
    }

static void ras_rr4(float x, float y, float w, float h, float rtl, float rtr,
                    float rbr, float rbl, float rot, float rcx, float rcy,
                    bool dark, float sc)
{
    if (w <= 0 || h <= 0) return;

    /* 包围盒必须按旋转后的四个角来算, 否则旋转形状会被裁掉一块 */
    float bx0 = x, by0 = y, bx1 = x + w, by1 = y + h;
    if (rot != 0.0f) {
        float ca2 = cosf(rot * PI / 180.0f), sa2 = sinf(rot * PI / 180.0f);
        float cxs[4] = { x, x + w, x + w, x };
        float cys[4] = { y, y, y + h, y + h };
        bx0 = by0 = 1e6f; bx1 = by1 = -1e6f;
        for (int i = 0; i < 4; i++) {
            float px = cxs[i] - rcx, py = cys[i] - rcy;
            float qx2 = rcx + px * ca2 - py * sa2;
            float qy2 = rcy + px * sa2 + py * ca2;
            if (qx2 < bx0) bx0 = qx2;
            if (qx2 > bx1) bx1 = qx2;
            if (qy2 < by0) by0 = qy2;
            if (qy2 > by1) by1 = qy2;
        }
    }
    RAST(sd_rr4(qx, qy, x, y, w, h, rtl, rtr, rbr, rbl))
}

static void ras_ell(float cx, float cy, float rx, float ry, bool dark, float sc)
{
    if (rx <= 0 || ry <= 0) return;
    float bx0 = cx - rx, by0 = cy - ry, bx1 = cx + rx, by1 = cy + ry;
    float rot = 0, rcx = 0, rcy = 0;
    RAST(sd_ell(qx, qy, cx, cy, rx, ry))
}

static void ras_stroke(const float *P, int n, float th, bool dark, float sc)
{
    if (n < 2) return;
    float half = th * 0.5f;
    float bx0 = P[0], by0 = P[1], bx1 = P[0], by1 = P[1];
    for (int i = 1; i < n; i++) {
        if (P[i * 2] < bx0) bx0 = P[i * 2];
        if (P[i * 2] > bx1) bx1 = P[i * 2];
        if (P[i * 2 + 1] < by0) by0 = P[i * 2 + 1];
        if (P[i * 2 + 1] > by1) by1 = P[i * 2 + 1];
    }
    bx0 -= half + 2; by0 -= half + 2; bx1 += half + 2; by1 += half + 2;
    float rot = 0, rcx = 0, rcy = 0;
    RAST(sd_poly_dist(qx, qy, P, n) - half)
}

static void ras_polyfill(const float *P, int n, bool dark, float sc)
{
    if (n < 3) return;
    float bx0 = P[0], by0 = P[1], bx1 = P[0], by1 = P[1];
    for (int i = 1; i < n; i++) {
        if (P[i * 2] < bx0) bx0 = P[i * 2];
        if (P[i * 2] > bx1) bx1 = P[i * 2];
        if (P[i * 2 + 1] < by0) by0 = P[i * 2 + 1];
        if (P[i * 2 + 1] > by1) by1 = P[i * 2 + 1];
    }
    bx0 -= 2; by0 -= 2; bx1 += 2; by1 += 2;
    float rot = 0, rcx = 0, rcy = 0;
    RAST(sd_poly(qx, qy, P, n))
}

static void ras_heart(float cx, float cy, float s, bool dark, float sc)
{
    float bx0 = cx - s, by0 = cy - s, bx1 = cx + s * 1.1f, by1 = cy + s;
    float rot = 0, rcx = 0, rcy = 0;
    RAST(sd_heart(qx, qy, cx, cy, s))
}

static void ras_star(float cx, float cy, float ro, float ri, bool dark, float sc)
{
    float P[20];
    for (int i = 0; i < 10; i++) {
        float r = (i & 1) ? ri : ro, a = -PI * 0.5f + i * PI / 5.0f;
        P[i * 2] = cx + cosf(a) * r;
        P[i * 2 + 1] = cy + sinf(a) * r;
    }
    ras_polyfill(P, 10, dark, sc);
}

static void ras_line(float x0, float y0, float x1, float y1, float th, bool dark, float sc)
{
    float P[4] = { x0, y0, x1, y1 };
    ras_stroke(P, 2, th, dark, sc);
}

static void ras_arcst(float cx, float cy, float r, float a0, float a1, float th, bool dark, float sc)
{
    float P[46];
    int n = 22;
    for (int i = 0; i <= n; i++) {
        float a = a0 + (a1 - a0) * i / (float)n;
        P[i * 2] = cx + cosf(a) * r;
        P[i * 2 + 1] = cy + sinf(a) * r;
    }
    ras_stroke(P, n + 1, th, dark, sc);
}

/* 二次贝塞尔采样, 写入 pts(需 2*(n+1) 个 float) */
static void quad(float *pts, float p0x, float p0y, float p1x, float p1y, float p2x, float p2y, int n)
{
    for (int i = 0; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.0f - t;
        pts[i * 2]     = u * u * p0x + 2 * u * t * p1x + t * t * p2x;
        pts[i * 2 + 1] = u * u * p0y + 2 * u * t * p1y + t * t * p2y;
    }
}

/* ======================= 表情参数 ======================= */
typedef struct {
    float w, h, rtl, rtr, rbr, rbl, open, lid_top, lid_bot, dx, dy, rot;
} eye_t;
typedef struct { int type; float tilt, dy, len, thick; } brow_t;
typedef struct { int type; float w, h, dy, thick; } mouth_t;

typedef struct {
    const char *id, *cn;
    uint32_t dur;
    eye_t el, er;
    brow_t bl, br;
    mouth_t mo;
    float blush; int accent, micro; float amp, per, gaze;
} face_def_t;

#define EYE(...) (eye_t){ .w = 44, .h = 54, .rtl = 16, .rtr = 16, .rbr = 16, .rbl = 16, .open = 1, __VA_ARGS__ }
#define BRW(...) (brow_t){ .type = 0, .len = 26, .thick = 7, __VA_ARGS__ }
#define MTH(...) (mouth_t){ .type = 0, .w = 40, .h = 20, .thick = 7, __VA_ARGS__ }

/* 微动: 0 无 1 上下 2 跳 3 横抖 4 摇摆 5 呼吸 6 点头 7 眼抖 8 轻颤 9 脉冲
         10 睁眼 11 快点 12 回头 13 哈欠 14 发抖
   嘴型: 0 无 1 微笑 2 沮丧 3 O 4 大笑 5 抽泣 6 坏笑 7 波浪 8 一字 9 吐舌
         10 咬牙 11 嘟嘴 12 咧嘴齿 13 小一字 14 笑开
   装饰: 0 无 1 Zzz 2 爱心 3 眼泪 4 星星 5 音符 6 汗滴 7 问号 8 思考点 9 火花
         10 转圈 11 声波 12 碎心 13 感叹号 14 省略号 15 低电量 16 唇印 17 雪花 18 火苗 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
static const face_def_t FACES[FACE_COUNT] = {
    /* --- 原有 16 种, 索引与 v2 完全一致 --- */
    { "NORMAL","平静",12000, EYE(), EYE(), BRW(), BRW(), MTH(), 0,0,0,0,1,.6f },
    { "HAPPY","开心",5200, EYE(.h=30,.rtl=15,.rtr=15,.rbr=8,.rbl=8), EYE(.h=30,.rtl=15,.rtr=15,.rbr=8,.rbl=8), BRW(), BRW(), MTH(.type=1,.w=44,.h=24), .42f,0,1,1.6f,1.6f,.7f },
    { "SAD","难过",5200, EYE(.h=46,.open=.86f,.rtl=22,.rtr=8,.rbr=8,.rbl=22,.dy=3), EYE(.h=46,.open=.86f,.rtl=22,.rtr=8,.rbr=8,.rbl=22,.dy=3), BRW(.type=1,.tilt=-13,.dy=-1), BRW(.type=1,.tilt=-13,.dy=-1), MTH(.type=2,.w=34,.h=18), .18f,0,0,0,1,.6f },
    { "ANGRY","生气",5200, EYE(.h=34,.rtl=5,.rtr=5,.rbr=17,.rbl=17), EYE(.h=34,.rtl=5,.rtr=5,.rbr=17,.rbl=17), BRW(.type=1,.tilt=13,.thick=8), BRW(.type=1,.tilt=13,.thick=8), MTH(.type=10,.w=38,.h=18), 0,0,8,.7f,.22f,.3f },
    { "SURPRISED","惊讶",5200, EYE(.w=50,.h=62,.rtl=21,.rtr=21,.rbr=21,.rbl=21), EYE(.w=50,.h=62,.rtl=21,.rtr=21,.rbr=21,.rbl=21), BRW(.type=2,.tilt=-6,.dy=-4), BRW(.type=2,.tilt=-6,.dy=-4), MTH(.type=3,.w=22,.h=26), 0,0,9,.2f,1.2f,.4f },
    { "SLEEPY","困倦",5600, EYE(.h=22,.rtl=11,.rtr=11,.rbr=11,.rbl=11), EYE(.h=22,.rtl=11,.rtr=11,.rbr=11,.rbl=11), BRW(), BRW(), MTH(.type=1,.w=22,.h=12,.dy=-6), .2f,1,5,.35f,3.2f,.2f },
    { "LOVE","喜爱",5200, EYE(.h=28,.rtl=14,.rtr=14,.rbr=9,.rbl=9), EYE(.h=28,.rtl=14,.rtr=14,.rbr=9,.rbl=9), BRW(), BRW(), MTH(.type=1,.w=30,.h=18), .95f,2,9,.18f,.9f,.4f },
    { "WINK","眨眼",4600, EYE(), EYE(.h=7,.rtl=3.5f,.rtr=3.5f,.rbr=3.5f,.rbl=3.5f), BRW(), BRW(), MTH(.type=1,.w=40,.h=22), .4f,0,1,1.2f,1.8f,.6f },
    { "CRY","哭泣",5200, EYE(.h=40,.open=.8f,.rtl=22,.rtr=8,.rbr=10,.rbl=22,.dy=3), EYE(.h=40,.open=.8f,.rtl=22,.rtr=8,.rbr=10,.rbl=22,.dy=3), BRW(.type=1,.tilt=-11,.dy=-2), BRW(.type=1,.tilt=-11,.dy=-2), MTH(.type=5,.w=28,.h=16), .25f,3,0,0,1,.2f },
    { "DIZZY","晕眩",5200, EYE(.w=40,.h=40,.rtl=4,.rtr=18,.rbr=4,.rbl=18), EYE(.w=40,.h=40,.rtl=4,.rtr=18,.rbr=4,.rbl=18), BRW(), BRW(), MTH(.type=7,.w=44,.h=16), 0,0,7,11,1.1f,.2f },
    { "EXCITED","兴奋",4400, EYE(.w=48,.h=60,.rtl=20,.rtr=20,.rbr=20,.rbl=20), EYE(.w=48,.h=60,.rtl=20,.rtr=20,.rbr=20,.rbl=20), BRW(.type=2,.tilt=-8,.dy=-3), BRW(.type=2,.tilt=-8,.dy=-3), MTH(.type=14,.w=46,.h=30), .25f,0,2,4.5f,.5f,.5f },
    { "COOL","酷",5200, EYE(.h=18,.rtl=4,.rtr=4,.rbr=9,.rbl=9,.dx=8), EYE(.h=18,.rtl=4,.rtr=4,.rbr=9,.rbl=9,.dx=8), BRW(.type=1,.tilt=-4,.dy=-2,.thick=6), BRW(.type=1,.tilt=-4,.dy=-2,.thick=6), MTH(.type=6,.w=36,.h=13), 0,0,1,1.0f,2.4f,.4f },
    /* 撸猫: 眼睛舒服得「眯成一条弯月」+ 满腮红 + 爱心飘(微动 9 = 腮红脉冲)。
       原来是"小眼睛 + 小一字嘴", 看不出被摸舒服了 —— 换成这个更像猫被顺毛。 */
    { "SHY","害羞",5200, EYE(.w=40,.h=17,.rtl=8.5f,.rtr=8.5f,.rbr=8.5f,.rbl=8.5f,.dy=4), EYE(.w=40,.h=17,.rtl=8.5f,.rtr=8.5f,.rbr=8.5f,.rbl=8.5f,.dy=4), BRW(), BRW(), MTH(.type=18,.w=26,.h=14), 1.0f,2,9,.3f,1.4f,.3f },
    { "SMUG","得意",5200, EYE(.h=30,.rtl=6,.rtr=6,.rbr=15,.rbl=15,.dx=5), EYE(.h=30,.rtl=6,.rtr=6,.rbr=15,.rbl=15,.dx=5), BRW(.type=2,.tilt=-9,.dy=-4), BRW(.type=1,.tilt=5,.dy=-1), MTH(.type=6,.w=38,.h=14), .22f,0,0,0,1,.3f },
    { "CONFUSED","困惑",5200, EYE(), EYE(.w=34,.h=40,.rtl=13,.rtr=13,.rbr=13,.rbl=13,.open=.78f,.dy=2), BRW(.type=2,.tilt=-8), BRW(.type=1,.tilt=7), MTH(.type=7,.w=34,.h=12), 0,7,0,0,1,.6f },
    { "THINKING","思考",5200, EYE(.h=40,.open=.78f,.dy=-3,.dx=-4), EYE(.h=40,.open=.78f,.dy=-3,.dx=-4), BRW(.type=2,.tilt=-11,.dy=-5), BRW(.type=1,.tilt=-4,.dy=-2), MTH(.type=8,.w=22,.h=6,.dy=2), 0,8,0,0,1,.3f },
    /* --- v3 新增 --- */
    { "SMILE","微笑",5200, EYE(.h=36,.rtl=14,.rtr=14,.rbr=10,.rbl=10), EYE(.h=36,.rtl=14,.rtr=14,.rbr=10,.rbl=10), BRW(), BRW(), MTH(.type=1,.w=34,.h=18), .28f,0,5,.2f,3.4f,.7f },
    { "LAUGH","大笑",4200, EYE(.h=24,.rtl=12,.rtr=12,.rbr=8,.rbl=8), EYE(.h=24,.rtl=12,.rtr=12,.rbr=8,.rbl=8), BRW(), BRW(), MTH(.type=14,.w=48,.h=30), .5f,4,2,4.5f,.5f,.4f },
    { "ANNOYED","不耐烦",5200, EYE(.h=20,.rtl=4,.rtr=4,.rbr=10,.rbl=10), EYE(.h=20,.rtl=4,.rtr=4,.rbr=10,.rbl=10), BRW(.type=1,.tilt=7), BRW(.type=1,.tilt=7), MTH(.type=13,.w=24,.h=6), 0,0,12,6,2.0f,.4f },
    { "SHOCK","震惊",4600, EYE(.w=54,.h=64,.rtl=22,.rtr=22,.rbr=22,.rbl=22,.dy=-2), EYE(.w=54,.h=64,.rtl=22,.rtr=22,.rbr=22,.rbl=22,.dy=-2), BRW(.type=2,.tilt=-10,.dy=-6), BRW(.type=2,.tilt=-10,.dy=-6), MTH(.type=3,.w=26,.h=30), 0,13,3,1.4f,.18f,1 },
    { "SCARED","惊恐",5200, EYE(.w=48,.h=58,.rtl=20,.rtr=20,.rbr=20,.rbl=20,.dy=-1), EYE(.w=48,.h=58,.rtl=20,.rtr=20,.rbr=20,.rbl=20,.dy=-1), BRW(.type=2,.tilt=-11,.dy=-5), BRW(.type=2,.tilt=-11,.dy=-5), MTH(.type=3,.w=16,.h=20), 0,6,8,.9f,.16f,.4f },
    { "PANIC","慌乱",4200, EYE(.w=46,.h=50,.rtl=18,.rtr=18,.rbr=18,.rbl=18), EYE(.w=46,.h=50,.rtl=18,.rtr=18,.rbr=18,.rbl=18), BRW(.type=2,.tilt=-8,.dy=-5), BRW(.type=2,.tilt=-8,.dy=-5), MTH(.type=7,.w=36,.h=16), 0,6,8,1.4f,.1f,1 },
    { "SLEEPING","熟睡",7000, EYE(.h=6,.rtl=3,.rtr=3,.rbr=3,.rbl=3,.lid_bot=.04f), EYE(.h=6,.rtl=3,.rtr=3,.rbr=3,.rbl=3,.lid_bot=.04f), BRW(), BRW(), MTH(.type=11,.w=14,.h=14,.dy=-2), .25f,1,5,.45f,4.2f,0 },
    { "YAWN","打哈欠",4200, EYE(.h=30,.rtl=14,.rtr=14,.rbr=14,.rbl=14), EYE(.h=30,.rtl=14,.rtr=14,.rbr=14,.rbl=14), BRW(), BRW(), MTH(.type=3,.w=30,.h=34), .25f,1,13,1,2.8f,1 },
    { "TIRED","疲惫",5600, EYE(.h=24,.rtl=12,.rtr=12,.rbr=10,.rbl=10,.dy=2), EYE(.h=24,.rtl=12,.rtr=12,.rbr=10,.rbl=10,.dy=2), BRW(.type=1,.tilt=-6,.dy=-1), BRW(.type=1,.tilt=-6,.dy=-1), MTH(.type=13,.w=22,.h=6), .12f,0,5,.3f,4.6f,.3f },
    { "KISS","亲亲",4600, EYE(.h=8,.rtl=4,.rtr=4,.rbr=4,.rbl=4), EYE(.h=8,.rtl=4,.rtr=4,.rbr=4,.rbl=4), BRW(), BRW(), MTH(.type=11,.w=12,.h=12), .8f,16,9,.12f,1.1f,.3f },
    { "GRATEFUL","感动",5600, EYE(.h=16,.rtl=8,.rtr=8,.rbr=8,.rbl=8,.dy=1), EYE(.h=16,.rtl=8,.rtr=8,.rbr=8,.rbl=8,.dy=1), BRW(), BRW(), MTH(.type=1,.w=28,.h=16), .62f,2,5,.28f,3.0f,.2f },
    { "WINK_L","左眨",4600, EYE(.h=7,.rtl=3.5f,.rtr=3.5f,.rbr=3.5f,.rbl=3.5f), EYE(), BRW(), BRW(), MTH(.type=1,.w=40,.h=22), .4f,0,1,1.2f,1.8f,.6f },
    { "WINK_R","右眨",4600, EYE(), EYE(.h=7,.rtl=3.5f,.rtr=3.5f,.rbr=3.5f,.rbl=3.5f), BRW(), BRW(), MTH(.type=1,.w=40,.h=22), .4f,0,1,1.2f,1.8f,.6f },
    { "PROUD","骄傲",5200, EYE(.h=36,.rtl=16,.rtr=16,.rbr=9,.rbl=9), EYE(.h=36,.rtl=16,.rtr=16,.rbr=9,.rbl=9), BRW(.type=1,.tilt=-5,.dy=-2), BRW(.type=1,.tilt=-5,.dy=-2), MTH(.type=1,.w=48,.h=26), .28f,9,0,0,1,.5f },
    { "CURIOUS","好奇",5200, EYE(.w=46,.h=58,.rtl=19,.rtr=19,.rbr=19,.rbl=19,.dx=5), EYE(.w=46,.h=58,.rtl=19,.rtr=19,.rbr=19,.rbl=19,.dx=5), BRW(.type=2,.tilt=-7,.dy=-4), BRW(.type=2,.tilt=-7,.dy=-4), MTH(.type=3,.w=16,.h=18), 0,7,12,7,2.4f,.8f },
    { "SUSPICIOUS","怀疑",5200, EYE(.h=22,.rtl=10,.rtr=10,.rbr=10,.rbl=10,.dx=-7), EYE(.h=22,.rtl=10,.rtr=10,.rbr=10,.rbl=10,.dx=-7), BRW(.type=2,.tilt=-10,.dy=-3), BRW(.type=1,.tilt=3), MTH(.type=6,.w=36,.h=12), 0,0,3,.8f,3.0f,.2f },
    { "BORED","无聊",5600, EYE(.h=20,.rtl=10,.rtr=10,.rbr=10,.rbl=10), EYE(.h=20,.rtl=10,.rtr=10,.rbr=10,.rbl=10), BRW(.type=1,.tilt=-3,.dy=2,.thick=6), BRW(.type=1,.tilt=-3,.dy=2,.thick=6), MTH(.type=8,.w=26,.h=6), 0,14,5,.3f,5.0f,.5f },
    { "RELIEVED","松口气",5200, EYE(.h=14,.rtl=7,.rtr=7,.rbr=7,.rbl=7), EYE(.h=14,.rtl=7,.rtr=7,.rbr=7,.rbl=7), BRW(), BRW(), MTH(.type=1,.w=34,.h=18), .2f,6,5,.4f,2.6f,.3f },
    { "DETERMINED","坚毅",5200, EYE(.h=34,.rtl=6,.rtr=6,.rbr=14,.rbl=14), EYE(.h=34,.rtl=6,.rtr=6,.rbr=14,.rbl=14), BRW(.type=1,.tilt=9,.thick=8), BRW(.type=1,.tilt=9,.thick=8), MTH(.type=8,.w=34,.h=8), 0,9,0,0,1,.4f },
    { "SERIOUS","严肃",5200, EYE(.h=38,.rtl=8,.rtr=8,.rbr=12,.rbl=12), EYE(.h=38,.rtl=8,.rtr=8,.rbr=12,.rbl=12), BRW(.type=1,.tilt=6), BRW(.type=1,.tilt=6), MTH(.type=8,.w=30,.h=7), 0,0,0,0,1,.5f },
    { "SILLY","蠢萌",5200, EYE(.h=50,.rtl=22,.rtr=22,.rbr=22,.rbl=22,.dx=-6), EYE(.h=50,.rtl=22,.rtr=22,.rbr=22,.rbl=22,.dx=-6), BRW(), BRW(), MTH(.type=9,.w=40,.h=26), .35f,0,7,8,1.4f,.3f },
    { "SICK","不舒服",5200, EYE(.h=24,.rtl=12,.rtr=12,.rbr=10,.rbl=10), EYE(.h=24,.rtl=12,.rtr=12,.rbr=10,.rbl=10), BRW(.type=1,.tilt=-8,.dy=-2), BRW(.type=1,.tilt=-8,.dy=-2), MTH(.type=7,.w=40,.h=14), .3f,6,4,5,2.6f,.2f },
    { "DEAD","离线",4600, EYE(.h=4,.rtl=2,.rtr=2,.rbr=2,.rbl=2), EYE(.h=4,.rtl=2,.rtr=2,.rbr=2,.rbl=2), BRW(.type=1,.tilt=12,.dy=-2,.thick=6), BRW(.type=1,.tilt=12,.dy=-2,.thick=6), MTH(.type=8,.w=30,.h=6), 0,0,0,0,1,0 },
    { "HELLO","打招呼",4200, EYE(.w=48,.h=52,.rtl=18,.rtr=18,.rbr=18,.rbl=18), EYE(.w=48,.h=52,.rtl=18,.rtr=18,.rbr=18,.rbl=18), BRW(.type=2,.tilt=-6,.dy=-3), BRW(.type=2,.tilt=-6,.dy=-3), MTH(.type=1,.w=48,.h=22), .25f,4,6,3,1.6f,.8f },
    { "BYE","再见",4200, EYE(.h=40,.rtl=8,.rtr=8,.rbr=14,.rbl=14,.dx=4), EYE(.h=40,.rtl=8,.rtr=8,.rbr=14,.rbl=14,.dx=4), BRW(), BRW(), MTH(.type=1,.w=34,.h=20), 0,0,1,2.2f,1.0f,.6f },
    { "LISTENING","聆听",5600, EYE(.h=46,.open=.92f,.dx=-7), EYE(.h=46,.open=.92f,.dx=-7), BRW(.type=2,.tilt=-8,.dy=-3), BRW(.type=1), MTH(.type=8,.w=20,.h=6), .2f,11,1,1.0f,2.0f,.2f },
    { "MUSIC","听歌",5200, EYE(.h=18,.rtl=9,.rtr=9,.rbr=9,.rbl=9,.dy=1), EYE(.h=18,.rtl=9,.rtr=9,.rbr=9,.rbl=9,.dy=1), BRW(), BRW(), MTH(.type=1,.w=34,.h=20), .3f,5,1,2.2f,1.0f,.2f },
    { "LOADING","加载中",5200, EYE(.h=44,.open=.88f), EYE(.h=44,.open=.88f), BRW(), BRW(), MTH(.type=13,.w=16,.h=5), 0,10,0,0,1,.3f },
    { "ERROR","错误",4200, EYE(.h=8,.rtl=4,.rtr=4,.rbr=4,.rbl=4), EYE(.h=8,.rtl=4,.rtr=4,.rbr=4,.rbl=4), BRW(.type=1,.tilt=11), BRW(.type=1,.tilt=11), MTH(.type=2,.w=30,.h=16), 0,13,3,1.6f,.5f,1 },
    { "COLD","冷",5200, EYE(.w=36,.h=44,.rtl=14,.rtr=14,.rbr=14,.rbl=14,.dy=1), EYE(.w=36,.h=44,.rtl=14,.rtr=14,.rbr=14,.rbl=14,.dy=1), BRW(.type=1,.tilt=-7,.dy=-2), BRW(.type=1,.tilt=-7,.dy=-2), MTH(.type=7,.w=26,.h=12), 0,17,8,.6f,.3f,1 },
    { "HOT","热",5200, EYE(.h=20,.rtl=10,.rtr=10,.rbr=10,.rbl=10), EYE(.h=20,.rtl=10,.rtr=10,.rbr=10,.rbl=10), BRW(), BRW(), MTH(.type=7,.w=34,.h=14), .55f,6,5,.35f,1.6f,1 },
    { "HUNGRY","饿",5200, EYE(.w=42,.h=50,.rtl=17,.rtr=17,.rbr=17,.rbl=17,.dy=2,.dx=-4), EYE(.w=42,.h=50,.rtl=17,.rtr=17,.rbr=17,.rbl=17,.dy=2,.dx=-4), BRW(), BRW(), MTH(.type=3,.w=28,.h=30), .2f,8,2,2.4f,1.4f,.7f },
    { "STARSTRUCK","崇拜",5200, EYE(.w=52,.h=60,.rtl=21,.rtr=21,.rbr=21,.rbl=21), EYE(.w=52,.h=60,.rtl=21,.rtr=21,.rbr=21,.rbl=21), BRW(.type=2,.tilt=-9,.dy=-4), BRW(.type=2,.tilt=-9,.dy=-4), MTH(.type=3,.w=20,.h=22), .35f,4,9,.22f,1.0f,1 },
    { "GUILTY","心虚",5200, EYE(.w=40,.h=44,.rtl=16,.rtr=16,.rbr=16,.rbl=16,.dx=7,.dy=3), EYE(.w=40,.h=44,.rtl=16,.rtr=16,.rbr=16,.rbl=16,.dx=7,.dy=3), BRW(.type=1,.tilt=-7,.dy=-1), BRW(.type=1,.tilt=-7,.dy=-1), MTH(.type=7,.w=26,.h=10), .35f,6,8,.5f,.5f,.2f },
    { "SUCCESS","成功",4600, EYE(.h=26,.rtl=13,.rtr=13,.rbr=8,.rbl=8), EYE(.h=26,.rtl=13,.rtr=13,.rbr=8,.rbl=8), BRW(), BRW(), MTH(.type=1,.w=44,.h=24), .3f,9,2,3.2f,.6f,.6f },
    { "AWAKE","醒来",4200, EYE(.h=50,.rtl=18,.rtr=18,.rbr=18,.rbl=18), EYE(.h=50,.rtl=18,.rtr=18,.rbr=18,.rbl=18), BRW(), BRW(), MTH(.type=3,.w=16,.h=18), .2f,0,10,1,2.6f,.5f },
    /* AI 播报时用: 两个正常眼睛 + 一条随时间跑动的电流波形嘴(嘟噜噜) */
    { "SPEAKING","说话",9000, EYE(.h=48,.open=.95f), EYE(.h=48,.open=.95f), BRW(.type=2,.tilt=-4,.dy=-3), BRW(.type=2,.tilt=4,.dy=-3), MTH(.type=15,.w=54,.h=20), .15f,11,0,.8f,1.2f,.3f },
};
#pragma GCC diagnostic pop

/* ======================= 运行时状态 ======================= */
static struct {
    int cur, prev;
    float morph, emo_timer;
    uint32_t morph_ms, override_dur;
    /* 眨眼 */
    int blink_state; float blink_t, next_blink;
    /* 扫视 */
    float look_timer, next_look, look_hold; int look_state;
    /* 弹簧视线 */
    float gz_x, gz_v, gz_target;
    float manual_look;        /* >900 = 自动 */
    float t;
    bool auto_cycle;
    uint32_t hold_ms, hold_timer;   /* 指定表情保持时长(AI 对话用) */
    uint32_t full_timer;            /* 定期全屏刷新的计时 */
    bool force_full;                /* 强制下一帧全屏(传输出错后自愈) */
    bool test_mode;                 /* 方向测试图 */
    uint32_t err_cnt;
    uint8_t dirty_pages;
} s;

/* ======================= 工具 ======================= */
static float lerpf(float a, float b, float m) { return a + (b - a) * m; }
static int   snapi(int a, int b, float m) { return m < 0.5f ? a : b; }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float smoothstepf(float x)
{
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    return x * x * (3.0f - 2.0f * x);
}
static float easeinout(float x) { return x < 0.5f ? 2 * x * x : 1.0f - (-2 * x + 2) * (-2 * x + 2) * 0.5f; }

static void rot_point(float x, float y, float deg, float cx, float cy, float *ox, float *oy)
{
    if (deg == 0.0f) { *ox = x; *oy = y; return; }
    float a = deg * PI / 180.0f, ca = cosf(a), sa = sinf(a);
    float dx = x - cx, dy = y - cy;
    *ox = cx + dx * ca - dy * sa;
    *oy = cy + dx * sa + dy * ca;
}

/* ======================= 场景插值 ======================= */
typedef struct {
    eye_t el, er; brow_t bl, br; mouth_t mo;
    float blush, amp, per, gaze, face_rot;
    int accent;
} scene_t;

static eye_t lerp_eye(const eye_t *a, const eye_t *b, float m)
{
    eye_t o;
    o.w = lerpf(a->w, b->w, m);       o.h = lerpf(a->h, b->h, m);
    o.rtl = lerpf(a->rtl, b->rtl, m); o.rtr = lerpf(a->rtr, b->rtr, m);
    o.rbr = lerpf(a->rbr, b->rbr, m); o.rbl = lerpf(a->rbl, b->rbl, m);
    o.open = lerpf(a->open, b->open, m);
    o.lid_top = lerpf(a->lid_top, b->lid_top, m);
    o.lid_bot = lerpf(a->lid_bot, b->lid_bot, m);
    o.dx = lerpf(a->dx, b->dx, m);   o.dy = lerpf(a->dy, b->dy, m);
    o.rot = lerpf(a->rot, b->rot, m);
    return o;
}
static brow_t lerp_brow(const brow_t *a, const brow_t *b, float m)
{
    brow_t o;
    o.type = snapi(a->type, b->type, m);
    o.tilt = lerpf(a->tilt, b->tilt, m); o.dy = lerpf(a->dy, b->dy, m);
    o.len = lerpf(a->len, b->len, m);    o.thick = lerpf(a->thick, b->thick, m);
    return o;
}
static mouth_t lerp_mouth(const mouth_t *a, const mouth_t *b, float m)
{
    mouth_t o;
    o.type = snapi(a->type, b->type, m);
    o.w = lerpf(a->w, b->w, m); o.h = lerpf(a->h, b->h, m);
    o.dy = lerpf(a->dy, b->dy, m); o.thick = lerpf(a->thick, b->thick, m);
    return o;
}

static scene_t build_scene(const face_def_t *A, const face_def_t *B, float m, float t, float open)
{
    scene_t sc;
    sc.el = lerp_eye(&A->el, &B->el, m);
    sc.er = lerp_eye(&A->er, &B->er, m);
    sc.bl = lerp_brow(&A->bl, &B->bl, m);
    sc.br = lerp_brow(&A->br, &B->br, m);
    sc.mo = lerp_mouth(&A->mo, &B->mo, m);
    sc.blush = lerpf(A->blush, B->blush, m);
    sc.gaze = lerpf(A->gaze, B->gaze, m);
    sc.accent = snapi(A->accent, B->accent, m);
    sc.face_rot = 0.0f;
    int micro = snapi(A->micro, B->micro, m);
    float amp = lerpf(A->amp, B->amp, m), per = lerpf(A->per, B->per, m);
    if (per < 0.05f) per = 0.05f;
    sc.amp = amp; sc.per = per;
    float sn = sinf(t / per * TAU);

    switch (micro) {
    case 1: sc.el.dy += amp * sn; sc.er.dy += amp * sn; sc.mo.dy += amp * sn * .4f; break;
    case 2: { float k = fabsf(sn); sc.el.dy -= k * amp; sc.er.dy -= k * amp; sc.mo.dy -= k * amp * .5f; break; }
    case 3: sc.el.dx += sn * amp; sc.er.dx += sn * amp; break;
    case 4: sc.face_rot += sn * amp; break;
    case 5: { float k = 1 + sn * amp * .35f; sc.el.open *= k; sc.er.open *= k; break; }
    case 6: sc.face_rot += sn * amp * .6f; sc.el.dy += sn * amp; sc.er.dy += sn * amp; break;
    case 7: sc.el.rot += sn * amp; sc.er.rot += sn * amp; break;
    case 8: { float f = sinf(t * TAU * per); sc.el.dx += f * amp; sc.er.dx += f * amp; break; }
    case 9: { float k = 1 + sn * amp; sc.blush *= k; break; }
    case 10: { /* 睁眼循环: 用余弦保证周期首尾平滑, 不能有跳变 */
               float p = fmodf(t / per, 1.0f);
               float k = .5f - .5f * cosf(p * TAU);
               float o = .06f + k * .94f;
               sc.el.open *= o; sc.er.open *= o; break; }
    case 11: { float p = fmodf(t / per, 1.0f), k = smoothstepf(p * 2) * smoothstepf(2 - p * 2); sc.el.dy += amp * k; sc.er.dy += amp * k; break; }
    case 12: { float p = fmodf(t / per, 1.0f), k = sinf(p * TAU) * expf(-p * 2.2f); sc.el.dx += k * amp; sc.er.dx += k * amp; break; }
    case 13: { float p = fmodf(t / per, 1.0f), k = smoothstepf(p * 3) * smoothstepf((1 - p) * 3);
               sc.mo.h *= (0.35f + k * 0.9f); sc.el.open *= (1 - k * .85f); sc.er.open *= (1 - k * .85f); break; }
    case 14: sc.el.dx += sinf(t * 42) * amp * .35f; sc.er.dx += sinf(t * 42) * amp * .35f;
             sc.el.dy += sinf(t * 31) * amp * .2f; break;
    }

    /* 全局呼吸 */
    float br = sinf(t / 3.4f * TAU);
    sc.el.dy += br * .5f; sc.er.dy += br * .5f; sc.mo.dy += br * .3f;

    sc.el.open *= open; sc.er.open *= open;
    sc.blush *= 0.5f;   /* 单色屏上腮红减半, 避免抖动噪点 */
    return sc;
}

/* ======================= 绘制 ======================= */
#define D(v)  ((v) * KP)           /* 设计单位 -> 像素 */

static void draw_eye(const eye_t *e, float cx_base, float cy_base,
                     float look, float gaze, bool mirror)
{
    float cx = cx_base + D(look * gaze) + D(e->dx);
    float cy = cy_base + D(e->dy);
    float open = clampf(e->open, 0.0f, 1.0f);
    float h = D(e->h);
    float drop = (1 - open) * h * 0.55f;
    float rise = (1 - open) * h * 0.45f;
    /* lid_top/lid_bot 是无量纲比例(0~1), 不能再乘缩放系数 */
    float top = cy - h * 0.5f + drop + e->lid_top * h;
    float bot = cy + h * 0.5f - rise - e->lid_bot * h;
    const float MINBAR = D(3.6f);
    if (bot - top < MINBAR) {
        float mid = (top + bot) * 0.5f;
        top = mid - MINBAR * 0.5f;
        bot = mid + MINBAR * 0.5f;
    }
    float hh = bot - top;
    /* 运动挤压: 视线高速平移时横向略微拉宽 */
    float sq = fabsf(s.gz_v) * 0.0035f;
    if (sq > 0.06f) sq = 0.06f;
    float st = 1.0f + sq;
    float w = D(e->w) * st;
    float rtl = D(mirror ? e->rtr : e->rtl), rtr = D(mirror ? e->rtl : e->rtr);
    float rbl = D(mirror ? e->rbr : e->rbl), rbr = D(mirror ? e->rbl : e->rbr);
    ras_rr4(cx - w * 0.5f, top, w, hh, rtl, rtr, rbr, rbl, e->rot, cx, cy, false, 1.0f);
}

#if FACE_DRAW_BROWS
static void draw_brow(const brow_t *b, float cx_base, float cy_base, float look, float gaze, int side)
{
    if (b->type == 0 || b->thick < 0.5f) return;
    float cx = cx_base + D(look * gaze);
    float cy = cy_base + D(b->dy) - D(34.0f);
    float half = D(b->len) * 0.5f;
    float tilt = D(b->tilt), th = D(b->thick);
    float ox = side == 0 ? cx - half : cx + half;
    float ix = side == 0 ? cx + half : cx - half;
    float P[22];
    if (b->type == 2) {
        quad(P, ox, cy + D(3.0f), cx, cy - D(6.0f), ix, cy + tilt * .5f, 9);
        ras_stroke(P, 10, th, false, 1.0f);
    } else {
        quad(P, ox, cy, (ox + ix) * 0.5f, cy - D(1.0f), ix, cy + tilt, 9);
        ras_stroke(P, 10, th, false, 1.0f);
    }
}
#endif /* FACE_DRAW_BROWS */

static void draw_mouth(const mouth_t *m, float cx, float cy, float tm)
{
    if (m->type == 0) return;
    float w = D(m->w), h = D(m->h), t = D(m->thick);
    float P[66];
    switch (m->type) {
    case 1: quad(P, cx - w * .5f, cy - h * .35f, cx, cy + h * .35f, cx + w * .5f, cy - h * .35f, 15);
        ras_stroke(P, 16, t, false, 1); break;
    case 2: quad(P, cx - w * .5f, cy + h * .35f, cx, cy - h * .35f, cx + w * .5f, cy + h * .35f, 15);
        ras_stroke(P, 16, t, false, 1); break;
    case 3: case 11: ras_ell(cx, cy, w * .5f, h * .5f, false, 1); break;
    case 4:
        ras_rr4(cx - w * .5f, cy - h * .5f, w, h, D(9), D(9), D(9), D(9), 0, 0, 0, false, 1);
        ras_rr4(cx - w * .5f + D(3), cy - h * .5f + D(2), w - D(6), D(2.5f), D(1.2f), D(1.2f), D(1.2f), D(1.2f), 0, 0, 0, true, 1);
        break;
    case 5: quad(P, cx - w * .5f, cy + h * .4f, cx, cy - h * .2f, cx + w * .5f, cy + h * .4f, 11);
        ras_stroke(P, 12, t * .8f, false, 1); break;
    case 6: { float A[6] = { cx - w * .5f, cy, cx + w * .18f, cy - h * .18f, cx + w * .5f, cy + h * .1f };
        ras_stroke(A, 3, t, false, 1); break; }
    case 7: {
        int n = 6;
        for (int i = 0; i <= n; i++) { P[i * 2] = cx - w * .5f + w * i / (float)n; P[i * 2 + 1] = cy + ((i & 1) ? h * .5f : -h * .5f); }
        ras_stroke(P, n + 1, t * .85f, false, 1); break;
    }
    case 8: case 13: ras_line(cx - w * .5f, cy, cx + w * .5f, cy, t < D(4) ? D(4) : t, false, 1); break;
    case 9:
        quad(P, cx - w * .5f, cy - h * .2f, cx, cy + h * .2f, cx + w * .5f, cy - h * .2f, 11);
        ras_stroke(P, 12, t, false, 1);
        ras_rr4(cx - w * .22f, cy + h * .05f, w * .44f, h * .55f, D(4), D(4), D(4), D(4), 0, 0, 0, false, 1);
        break;
    case 10:
        ras_rr4(cx - w * .5f, cy - h * .5f, w, h, D(5), D(5), D(5), D(5), 0, 0, 0, false, 1);
        for (int i = 1; i < 5; i++) ras_line(cx - w * .5f + w * i / 5.0f, cy - h * .5f, cx - w * .5f + w * i / 5.0f, cy + h * .5f, D(1.8f), true, 1);
        break;
    case 12:
        ras_rr4(cx - w * .5f, cy - h * .5f, w, h, D(8), D(8), D(14), D(14), 0, 0, 0, false, 1);
        ras_line(cx - w * .5f + D(4), cy - h * .5f + D(3.5f), cx + w * .5f - D(4), cy - h * .5f + D(3.5f), D(2.2f), true, 1);
        break;
    case 14:
        ras_rr4(cx - w * .5f, cy - h * .5f, w, h, D(10), D(10), D(16), D(16), 0, 0, 0, false, 1);
        ras_rr4(cx - w * .5f + D(5), cy - h * .5f + D(4), w - D(10), h * .45f, D(5), D(5), D(5), D(5), 0, 0, 0, true, 1);
        break;
    case 15: {
        /* "嘟噜噜"电流说话嘴: 一条随时间跑动的复合波形。
           主波 + 二次谐波叠加, 相位随 t 平移 → 看起来像电流在嘴里跑;
           振幅随 t 快慢交错地张合 → 像说话时嘴一开一合一嘟噜。 */
        int n = 14;
        /* 开合: 两个不同频率叠加, 避免规律太明显 */
        float k = 0.42f + 0.34f * sinf(tm * 17.0f) + 0.24f * sinf(tm * 26.0f + 1.1f);
        if (k < 0.12f) k = 0.12f;
        if (k > 1.0f)  k = 1.0f;
        float amp = h * .55f * k;
        for (int i = 0; i <= n; i++) {
            float u = (float)i / (float)n;
            float ph = u * 11.0f - tm * 20.0f;      /* 相位随时间平移 = 跑动 */
            float v = sinf(ph) * .62f + sinf(ph * 2.3f + 1.7f) * .38f;
            /* 两端收窄成尖角, 更像"一嘟噜" */
            float taper = 0.45f + 0.55f * sinf(u * 3.14159f);
            P[i * 2]     = cx - w * .5f + w * u;
            P[i * 2 + 1] = cy + v * amp * taper;
        }
        ras_stroke(P, n + 1, t * .8f, false, 1);
        break;
    }
    case 18: {
        /* 「w」猫嘴: 三段折线两头微微上扬, 撸猫/撒娇时用。
           比一字嘴有表情, 又不至于像微笑那么"正式"。 */
        P[0] = cx - w * .5f;  P[1] = cy + h * .15f;
        P[2] = cx - w * .25f; P[3] = cy - h * .35f;
        P[4] = cx;            P[5] = cy + h * .20f;
        P[6] = cx + w * .25f; P[7] = cy - h * .35f;
        P[8] = cx + w * .5f;  P[9] = cy + h * .15f;
        ras_stroke(P, 5, t, false, 1);
        break;
    }
    }
}

static void draw_blush(float blush, float look, float gaze)
{
    if (blush < 0.02f) return;
    float y = EYE_CY + D(25.0f);
    const float xs[2] = { EYE_CX0 - D(25.0f), EYE_CX1 + D(25.0f) };
    for (int i = 0; i < 2; i++) {
        float x = xs[i] + D(look * gaze);
        ras_ell(x, y, D(13), D(6), false, blush * 0.275f);
        ras_ell(x, y, D(8), D(4), false, blush * 0.35f);
    }
}

/* ---------- 装饰件 ---------- */
static void draw_accent(int accent, float t)
{
    const float sz = A_SZ, sy = A_SY;
    const float tx = D(A_TRX), ty = D(A_TRY), rx = D(A_RX), ry = D(A_RY);
    float P[48];
    switch (accent) {
    case 1:
        for (int i = 0; i < 3; i++) {
            float ph = fmodf(t * .45f + i * .34f, 1.0f), sp = D((7 + i * 4) * sz);
            float x = tx + D(i * 7 * sz + ph * 7 * sz), y = ty - D((i * 15 + ph * 12) * sy);
            float a = clampf(1 - ph, 0, 1) * (1 - i * .18f);
            P[0] = x - sp * .5f; P[1] = y - sp * .5f; P[2] = x + sp * .5f; P[3] = y - sp * .5f;
            P[4] = x - sp * .5f; P[5] = y + sp * .5f; P[6] = x + sp * .5f; P[7] = y + sp * .5f;
            ras_stroke(P, 4, D(3 * sz), false, a);
        }
        break;
    case 2:
        for (int i = 0; i < 3; i++) {
            float ph = fmodf(t * .40f + i * .33f, 1.0f);
            float x = tx + D(i * 9 * sz + sinf(ph * TAU) * 5 * sz), y = ty - D((i * 8 + ph * 42) * sy);
            ras_heart(x, y, D((9 - i * 1.6f) * sz), false, clampf(1 - ph * 1.15f, 0, 1) * .95f);
        }
        break;
    case 3:
        for (int i = 0; i < 3; i++) {
            float ph = fmodf(t * .55f + i * .34f, 1.0f), a = clampf(1 - ph, 0, 1) * .95f;
            ras_ell(EYE_CX0 - D(16), EYE_CY + D(12 + ph * 34), D(3.2f), D(5.2f), false, a);
            ras_ell(EYE_CX1 + D(16), EYE_CY + D(12 + ph * 34), D(3.2f), D(5.2f), false, a);
        }
        break;
    case 4: {
        const float px3[3] = { tx - D(6), tx + D(16), rx - D(20) };
        const float py3[3] = { ty + D(2), ty - D(6), ry - D(12) };
        for (int i = 0; i < 3; i++) {
            float k = sinf(fmodf(t * .8f + i * .33f, 1.0f) * PI);
            ras_star(px3[i], py3[i], D((7 + k * 3) * sz), D(2.6f * sz), false, k * .95f);
        }
        break;
    }
    case 5:
        for (int i = 0; i < 2; i++) {
            float ph = fmodf(t * .35f + i * .5f, 1.0f), a = clampf(1 - ph * 1.1f, 0, 1) * .95f;
            float x = tx + D(i * 12 * sz + sinf(ph * TAU) * 4 * sz), y = ty - D(ph * 34 * sy);
            ras_ell(x - D(3), y + D(4), D(4.5f * sz), D(3.4f * sz), false, a);
            ras_rr4(x + D(1), y - D(8), D(1.8f), D(13), D(.9f), D(.9f), D(.9f), D(.9f), 0, 0, 0, false, a);
            P[0] = x + D(2.8f); P[1] = y - D(8); P[2] = x + D(8); P[3] = y - D(5.5f); P[4] = x + D(8); P[5] = y - D(1);
            ras_stroke(P, 3, D(1.8f), false, a);
        }
        break;
    case 6: {
        float ph = fmodf(t * .7f, 1.0f), d = D(5 * sz);
        float x = tx, y = ty + D(ph * 20 * sy);
        float Q[8] = { x, y - d * 1.2f, x + d * .84f, y + d * .3f, x, y + d, x - d * .84f, y + d * .3f };
        ras_polyfill(Q, 4, false, clampf(1 - ph, 0, 1) * .95f);
        break;
    }
    case 7: {
        float y = ty + D(sinf(t * TAU * .5f) * 3 * sy), d = D(9 * sz), a = .95f;
        P[0] = tx - d * .5f; P[1] = y - d * .2f; P[2] = tx; P[3] = y - d * .9f;
        P[4] = tx + d * .6f; P[5] = y - d * .3f; P[6] = tx + d * .2f; P[7] = y + d * .2f;
        P[8] = tx - d * .2f; P[9] = y;
        ras_stroke(P, 5, D(3.2f * sz), false, a);
        ras_ell(tx - d * .1f, y + d * .8f, D(1.8f * sz), D(1.8f * sz), false, a);
        break;
    }
    case 8: {
        int on = (int)(fmodf(t * 1.1f, 1.0f) * 3.99f) % 4;
        for (int i = 0; i < 3; i++)
            ras_ell(tx + D(i * 9 * sz), ty - D(i * 1.5f * sy), D(2.6f * sz), D(2.6f * sz), false, i == on ? .95f : .28f);
        break;
    }
    case 9:
        for (int i = 0; i < 6; i++) {
            float a = i / 6.0f * TAU + t * 1.6f, k = .5f + .5f * sinf(t * 3 + i);
            float r0 = D(A_SPR), r1 = r0 + D(8 + k * 6);
            float sx = D(A_SPX), sy2 = D(A_SPY);
            ras_line(sx + cosf(a) * r0, sy2 + sinf(a) * r0 * .85f,
                     sx + cosf(a) * r1, sy2 + sinf(a) * r1 * .85f, D(2.6f * sz), false, .35f + k * .55f);
        }
        break;
    case 10: {
        float a0 = t * TAU * 1.1f;
        ras_arcst(D(A_SPX), D(A_SPY), D(A_SPR), a0, a0 + 1.5f, D(5 * sz), false, .95f);
        break;
    }
    case 11:
        for (int i = 0; i < 5; i++) {
            float k = fabsf(sinf(t * 2.2f + i * .7f)), hgt = D((5 + k * 20) * sz);
            ras_rr4(rx + D(i * 7 * sz), ry - hgt * .5f, D(3.2f * sz), hgt, D(1.6f), D(1.6f), D(1.6f), D(1.6f), 0, 0, 0, false, .35f + k * .6f);
        }
        break;
    case 12:
        ras_heart(tx, ty, D(11 * sz), false, .95f);
        ras_line(tx - D(10 * sz), ty - D(10 * sz), tx + D(10 * sz), ty + D(10 * sz), D(3 * sz), true, 1);
        break;
    case 13: {
        float y = ty + D(sinf(t * TAU * .8f) * 2 * sy), d = D(10 * sz);
        ras_line(tx, y - d, tx, y + d * .3f, D(4 * sz), false, .95f);
        ras_ell(tx, y + d * .9f, D(2.2f * sz), D(2.2f * sz), false, .95f);
        break;
    }
    case 14:
        for (int i = 0; i < 3; i++) {
            float ph = fmodf(t * 1.6f + i * .3f, 1.0f);
            ras_ell(tx + D(i * 6 * sz), ty, D(1.8f * sz), D(1.8f * sz), false, clampf(1.2f - ph, .25f, .95f));
        }
        break;
    case 15: {
        float d = D(9 * sz);
        ras_rr4(tx - d, ty - d * .55f, d * 2, d * 1.1f, D(1.6f), D(1.6f), D(1.6f), D(1.6f), 0, 0, 0, false, .95f);
        ras_rr4(tx - d + D(1.4f), ty - d * .55f + D(1.4f), (d * 2 - D(2.8f)) * .24f, d * 1.1f - D(2.8f), D(1), D(1), D(1), D(1), 0, 0, 0, true, 1);
        break;
    }
    case 16: {
        float k = 1 + sinf(t * TAU * 1.1f) * .12f;
        ras_heart(tx, ty - D(sinf(t * TAU * .5f) * 3 * sy), D(8 * sz) * k, false, .95f);
        break;
    }
    case 17:
        for (int i = 0; i < 3; i++) {
            float ph = fmodf(t * .3f + i * .33f, 1.0f);
            float x = tx + D(sinf(ph * TAU + i) * 6 * sz), y = ty + D(8 * sy + ph * 18 * sy);
            float a = clampf(1 - ph, 0, 1) * .9f;
            ras_line(x - D(3 * sz), y, x + D(3 * sz), y, D(1.6f * sz), false, a);
            ras_line(x, y - D(3 * sz), x, y + D(3 * sz), D(1.6f * sz), false, a);
        }
        break;
    case 18:
        for (int i = 0; i < 3; i++) {
            float ph = fmodf(t * .8f + i * .33f, 1.0f), d = D((6 - i) * sz);
            float x = tx + D(sinf(ph * TAU + i * 2) * 3 * sz), y = ty - D(ph * 26 * sy);
            float Q[6] = { x, y - d, x + d * .7f, y + d * .5f, x - d * .7f, y + d * .5f };
            ras_polyfill(Q, 3, false, clampf(1 - ph, 0, 1) * .95f);
        }
        break;
    }
}

/* ======================= 渲染一帧 ======================= */
static void render(const scene_t *sc, float look, float t)
{
    memset(s_luma, 0, sizeof(s_luma));

    float lx, ly, rx, ry, mx, my;
    rot_point(EYE_CX0, EYE_CY, sc->face_rot, TILT_CX, TILT_CY, &lx, &ly);
    rot_point(EYE_CX1, EYE_CY, sc->face_rot, TILT_CX, TILT_CY, &rx, &ry);
    rot_point(MTH_CX, MTH_CY, sc->face_rot, TILT_CX, TILT_CY, &mx, &my);

    draw_blush(sc->blush, look, sc->gaze);
    draw_eye(&sc->el, lx, ly, look, sc->gaze, false);
    draw_eye(&sc->er, rx, ry, look, sc->gaze, true);
#if FACE_DRAW_BROWS
    draw_brow(&sc->bl, EYE_CX0, EYE_CY, look, sc->gaze, 0);
    draw_brow(&sc->br, EYE_CX1, EYE_CY, look, sc->gaze, 1);
#endif
    draw_mouth(&sc->mo, mx + D(look * sc->gaze * .3f), my + D(sc->mo.dy), t);
    draw_accent(sc->accent, t);
}

/* ======================= 抖动 + 脏页刷新 ======================= */
static const uint8_t s_bayer[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};

#define FULL_REFRESH_MS  3000   /* 每 3 秒兜底全屏刷一次, 防止意外丢页造成残留 */

static void flush(uint32_t dt_ms)
{
    uint8_t *fb = ssd1305_get_framebuffer();
    uint8_t pmin = 0xFF, pmax = 0;
    for (int p = 0; p < 8; p++) {
        int changed = 0;
        for (int x = 0; x < OLED_W; x++) {
            uint8_t bits = 0;
            for (int b = 0; b < 8; b++) {
                int y = p * 8 + b;
                int l = s_luma[y * OLED_W + x];
                if (l > 0) {
                    int th = s_bayer[y & 3][x & 3] * 16 + 8;
                    if (l >= th) bits |= (uint8_t)(1 << b);
                }
            }
            uint8_t old = fb[p * OLED_W + x];
            if (bits != old) {
                fb[p * OLED_W + x] = bits;
                changed = 1;
            }
        }
        if (changed) {
            if (p < pmin) pmin = (uint8_t)p;
            if (p > pmax) pmax = (uint8_t)p;
        }
    }
    /* 定期兜底: 或上一次传输失败时, 强制全屏重发 */
    s.full_timer += dt_ms;
    if (s.force_full || s.full_timer >= FULL_REFRESH_MS) {
        pmin = 0;
        pmax = 7;
        s.full_timer = 0;
        s.force_full = false;
        /* 重申显示配置: 否则被 I2C 毛刺/电压跌落打乱后, 画面会「自己倒过来」且不再恢复 */
        ssd1305_reassert();
    }
    if (pmax < pmin) { s.dirty_pages = 0; return; }   /* 画面无变化, 完全不占用 I2C */

    s.dirty_pages = (uint8_t)(pmax - pmin + 1);
    if (ssd1305_display_pages(pmin, pmax) != ESP_OK) {
        /* 传输出错时显存镜像已经领先于屏幕, 必须下一帧整屏重发才能自愈,
           否则那一页会永久残留旧内容 —— 就是画面「花掉」的根因之一 */
        s.force_full = true;
        s.err_cnt++;
        if (s.err_cnt <= 5 || (s.err_cnt % 200) == 0)
            ESP_LOGW(TAG, "display_pages(%u,%u) failed, total=%u",
                     (unsigned)pmin, (unsigned)pmax, (unsigned)s.err_cnt);
    }
}

/* ======================= 动画推进 ======================= */
/* 视线扫视幅度(设计单位): 越大眼睛左右甩得越开
   26 设计单位 ≈ 16.1px, 最宽的眼睛(SHOCK w=54)甩到边也只到 x≈120, 不会出界 */
#define GAZE_MAX        26.0f
#define GAZE_SWEEP_MIN  12.0f
#define GAZE_SWEEP_MAX  26.0f

#define BLINK_CLOSE_MS  85
#define BLINK_HOLD_MS   45
#define BLINK_OPEN_MS   115

static float update_blink(uint32_t dt)
{
    s.blink_t += (float)dt;
    switch (s.blink_state) {
    case 0: if (s.blink_t >= s.next_blink) { s.blink_state = 1; s.blink_t = 0; } break;
    case 1: if (s.blink_t >= BLINK_CLOSE_MS) { s.blink_state = 2; s.blink_t = 0; } break;
    case 2: if (s.blink_t >= BLINK_HOLD_MS) { s.blink_state = 3; s.blink_t = 0; } break;
    default: if (s.blink_t >= BLINK_OPEN_MS) {
                 s.blink_state = 0; s.blink_t = 0;
                 s.next_blink = 1400.0f + (float)(rand() % 3400);
             } break;
    }
    if (s.blink_state == 0) return 1.0f;
    if (s.blink_state == 1) return 1.0f - smoothstepf(s.blink_t / BLINK_CLOSE_MS);
    if (s.blink_state == 2) return 0.0f;
    return smoothstepf(s.blink_t / BLINK_OPEN_MS);
}

static void update_look(uint32_t dt)
{
    float d = (float)dt;
    s.look_timer += d;
    if (s.manual_look <= 900.0f) {
        s.gz_target = clampf(s.manual_look, -GAZE_MAX, GAZE_MAX);
        return;
    }
    if (s.look_state == 0) {
        if (s.look_timer >= s.next_look) {
            s.look_timer = 0;
            int span = (int)((GAZE_SWEEP_MAX - GAZE_SWEEP_MIN) * 10.0f) + 1;
            float d = GAZE_SWEEP_MIN + (float)(rand() % span) / 10.0f;
            s.gz_target = (rand() & 1) ? -d : d;
            s.look_hold = 500.0f + (float)(rand() % 1100);
            s.look_state = 1;
        }
    } else if (s.look_state == 1) {
        if (s.look_timer >= s.look_hold + 200.0f) { s.look_timer = 0; s.gz_target = 0; s.look_state = 2; }
    } else {
        if (s.look_timer >= 600.0f) {
            s.look_timer = 0; s.look_state = 0;
            s.next_look = 1800.0f + (float)(rand() % 4000);
        }
    }
}

/* 弹簧跟随: ω≈17.9 rad/s, ζ≈0.84 → 轻微过冲, 手感自然 */
static void step_gaze(float dt)
{
    float dt_s = dt / 1000.0f;
    if (dt_s > 0.05f) dt_s = 0.05f;
    const float K = 320.0f, Dd = 30.0f;
    s.gz_v += ((s.gz_target - s.gz_x) * K - s.gz_v * Dd) * dt_s;
    s.gz_x += s.gz_v * dt_s;
}

/* ======================= 公开 API ======================= */
void face_init(void)
{
    memset(&s, 0, sizeof(s));
    s.cur = FACE_NORMAL;
    s.prev = FACE_NORMAL;
    s.morph = 1.0f;
    s.morph_ms = 380;
    s.auto_cycle = true;
    s.gz_x = 0; s.gz_v = 0; s.gz_target = 0;
    s.manual_look = 999.0f;
    s.next_blink = 2400.0f;
    s.next_look = 2200.0f;
    ESP_LOGI(TAG, "face v3 init: %d emotions (四角圆角方块眼 / 弹簧跟随 / 脏页刷新)",
             (int)FACE_COUNT);
}

void face_update(uint32_t dt_ms)
{
    if (s.test_mode) return;        /* 测试图模式下不覆盖画面 */
    if (dt_ms == 0) dt_ms = 33;
    if (dt_ms > 100) dt_ms = 33;
    float t_dt = (float)dt_ms / 1000.0f;
    s.t += t_dt;

    if (s.auto_cycle) {
        if (s.hold_timer < s.hold_ms) {
            /* hold 期间冻结自动切换, 且不累积停留计时 */
            s.hold_timer += dt_ms;
            s.emo_timer = 0.0f;
        } else {
            s.emo_timer += (float)dt_ms;
            uint32_t dur = s.override_dur ? s.override_dur : FACES[s.cur].dur;
            if (s.emo_timer >= (float)dur) {
                s.emo_timer = 0;
                face_set_emotion((face_emotion_t)((s.cur + 1) % FACE_COUNT));
            }
        }
    }

    update_look(dt_ms);
    step_gaze((float)dt_ms);
    float open = update_blink(dt_ms);

    if (s.morph < 1.0f) {
        s.morph += s.morph_ms ? (float)dt_ms / (float)s.morph_ms : 1.0f;
        if (s.morph > 1.0f) s.morph = 1.0f;
    }
    scene_t sc = build_scene(&FACES[s.prev], &FACES[s.cur], easeinout(s.morph), s.t, open);
    render(&sc, s.gz_x, s.t);
    flush(dt_ms);
}

void face_set_emotion(face_emotion_t e)
{
    /* ★ 必须同时判负数: 只判上界的话, 传进来 -1 会被一路写进 s.cur,
       之后 FACES[s.cur] 就是越界读(崩溃级)。 */
    if ((int)e < 0 || (int)e >= FACE_COUNT || (int)e == s.cur) return;
    s.prev = s.cur;
    s.cur = (int)e;
    s.morph = 0.0f;
    s.emo_timer = 0.0f;
    ESP_LOGD(TAG, "emotion -> %s", FACES[s.cur].id);
}

face_emotion_t face_get_emotion(void) { return (face_emotion_t)s.cur; }

void face_blink(void)
{
    if (s.blink_state == 0) { s.blink_state = 1; s.blink_t = 0; }
}

void face_set_auto_cycle(bool enable) { s.auto_cycle = enable; }

void face_set_emotion_hold(face_emotion_t e, uint32_t hold_ms)
{
    face_set_emotion(e);
    s.hold_ms = hold_ms;
    s.hold_timer = 0;
    ESP_LOGD(TAG, "hold %s for %ums", FACES[s.cur].id, (unsigned)hold_ms);
}

bool face_set_emotion_by_name(const char *name)
{
    if (!name || !name[0]) return false;
    for (int i = 0; i < FACE_COUNT; i++) {
        if (strcasecmp(name, FACES[i].id) == 0) { face_set_emotion((face_emotion_t)i); return true; }
    }
    for (int i = 0; i < FACE_COUNT; i++) {
        if (strcmp(name, FACES[i].cn) == 0) { face_set_emotion((face_emotion_t)i); return true; }
    }
    ESP_LOGW(TAG, "unknown emotion name: %s", name);
    return false;
}

void face_set_emotion_duration(uint32_t ms) { s.override_dur = ms; }

void face_set_look(float offset) { s.manual_look = offset; }

const char *face_emotion_name(face_emotion_t e)
{
    return ((int)e >= 0 && (int)e < FACE_COUNT) ? FACES[e].id : "UNKNOWN";
}

const char *face_emotion_cn(face_emotion_t e)
{
    return ((int)e >= 0 && (int)e < FACE_COUNT) ? FACES[e].cn : "?";
}

uint8_t face_last_dirty_pages(void) { return s.dirty_pages; }

/* 强制下一帧整屏重发. 供「配网页 → 表情」切换时使用:
   配网页直接写过同一块显存, 让对方立刻整屏覆盖, 不留残影 */
void face_force_full(void) { s.force_full = true; }

/* ======================= 方向测试图 =======================
   三角朝上 = 上; 方块在左上角 = 左右没反; 底部横条 = 下。
   三角朝下 / 方块在左下角 就说明对应轴反了。 */
static void draw_test_pattern(void)
{
    ssd1305_clear();
    /* 大三角, 顶点朝上 */
    for (int y = 8; y <= 48; y++) {
        int half = (y - 8) * 18 / 40;
        for (int x = 64 - half; x <= 64 + half; x++) ssd1305_set_pixel(x, y, 1);
    }
    /* 左上角方块 */
    for (int y = 4; y <= 15; y++)
        for (int x = 4; x <= 15; x++) ssd1305_set_pixel(x, y, 1);
    /* 底部横条 */
    for (int y = 55; y <= 59; y++)
        for (int x = 30; x <= 98; x++) ssd1305_set_pixel(x, y, 1);
    ssd1305_display();
}

void face_set_test(bool on)
{
    s.test_mode = on;
    if (on) draw_test_pattern();
    ESP_LOGI(TAG, "test pattern %s", on ? "ON" : "OFF");
}

bool face_get_test(void) { return s.test_mode; }
