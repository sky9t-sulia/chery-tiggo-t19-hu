/* Apps screen: 9 apps (6-per-row grid), divider, 4 settings. No wave appearance (design v35: op 1, tf none;
 * before v34 - opacity .5s + translateY(24) scale(.85) -> .6s spring, 35 ms delay per tile).
 * Design v21: is.apps / appDefs (slice 0..9 and 9..13), padding-top 30, row-gap 28, divider margin 28.
 * v33: CarPlay and Android Auto - image instead of icon (CarPlay - full tile, AA - 64 on white); HUD only for now.
 * Owner 2026-10-25: CarPlay and AA are unavailable without a phone - like the design's unavailable sources (opacity 0.4, not
 * clickable; DEVIATIONS.md). AA is available: a session is running (incl. wireless in the background) or a phone is in the CarPlay port. */
#include "ui_int.h"
#include "aa.h"
#include "aa_usb.h"
#include "aa_wifi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct { const char *label, *icon; uint32_t color; int screen; const char *img; float img_sz, alpha; const char *hud; } app_def;
#define N_APPS 9
#define N_ALL 13
static const app_def APPS[N_ALL] = {
    { "Music", "app_music", 0xFF4F6A, S_MUSIC },
    { "Video", "app_video", 0x7B61FF, S_VIDEO },
    { "Radio", "app_radio", 0xFF8A2A, S_RADIO },
    { "Rear Camera", "app_camera", 0x14A3A3, S_CAMERA },
    { "Phone", "app_phone", 0x2DBE6C, S_PHONE },
    { "USB Files", "app_usb", 0x2F7BF6, S_USB },
    { "Lua Apps", "app_lua", 0x3B5BDB, S_LUA },
    { "CarPlay", NULL, 0, -1, "app_carplay.png", 112, 0, "Connect an iPhone via USB" },             /* background - transparent */
    { "Android Auto", NULL, 0xFFFFFF, -1, "app_aauto.png", 64, 1, NULL },                           /* aa_view_open */
    { "Bluetooth", "app_bt", 0x1F9FD6, S_BT },
    { "Sound", "app_sound", 0xE0458F, S_SOUND },
    { "Display", "app_display", 0xC2410C, S_DISPLAY },
    { "Vehicle", "car_w18", 0x3B5BDB, S_VEHICLE },
};
static tween op[N_ALL], ent[N_ALL], av[N_ALL], sep;
static int avail_now[N_ALL];
#define I_CARPLAY 7
#define I_AA 8

static int avail(int i)
{
    if (i == I_CARPLAY) return aa_usb_phone(1);
    if (i == I_AA) return aa_state() != AA_IDLE || aa_usb_phone(0) || aa_wifi_phone() || aa_wifi_pending();
    return 1;
}
static gfx_tex img[N_ALL][2];                          /* [1] - gray (assets/..._gray.png) */
#define AV_OFF 0.38f                                    /* v36 iop of an unavailable one */

#define COL_W 182.0f
#define CELL_H (112 + 18 + t_lh(700, 23))   /* tile 112, gap 18, label 23/700 (line-height normal) */
#define ROW1 48.0f                                      /* ST_Y + 30 */
#define SEP_Y (ROW1 + 2 * CELL_H + 28 + 28)
#define ROW2 (SEP_Y + 1 + 28)
static float cx(int i) { return 146 + COL_W * ((i < N_APPS ? i % 6 : i - N_APPS) + 0.5f); }
static float ty(int i) { return i < N_APPS ? ROW1 + (i / 6) * (CELL_H + 28) : ROW2; }

static void layout(void)
{
    int i;
    for (i = 0; i < N_ALL; i++) set_hit(HIT_SCREEN + i, cx(i) - 70, ty(i), 140, CELL_H);
}

static void draw_dyn(double now)
{
    int i;
    float so = tw_get(&sep, now);
    if (so > 0.005f) rrect(166, SEP_Y, 1052, 1, 0, col_alpha(C_BORDER, so));
    for (i = 0; i < N_ALL; i++) {
        /* v36 unavailable (CarPlay / AA): tile grayscale(1) and opacity .38, no shadow, label - muted */
        float oa = tw_get(&op[i], now), av_ = tw_get(&av[i], now), d = (1 - av_) / (1 - AV_OFF), o = oa * av_;
        float e = tw_get(&ent[i], now), k = avail_now[i] ? press_k(HIT_SCREEN + i, now) : 1;
        float s = (0.85f + 0.15f * e) * k, dy = 24 * (1 - e);
        float x = cx(i), y = ty(i) + dy, tile = 112 * s;
        if (oa < 0.005f) continue;
        shadow(x - tile * 0.5f, y + 56 - tile * 0.5f, tile, tile, 30 * s, 30, 0, 12, hexa(0, 0.25f * oa * (1 - d)));
        if (!APPS[i].img || APPS[i].alpha > 0) rrect(x - tile * 0.5f, y + 56 - tile * 0.5f, tile, tile, 30 * s, hexa(APPS[i].color, o));
        if (!APPS[i].img) ico(APPS[i].icon, x, y + 56, 50 * s, hexa(0xFFFFFF, o));
        else {                                              /* object-fit: contain, tile overflow: hidden (v35: squircle 40) */
            float is = APPS[i].img_sz * s;
            int g;
            gfx_squircle(1);
            for (g = 0; g < 2; g++) {                       /* color -> gray cross-fade (transition filter) */
                float a = o * (g ? d : 1 - d);
                if (a > 0.005f && img[i][g].id)
                    gfx_image_round(&img[i][g], D(x - is * 0.5f), D(y + 56 - is * 0.5f), D(is), D(is), 0, 0, 1, 1, hexa(0xFFFFFF, a), D(is >= tile ? 40 * s : 0));
            }
            gfx_squircle(0);
        }
        if (k < 1) rrect(x - tile * 0.5f, y + 56 - tile * 0.5f, tile, tile, 30 * s, hexa(0, press_dim(k) * o));
        t_center(700, 23, x, y + 56 + (56 + 18 + t_lh(700, 23) * 0.5f) * s, APPS[i].label, col_alpha(col_mix(C_TEXT, C_MUTED, d), oa));
    }
}

static void update(double now)
{
    static double t;
    int i;
    if (now - t > 0.5) {                                    /* phone arrived/left */
        t = now;
        for (i = 0; i < N_ALL; i++) {
            int a = avail(i);
            if (a != avail_now[i]) { avail_now[i] = a; tw_set(&av[i], a ? 1 : AV_OFF, 300, &EASE, now); }
        }
    }
    if (any_active(av, N_ALL, now)) damage_d(ST_X, ST_Y, ST_W, ST_H);
    if (any_active(op, N_ALL, now) || any_active(ent, N_ALL, now) || tw_active(&sep, now)) damage_d(ST_X, ST_Y, ST_W, ST_H);
}

static void click(int id, double now)
{
    const app_def *a = &APPS[id - HIT_SCREEN];
    if (!avail_now[id - HIT_SCREEN]) return;             /* disabled */
    if (a->hud) hud_show(13, a->hud, now);               /* v33: "Can’t open · Connect an iPhone via USB" */
    else if (a->img) aa_view_open(now);                  /* Android Auto: no phone - HUD "Connect an Android phone" */
    else go_screen(a->screen, now);
}

static void enter(double now)
{
    int i;
    (void)now;
    char path[512];
    const char *dir = getenv("HU_ASSETS") ? getenv("HU_ASSETS") : "assets";
    for (i = 0; i < N_ALL; i++)
        if (APPS[i].img && !img[i][0].id) {
            size_t n;
            snprintf(path, sizeof path, "%s/%s", dir, APPS[i].img); gfx_tex_load(&img[i][0], path, 1);
            n = strlen(path); if (n > 4) { snprintf(path + n - 4, sizeof path - (n - 4), "_gray.png"); gfx_tex_load(&img[i][1], path, 1); }
        }
    for (i = 0; i < N_ALL; i++) {
        tw_init(&op[i], 1); tw_init(&ent[i], 1);   /* v35: in place immediately */
    }
    for (i = 0; i < N_ALL; i++) { avail_now[i] = avail(i); tw_init(&av[i], avail_now[i] ? 1 : AV_OFF); }
    tw_init(&sep, 1);                                      /* v35: sepOp 1 */
}

const screen_def SCR_APPS = { layout, NULL, draw_dyn, update, click, enter, NULL };
