/* Rear camera screen: camera video (cam.c, top CAM_ROWS rows of the frame, full screen), Normal/Wide, Guidelines.
 * Design: Main.dc.html, is.camera; no frame - the design's placeholder. Lines are our own, sized like the stock unit's, by steering angle
 * (MCU 0x58; they match the wheels, owner; DEVIATIONS.md) instead of the design's lines. */
#include "ui_int.h"
#include "cam.h"
#include "mcu.h"

#include <math.h>

enum { H_NORMAL = HIT_SCREEN, H_WIDE, H_GUIDES };
static tween view_th, zoom, g_op, g_bg;

#define SEG_X (ST_X + ST_W - 1 - 24 - 234)   /* v26: right/top/bottom 24 */
#define SEG_Y (ST_Y + 1 + 24)
#define GB_Y (ST_Y + ST_H - 1 - 24 - 60)
#define GB_R (ST_X + ST_W - 1 - 24)

static float gb_w(void) { return 22 + 22 + 10 + t_width(700, 20, "Guidelines") + 22; }

static void layout(void)
{
    set_hit(H_NORMAL, SEG_X + 5, SEG_Y + 5, 112, 48);
    set_hit(H_WIDE, SEG_X + 117, SEG_Y + 5, 112, 48);
    set_hit(H_GUIDES, GB_R - gb_w(), GB_Y, gb_w(), 60);
}

static void draw_static(void)
{
    rrect_b(ST_X, ST_Y, ST_W, ST_H, PANEL_R, hexc(0x0B0D0C), C_BORDER);   /* title and Normal/Wide - in draw_dyn, over the frame */
}

/* lines sized like the stock cameradetect (geometry extracted from its images: tools/make_guides.py -> guides_tab.h),
 * in camera frame coordinates 720x480: static lines with labels (design colors) and wheel trajectories by steering angle -
 * frame f = 54 - angle/10 (as in the stock unit), points are interpolated between neighbouring frames, the angle is smoothed. */
#include "guides_tab.h"

/* where the camera frame is on screen: Normal - frame proportions (720:480, as in the stock unit), fit to height, margins at the sides;
 * Wide - full width (design zoom). Lines are in frame coordinates and move with it */
static float RX, RY, RW, RH;
static void cam_rect(float z)
{
    float nw = ST_H * 720.0f / CAM_ROWS;
    RW = nw + (ST_W - nw) * z; RH = ST_H;
    RX = ST_X + (ST_W - RW) * 0.5f; RY = ST_Y;
}
#define GX(x) (RX + (x) * RW / 720.0f)
#define GY(y) (RY + (y) * RH / (float)CAM_ROWS)
#define GK (RW / 720.0f)

static float steer_s;                    /* smoothed angle */
static double steer_t;

static uint32_t band(float y) { return y < 300 ? 0x32D74B : y < 380 ? 0xFFD60A : 0xFF453A; }

static void track_draw(float go, double now)
{
    mcu_state *m = mcu_st();
    float f, t, a = m->steer_ok ? (float)m->steer : 0;    /* MCU sends the angle only when the wheel turns - straight until then */
    int i, c, k, i0, i1;
    if (steer_t == 0) steer_s = a;
    else steer_s += (a - steer_s) * fminf(1, (float)(now - steer_t) * 12);
    steer_t = now;
    for (c = 0; c < 2; c++)                                   /* static lines */
        for (k = 0; k + 1 < GUIDE_N; k++) {
            float y0 = GUIDE_Y0 + k * GUIDE_STEP, y1 = y0 + GUIDE_STEP;
            if (GUIDE_SX[c][k] < 0 || GUIDE_SX[c][k + 1] < 0) continue;
            gfx_line(D(GX(GUIDE_SX[c][k] * 0.25f)), D(GY(y0)), D(GX(GUIDE_SX[c][k + 1] * 0.25f)), D(GY(y1)),
                     D(GK * 0.5f * (GUIDE_SW[c][k] + GUIDE_SW[c][k + 1])), hexa(band(y0), go));
        }
    for (i = 0; i < (int)(sizeof GUIDE_TICK / sizeof GUIDE_TICK[0]); i++) {
        const short *r = GUIDE_TICK[i];
        rrect(GX(r[2]), GY(r[0]), GX(r[3] + 1) - GX(r[2]), GY(r[1] + 1) - GY(r[0]), 0, hexa(band(r[0]), go));
    }
    f = 54 - steer_s / 10;
    f = f < 0 ? 0 : f > GUIDE_FRAMES - 1 ? GUIDE_FRAMES - 1 : f;
    i0 = (int)f; i1 = i0 + 1 < GUIDE_FRAMES ? i0 + 1 : i0; t = f - i0;
    for (c = 0; c < 2; c++) {                                 /* wheel trajectories */
        float px = -1, py = 0, pw = 0;
        for (k = 0; k < GUIDE_N; k++) {
            short a = GUIDE_X[i0][c][k], b = GUIDE_X[i1][c][k];
            float x, w, y = GUIDE_Y0 + k * GUIDE_STEP;
            if (a >= 0 && b >= 0) { x = 0.25f * (a + (b - a) * t); w = GUIDE_W[i0][c][k] + (GUIDE_W[i1][c][k] - GUIDE_W[i0][c][k]) * t; }
            else if (a >= 0 && t < 0.5f) { x = 0.25f * a; w = GUIDE_W[i0][c][k]; }
            else if (b >= 0 && t >= 0.5f) { x = 0.25f * b; w = GUIDE_W[i1][c][k]; }
            else { px = -1; continue; }
            if (px >= 0) gfx_line(D(GX(px)), D(GY(py)), D(GX(x)), D(GY(y)), D(GK * 0.5f * (pw + w)), hexa(0xFF9F0A, go));
            px = x; py = y; pw = w;
        }
    }
}

static void draw_dyn(double now)
{
    static const char *const VL[2] = { "Normal", "Wide" };
    float z = tw_get(&zoom, now), go = tw_get(&g_op, now), th = tw_get(&view_th, now);
    float bg = tw_get(&g_bg, now), k;
    int i;
    {
        int cw, chh;
        gfx_tex *t = cam_frame(&cw, &chh);
        cam_rect(z);
        if (t) gfx_image_round(t, D(RX), D(RY), D(RW), D(RH), 0, 0, 1, (float)CAM_ROWS / chh, C_WHITE, D(z > 0.99f ? PANEL_R : 0));
        else {   /* video placeholder: icon 96 + "Camera feed", scale .86 in Wide */
            float s = 1 - 0.14f * z;
            ico("camera_w12", 692, 336 - 16 * s, 96 * s, hexc(0x3A3D3B));
            t_center(600, 20, 692, 336 + 52 * s, "Camera feed", hexc(0x3A3D3B));
        }
    }
    if (go > 0.005f) track_draw(go, now);
    screen_head(PX0, PY0 + 29, 0x14A3A3, "app_camera_w22", "Rear camera", hexc(0xF5F5F7));   /* v26: no pill (no text-shadow) */
    rrect_b(SEG_X, SEG_Y, 234, 58, 29, hexa(0x16161A, 0.5f), hexa(0xFFFFFF, 0.12f));
    rrect(SEG_X + 5 + th * 112, SEG_Y + 5, 112, 48, 24, C_ACCENT);       /* v17: accent */
    for (i = 0; i < 2; i++) {
        float m = 1 - (th > i ? th - i : i - th);
        col c = col_mix(hexc(0xF5F5F7), C_ONACCENT, m < 0 ? 0 : m);
        k = press_k(H_NORMAL + i, now);
        t_center(700, 20, SEG_X + 5 + i * 112 + 56, SEG_Y + 29, VL[i], col_alpha(c, 1 - press_dim(k)));
    }
    {   /* Guidelines (v17): background accent / rgba(22,22,26,.5), color onAccent / #F5F5F7 */
        float x = GB_R - gb_w(), y = GB_Y, w = gb_w(), h = 60;
        col fg = col_mix(hexc(0xF5F5F7), C_ONACCENT, bg);
        k = press_box(H_GUIDES, &x, &y, &w, &h, now);
        rrect_b(x, y, w, h, 30 * k, col_mix(hexa(0x16161A, 0.5f), C_ACCENT, bg), hexa(0xFFFFFF, 0.12f));
        ico("guides", x + 22 * k + 11 * k, y + h * 0.5f, 22 * k, fg);
        t_left(700, 20, x + (22 + 22 + 10) * k, y + h * 0.5f, "Guidelines", fg);
    }
}

static void update(double now)
{
    if (cam_active() || mcu_st()->steer_ok) damage_d(ST_X, ST_Y, ST_W, ST_H);   /* camera frames, steering angle */
    if (tw_active(&zoom, now) || tw_active(&g_op, now)) damage_d(ST_X, ST_Y, ST_W, ST_H);
    if (tw_active(&view_th, now)) damage_d(SEG_X, SEG_Y, 234, 58);
    if (tw_active(&g_bg, now)) damage_d(1242 - gb_w(), GB_Y, gb_w(), 60);
}

static void click(int id, double now)
{
    if (id == H_NORMAL || id == H_WIDE) {
        A.wide = id == H_WIDE;
        tw_set(&view_th, (float)A.wide, SPRING_MS, &SPRING_SOFT, now);
        tw_set(&zoom, (float)A.wide, 700, &FAN, now);
    } else if (id == H_GUIDES) {
        A.guides = !A.guides;
        tw_set(&g_op, (float)A.guides, 400, &EASE, now);
        tw_set(&g_bg, (float)A.guides, 300, &EASE, now);
    }
}

static void enter(double now)
{
    (void)now;
    tw_init(&view_th, (float)A.wide); tw_init(&zoom, (float)A.wide);
    tw_init(&g_op, (float)A.guides); tw_init(&g_bg, (float)A.guides);
}

const screen_def SCR_CAMERA = { layout, draw_static, draw_dyn, update, click, enter };
