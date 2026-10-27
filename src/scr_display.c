/* Display screen: close, reset, brightness (slider 1..10), Dark/Light theme, accent color (v17), 3x2 wallpapers (5 scenes + photo from USB).
 * Design: Main.dc.html, is.display. */
#include "ui_int.h"
#include "i18n.h"
#include "media.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define NT (N_SCENE + 1)          /* v26: 11 scenes + "From USB" */
enum { H_CLOSE = HIT_SCREEN, H_RESET, H_BRIGHT, H_AUTO, H_DARK, H_LIGHT, H_W0, H_ACC0 = H_W0 + NT };
static tween th_th, bright_f, sel[NT], selo[NT], acc_k[N_ACCENT];
static gfx_tex thumb[N_SCENE];
static int last_bright = -1;

#define X0 PX0                       /* v26: padding 24 */
#define Y0 PY0
#define COL_W ((PCW - 18) / 2)   /* v26: gap 18 */
#define COL2 (X0 + COL_W + 18)
#define BOTTOM PBOT

static float box_y(void) { return Y0 + 52 + 18 + t_lh(700, 19) + 10; }
static float acc_y(void) { return box_y() + 92 + 18 + t_lh(700, 19) + 10; }     /* v17: "Accent color" - strip 80 */
static float acc_x(int i) { return X0 + PCW - 20 - N_ACCENT * 56 - (N_ACCENT - 1) * 14 + i * 70; }
static float grid_y(void) { return acc_y() + 80 + 18 + t_lh(700, 19) - 8 + 18; }
static float tile_w(void) { return (PCW - 5 * 14) / 6.0f; }       /* v26: 6x2 grid, gap 14 */
static float tile_h(void) { return (BOTTOM - grid_y() - 14) * 0.5f; }
static float tile_x(int i) { return X0 + (i % 6) * (tile_w() + 14); }
static float tile_y(int i) { return grid_y() + (i / 6) * (tile_h() + 14); }
static float slider_y(void) { return box_y() + (92 - (t_lh(700, 20) + 6 + 40)) * 0.5f + t_lh(700, 20) + 6; }
#define SEG_X (COL2 + COL_W - 10 - 320)   /* v30: Auto | Dark | Light, 3 x 104 + 2 x 4 */
static float theme_pos(void) { return A.theme_auto ? 0 : A.light ? 2 : 1; }   /* position of the pill */

static void layout(void)
{
    int i;
    set_hit(H_CLOSE, X0, Y0, 52, 52);
    set_hit(H_RESET, X0 + PCW - reset_w(), Y0 + 2, reset_w(), 48);
    set_drag(H_BRIGHT, X0 + 20, slider_y() - 8, COL_W - 40, 56);
    for (i = 0; i < 3; i++) set_hit(H_AUTO + i, SEG_X + 4 + i * 104, box_y() + 22, 104, 48);
    for (i = 0; i < NT; i++) set_hit(H_W0 + i, tile_x(i), tile_y(i), tile_w(), tile_h());
    for (i = 0; i < N_ACCENT; i++) set_hit(H_ACC0 + i, acc_x(i), acc_y() + 12, 56, 56);
}

static void draw_static(void)
{
    float by = box_y();
    panel(ST_X, ST_Y, ST_W, ST_H, PANEL_R);
    head_static(X0, Y0, 0xC2410C, "photo_w22", 22, "Display");
    t_left(700, 19, X0, by - 10 - t_lh(700, 19) * 0.5f, "Brightness", C_SUB);
    t_left(700, 19, COL2, by - 10 - t_lh(700, 19) * 0.5f, "Theme", C_SUB);
    rrect(X0, by, COL_W, 92, 20, C_INNER);
    rrect(COL2, by, COL_W, 92, 20, C_INNER);
    t_fit(700, 21, COL2 + 20, by + 46, SEG_X - 14 - COL2 - 20,
          A.theme_auto ? (A.light ? "Auto \xC2\xB7 day" : "Auto \xC2\xB7 night") : A.light ? "Light" : "Dark", C_TEXT);
    seg_static2(SEG_X, by + 18, 3, 104, 48, 4, 0);
    t_left(700, 19, X0, acc_y() - 10 - t_lh(700, 19) * 0.5f, "Accent color", C_SUB);
    rrect(X0, acc_y(), PCW, 80, 20, C_INNER);
    t_left(700, 21, X0 + 20, acc_y() + 40, ACCENT_NAME[theme_accent], C_TEXT);
    t_left(700, 19, X0, grid_y() - 10 - t_lh(700, 19) * 0.5f, "Wallpaper", C_SUB);
}

static void pill(float x, float y, const char *s)        /* v26: tile label 28, padding 10, 15/700, rgba(0,0,0,.42) */
{
    float w = t_width(700, 15, s) + 20;
    rrect(x, y - 28, w, 28, 14, hexa(0, 0.42f));
    t_left(700, 15, x + 10, y - 14, s, hexc(0xF5F5F7));
}

/* v26: selected - 3 accent ring inside (box-shadow inset), no scale or shadow; others brightness(.72);
 * checkmark 26 (opacity .3s, scale .4 -> 1 spring) - scenes only */
static void draw_tile(int i, double now)
{
    float s = tw_get(&sel[i], now), o = tw_get(&selo[i], now), k = press_k(H_W0 + i, now);
    float x = tile_x(i), y = tile_y(i), w = tile_w(), h = tile_h();
    gfx_tex *t = i < N_SCENE ? &thumb[i] : media_wallpaper_thumb() ? media_wallpaper_thumb()   /* saved ones - also without a stick */
               : A.custom_img >= 0 ? media_thumb(A.custom_img) : NULL;
    x += w * (1 - k) * 0.5f; y += h * (1 - k) * 0.5f; w *= k; h *= k;
    if (t && t->id) {
        /* background: cover */
        float ar = w / h, ia = (float)t->w / t->h, uw = ar < ia ? ar / ia : 1, vh = ar < ia ? 1 : ia / ar;
        gfx_image_round(t, D(x), D(y), D(w), D(h), 0.5f - uw * 0.5f, 0.5f - vh * 0.5f, 0.5f + uw * 0.5f, 0.5f + vh * 0.5f, C_WHITE, D(20 * k));
        if (o < 0.995f) rrect(x, y, w, h, 20 * k, hexa(0, 0.28f * (1 - o)));          /* brightness(.72) */
    } else {                                                                       /* "From USB" without a photo: dashed 2, + 26 */
        dash_rect(x, y, w, h, 20 * k, C_TRACK);
        ico("plus_w20", x + w * 0.5f, y + h * 0.5f - 13, 26 * k, C_SUB);
    }
    if (o > 0.005f) gfx_rect_border(D(x), D(y), D(w), D(h), D(20 * k), hexa(0, 0), D(3), col_alpha(C_ACCENT, o));
    if (k < 1) rrect(x, y, w, h, 20 * k, hexa(0, press_dim(k)));
    pill(x + 8, y + h - 8, i < N_SCENE ? SCENE_NAME[i] : "From USB");
    if (o > 0.005f && i < N_SCENE) {
        float cs = 26 * (0.4f + 0.6f * s), cx = x + w - 8 - 13, cy = y + 8 + 13;
        rrect(cx - cs * 0.5f, cy - cs * 0.5f, cs, cs, cs * 0.5f, hexa(0xF5F5F7, o));
        ico("check", cx, cy, 15 * (0.4f + 0.6f * s), hexa(0, o));
    }
}

static void draw_dyn(double now)
{
    static const char *IC[3] = { "auto_theme", "moon", "sun" }, *LB[3] = { "Auto", "Dark", "Light" };
    char v[8];
    float by = box_y(), th = tw_get(&th_th, now);
    int i;
    close_btn(H_CLOSE, X0, Y0, now);
    reset_btn(H_RESET, X0 + PCW - reset_w(), Y0 + 2, now);
    snprintf(v, sizeof v, "%d", A.bright);
    slider_row(X0 + 20, slider_y() - 6 - t_lh(700, 20), COL_W - 40, "Screen brightness", v, tw_get(&bright_f, now), "sun", C_TEXT);
    rrect(SEG_X + 4 + th * 104, by + 22, 104, 48, 24, C_ACCENT);
    for (i = 0; i < 3; i++) {
        float m = 1 - fminf(1, fabsf(th - i)), k = press_k(H_AUTO + i, now);
        float cx = SEG_X + 4 + i * 104 + 52, tw = (18 + 8 + t_width(700, 20, LB[i])) * k;
        col c = col_alpha(col_mix(C_TEXT, C_ONACCENT, m), 1 - press_dim(k));
        ico(IC[i], cx - tw * 0.5f + 9 * k, by + 46, 18 * k, c);
        {
            font *f = F(700, 20);
            text_draw(f, D(cx - tw * 0.5f + 26 * k), D(by + 46) - text_line_height(f) * 0.5f, tr(LB[i]), c, 0);
        }
    }
    for (i = 0; i < NT; i++) draw_tile(i, now);
    for (i = 0; i < N_ACCENT; i++) {   /* circle 44: selected - 3px ring (background) + 3px (color), scale .86 -> 1, checkmark */
        float s = tw_get(&acc_k[i], now) * press_k(H_ACC0 + i, now), cx = acc_x(i) + 28, cy = acc_y() + 40, r = 22 * s;
        float on = (tw_get(&acc_k[i], now) - 0.86f) / 0.14f;
        col c = hexc(accent_rgb(i, A.light));
        if (on > 0.01f) {
            rrect(cx - r - 6, cy - r - 6, (r + 6) * 2, (r + 6) * 2, r + 6, col_alpha(c, on));
            rrect(cx - r - 3, cy - r - 3, (r + 3) * 2, (r + 3) * 2, r + 3, col_alpha(GLASS_TINT, on));
        }
        rrect(cx - r, cy - r, r * 2, r * 2, r, c);
        if (on > 0.01f) ico("check", cx, cy, 22 * s, col_alpha(C_ONACCENT, on));
    }
}

static void sel_state(double now)
{
    int i;
    for (i = 0; i < NT; i++) {
        int on = A.scene == i;                           /* last tile - SCENE_CUSTOM */
        tw_set(&sel[i], on ? 1 : 0, 500, &SPRING, now);
        tw_set(&selo[i], on ? 1 : 0, 350, &EASE, now);
    }
}

static void acc_state(double now)
{
    int i;
    for (i = 0; i < N_ACCENT; i++) tw_set(&acc_k[i], i == theme_accent ? 1 : 0.86f, 400, &SPRING, now);
}

static void update(double now)
{
    if (A.bright != last_bright) { last_bright = A.bright; tw_set(&bright_f, (A.bright - 1) / 9.0f, 180, &EASE_OUT, now); }
    if (tw_active(&bright_f, now)) damage_d(X0, box_y(), COL_W, 92);
    {   /* theme changed elsewhere (panel button, Auto by clock/headlights) - pill and label follow it */
        static float last_pos = -1;
        float p = theme_pos();
        if (p != last_pos) { last_pos = p; tw_set(&th_th, p, SPRING_MS, &SPRING_SOFT, now); screen_dirty(); }
    }
    if (tw_active(&th_th, now)) damage_d(SEG_X - 8, box_y(), 320 + 16, 92);   /* whole switch + margin for the spring */
    if (any_active(sel, 6, now) || any_active(selo, 6, now)) damage_d(X0 - 20, grid_y() - 12, 1114, BOTTOM - grid_y() + 40);
    if (any_active(acc_k, N_ACCENT, now) || press_active(now)) damage_d(X0, acc_y(), PCW, 80);
}

static void drag(int id, float x, float y, int phase, double now)
{
    (void)y; (void)now;
    if (id == H_BRIGHT && phase < 2) set_bright(1 + (int)lroundf(slider_value(X0 + 20, COL_W - 40, x) * 9));
}

static void click(int id, double now)
{
    if (id == H_CLOSE) close_screen(now);
    else if (id == H_RESET) {
        A.theme_auto = 0; set_bright(8); set_theme(0, now); set_scene(0, now); set_accent(0);
        hud_show(4, NULL, now);
    } else if (id == H_AUTO) { A.theme_auto = 1; theme_auto_apply(now); }
    else if (id == H_DARK || id == H_LIGHT) { A.theme_auto = 0; set_theme(id == H_LIGHT, now); }
    else if (id >= H_W0 && id < H_W0 + N_SCENE) set_scene(id - H_W0, now);
    else if (id >= H_ACC0 && id < H_ACC0 + N_ACCENT) set_accent(id - H_ACC0);
    else if (id == H_W0 + N_SCENE) {
        if ((A.custom_img >= 0 || media_wallpaper_thumb()) && A.scene != SCENE_CUSTOM) set_scene(SCENE_CUSTOM, now);
        else { A.usb_path = 3; A.usb_img = -1; go_screen(S_USB, now); }
    }
    tw_set(&th_th, theme_pos(), SPRING_MS, &SPRING_SOFT, now);
    sel_state(now);
    acc_state(now);
    screen_dirty();
}

static void enter(double now)
{
    int i;
    char path[512];
    const char *dir = getenv("HU_ASSETS") ? getenv("HU_ASSETS") : "assets";
    tw_init(&th_th, theme_pos());
    tw_init(&bright_f, (A.bright - 1) / 9.0f); last_bright = A.bright;
    for (i = 0; i < NT; i++) { tw_init(&sel[i], 0); tw_init(&selo[i], 0); }
    sel_state(now);
    acc_state(now);
    for (i = 0; i < NT; i++) { tw_init(&sel[i], sel[i].to); tw_init(&selo[i], selo[i].to); }
    for (i = 0; i < N_ACCENT; i++) tw_init(&acc_k[i], acc_k[i].to);
    for (i = 0; i < N_SCENE; i++)
        if (!thumb[i].id) {                                     /* thumbnails are loaded on first entry */
            snprintf(path, sizeof path, "%s/bg_%s_thumb.png", dir, SCENE_FILE[i]);
            gfx_tex_load(&thumb[i], path, 0);
        }
}

void display_shutdown(void)
{
    int i;
    for (i = 0; i < N_SCENE; i++) gfx_tex_free(&thumb[i]);
}

const screen_def SCR_DISPLAY = { layout, draw_static, draw_dyn, update, click, enter, drag };
