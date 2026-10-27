/* DESIGN TOKENS (Car Media design, dark theme). Everything that defines the look is edited here.
 * Sizes are in design pixels (1280x750); D(x) converts to 1024x600 screen pixels. */
#ifndef THEME_H
#define THEME_H

#include "gfx.h"

#define SCREEN_W 1024
#define SCREEN_H 600
#define K 0.8f
#define D(x) ((float)(x) * K)

/* colors: palette of the current theme (dark / light, from the design), switched by theme_set() */
typedef struct {
    col text, sub, muted, border, inner, track, inv, oninv, accent, onaccent, railact, seg, tick, ok;
    col glass_tint, rail_tint, hud_tint;
    float glass_a, rail_a, hud_a, glass_edge;
    float glass_blur, glass_sat;      /* wallpaper blur under glass: fraction of 30px, saturation (design c.blur) */
    col panel_shadow;               /* light theme: 0 10px 30px rgba(20,20,40,.10); dark: none */
    int light;
} palette;
extern palette PAL;
void theme_set(int light);
/* accent color (design v17: Blue, Teal, Green, Orange, Red, Pink, Purple) */
#define N_ACCENT 7
extern const char *const ACCENT_NAME[N_ACCENT];
extern int theme_accent;
uint32_t accent_rgb(int k, int light);
void theme_set_accent(int k);

#define C_TEXT      PAL.text
#define C_SUB       PAL.sub
#define C_MUTED     PAL.muted
#define C_BORDER    PAL.border
#define C_INNER     PAL.inner
#define C_TRACK     PAL.track
#define C_INV       PAL.inv
#define C_ONINV     PAL.oninv
#define C_ACCENT    PAL.accent
#define C_ONACCENT  PAL.onaccent
#define C_ACC_SOFT  col_alpha(PAL.accent, 0.16f)    /* c.accentSoft */
#define C_ACC_GLOW  col_alpha(PAL.accent, 0.6f)     /* c.accentGlow */
#define C_OK        PAL.ok          /* green status ("Connected", "GB free") */
#define C_RAILACT   PAL.railact
#define C_SEG       PAL.seg
#define C_TICK      PAL.tick
#define GLASS_TINT  PAL.glass_tint
#define GLASS_A     PAL.glass_a
#define RAIL_TINT   PAL.rail_tint
#define RAIL_A      PAL.rail_a
#define HUD_TINT    PAL.hud_tint
#define HUD_A       PAL.hud_a
#define GLASS_EDGE  PAL.glass_edge
/* app and climate colors - same in both themes */
#define C_MUSIC     hexc(0xFF4F6A)
#define C_VIDEO     hexc(0x7B61FF)
#define C_RADIO     hexc(0xFF8A2A)
#define C_CAMERA    hexc(0x14A3A3)
#define C_PHONE     hexc(0x2DBE6C)
#define C_COLD      hexc(0x3B82F6)
#define C_HOT       hexc(0xEF4444)
#define C_WHITE     hexc(0xFFFFFF)
static const uint32_t TEMPS[9] = { 0x2563EB, 0x3B82F6, 0x60A5FA, 0x93C5FD, 0xA1A1AA,
                                   0xFCA5A5, 0xF87171, 0xEF4444, 0xDC2626 };

/* geometry (design px) */
#define GAP          18
#define RAIL_W       104
#define BOTTOM_H     78
#define PANEL_R      28
#define PILL_H       60
#define PILL_R       30
#define PILL_BTN     50
#define RAIL_BTN     68
#define RAIL_BTN_R   18
#define TILE_S       32
#define TILE_S_R     9

/* animations (from the design) */
#define PRESS_SCALE  0.93f      /* press: scale(0.93) */
#define PRESS_DIM    0.12f      /*          brightness(0.88) */
#define PRESS_MS     120
#define SPRING_MS    450        /* cubic-bezier(.34,1.56,.64,1) */
#define ENTER_FADE_MS 420
#define ENTER_MOVE_MS 600       /* cubic-bezier(.2,.9,.25,1) */
#define LEAVE_MS     170
#define DRIFT_MS     8000
#define TICK_MS      250
#define HUD_TICKS    7

#endif
