/* Video screen: title + "Full screen", player (tap - pause), slider, -10 / prev / play / next / +10,
 * video list on the right. Full-screen mode is on top of everything (the core draws and handles it via video_fs_*):
 * the control panel hides after 14 ticks (3.5 s) of playback, a tap shows/hides it.
 * Design: Main.dc.html, is.video + "Full screen video". */
#include "ui_int.h"
#include "i18n.h"
#include "media.h"

#include <math.h>
#include <stdio.h>

enum { H_FULL = HIT_SCREEN, H_PLAYER, H_SEEK, H_BACK, H_PREV, H_PLAY, H_NEXT, H_FWD, H_LIST, H_BROWSE, H_ITEM0 };
#define SLOTS 8                    /* buttons of visible list rows (circular) */
static tween ov_op, ov_k, item_bg, fs_op, fs_k, fs_ctl;   /* item_bg - highlight of the selected one (.3s) */
static scroller sc;
static int slot_row[SLOTS], last_gen = -1;
static float last_off = -1;
static int last_sec = -1, last_sel = -1, last_ctl = -1, last_usb = -1, last_none = -1, last_loaded = -1;

#define X0 PX0                     /* v26: padding 24 */
#define Y0 PY0
#define ROW_Y (Y0 + 58 + 18)
#define LIST_W 440.0f
#define ROW_H (PBOT - ROW_Y)
#define COL_W (PCW - 28 - LIST_W)
#define PL_H (ROW_H - (22 + 6 + 10 + t_lh(600, 19) + 10 + 88))   /* player: the rest - slider, time, buttons */
#define LIST_X (X0 + PCW - LIST_W)

static float bar_y(void) { return ROW_Y + PL_H + 22; }
static float ctl_cy(void) { return bar_y() + 6 + 10 + t_lh(600, 19) + 10 + 44; }
static float ctl_cx(int i)
{
    static const float S[5] = { 64, 72, 88, 72, 64 };
    float g = (COL_W - 360) / 4, x = X0;
    int k;
    for (k = 0; k < i; k++) x += S[k] + g;
    return x + S[i] * 0.5f;
}
static int draw_frame(float x, float y, float w, float h, float r, int bars);
/* v24 vNone: stick present, no videos on it (after scanning) */
static int v_none(void) { return A.usb_conn && !NVIDEOS && !media_scanning(); }
static float browse_w(void) { return 20 + 22 + 10 + t_width(800, 20, "Browse USB Files") + 26; }
static float none_y(void)                                   /* top of the block, centered below "Video" */
{
    float top = Y0 + 52, h = 96 + 12 + 6 + t_lh(800, 34) + 12 + t_lh(600, 21) + 12 + 14 + 56;   /* below the header 52 */
    return top + (PBOT - top - h) * 0.5f;
}
static float browse_y(void) { return none_y() + 96 + 12 + 6 + t_lh(800, 34) + 12 + t_lh(600, 21) + 12 + 14; }
static float full_w(void) { return 20 + 20 + 10 + t_width(700, 19, "Full screen") + 20; }
static float list_y(void) { return ROW_Y + 16 + t_lh(700, 24) + 2 + t_lh(600, 20) + 10; }

static void layout(void)
{
    static const float S[5] = { 64, 72, 88, 72, 64 };
    int i;
    if (!A.usb_conn) return;                                     /* v14: without a stick everything is inactive */
    if (v_none()) { set_hit(H_BROWSE, X0 + PCW * 0.5f - browse_w() * 0.5f, browse_y(), browse_w(), 56); return; }
    set_hit(H_FULL, X0 + PCW - full_w(), Y0 + 3, full_w(), 52);
    set_hit(H_PLAYER, X0, ROW_Y, COL_W, PL_H);
    set_drag(H_SEEK, X0 - 10, bar_y() - 13, COL_W + 20, 32);
    for (i = 0; i < 5; i++) set_hit(H_BACK + i, ctl_cx(i) - S[i] * 0.5f, ctl_cy() - S[i] * 0.5f, S[i], S[i]);
    set_scroll(H_LIST, LIST_X, list_y(), LIST_W, ROW_Y + ROW_H - list_y());
    for (i = 0; i < SLOTS; i++) slot_row[i] = -1;
    for (i = (int)(sc.off / 82); i < NVIDEOS && i < (int)(sc.off / 82) + SLOTS; i++) {   /* visible part of the row */
        float y0 = list_y() + i * 82 - sc.off, y1 = y0 + 80, bot = ROW_Y + ROW_H;
        if (y0 < list_y()) y0 = list_y();
        if (y1 > bot) y1 = bot;
        if (y1 <= y0) continue;
        slot_row[i % SLOTS] = i;
        set_hit(H_ITEM0 + i % SLOTS, LIST_X + 10, y0, LIST_W - 20, y1 - y0);
    }
}

static void draw_static(void)
{
    char cnt[40];
    panel(ST_X, ST_Y, ST_W, ST_H, PANEL_R);
    if (v_none()) {                                             /* v24: "Video", circle 96, title, caption; button in draw_dyn */
        float y = none_y(), cx = X0 + PCW * 0.5f, l1 = t_lh(800, 34), l2 = t_lh(600, 21);
        screen_head(X0, Y0 + 26, 0x7B61FF, "app_video_w22", "Video", C_TEXT);   /* v26 */
        rrect(cx - 48, y, 96, 96, 48, C_ACC_SOFT);
        ico("video_w18", cx, y + 48, 46, C_ACCENT);
        y += 96 + 12 + 6;
        t_center(800, 34, cx, y + l1 * 0.5f, "No videos on USB drive", C_TEXT);
        t_center(600, 21, cx, y + l1 + 12 + l2 * 0.5f, "Add MP4, MKV or AVI files to the drive and plug it in again.", C_SUB);
        return;
    }
    screen_head(X0, Y0 + 29, 0x7B61FF, "app_video_w22", "Video", C_TEXT);   /* v26: header 58 - tile and "Video" (the video title is no longer here) */
    rrect(LIST_X, ROW_Y, LIST_W, ROW_H, 20, C_INNER);
    t_top(700, 24, LIST_X + 22, ROW_Y + 16, "Videos", C_SUB, 0);
    if (A.usb_conn) snprintf(cnt, sizeof cnt, tr_n("%d videos \xC2\xB7 USB drive", (long)(NVIDEOS)), NVIDEOS);
    else cnt[0] = 0;                                            /* v24: without a stick - empty */
    t_top(600, 20, LIST_X + 22, ROW_Y + 16 + t_lh(700, 24) + 2, cnt, C_MUTED, 0);
    if (!A.usb_conn) {   /* v14: list placeholder - circle 64, "No USB drive", caption */
        float top = list_y(), bot = ROW_Y + ROW_H, l1 = t_lh(700, 22), l2 = t_lh(600, 18);
        float h = 64 + 10 + l1 + 10 + l2, y = top + (bot - top - h) * 0.5f, cx = LIST_X + LIST_W * 0.5f;
        rrect(cx - 32, y, 64, 64, 32, C_SEG);
        ico("usb_full", cx, y + 32, 30, C_SUB);
        t_center(700, 22, cx, y + 74 + l1 * 0.5f, "No USB drive", C_TEXT);
        t_center(600, 18, cx, y + 84 + l1 + l2 * 0.5f, "Insert a USB drive with videos", C_SUB);
    }
}

static void draw_dyn(double now)
{
    const video_t *v = vid_at(A.vsel);
    if (v_none()) {                                             /* "Browse USB Files" button 56, accent */
        float x = X0 + PCW * 0.5f - browse_w() * 0.5f, y = browse_y(), w = browse_w(), h = 56, k = press_box(H_BROWSE, &x, &y, &w, &h, now);
        col fg = col_alpha(C_ONACCENT, 1 - press_dim(k));
        rrect(x, y, w, h, 28 * k, C_ACCENT);
        ico("app_usb_w22", x + (20 + 11) * k, y + h * 0.5f, 22 * k, fg);
        t_left(800, 20, x + (20 + 22 + 10) * k, y + h * 0.5f, "Browse USB Files", fg);
        return;
    }
    char a[16], b[24];
    float frac = (v->len > 0 ? (float)(A.vpos / v->len) : 0), k, o = tw_get(&ov_op, now), by = bar_y(), cy = ctl_cy();
    int i;
    gfx_xform_push(0, 0, 1, 0, A.usb_conn ? 1 : 0.35f);          /* v14: without a stick the button and player are 0.35 */
    {
        float x = X0 + PCW - full_w(), y = Y0 + 3, w = full_w(), h = 52;
        col c;
        k = press_box(H_FULL, &x, &y, &w, &h, now);
        c = col_alpha(C_TEXT, 1 - press_dim(k));
        rrect_b(x, y, w, h, 26 * k, C_SEG, C_BORDER);
        ico("fullscreen_w22", x + 30 * k, y + h * 0.5f, 20 * k, c);
        t_left(700, 19, x + 50 * k, y + h * 0.5f, "Full screen", c);
    }
    {   /* player */
        float x = X0, y = ROW_Y, w = COL_W, h = PL_H;
        k = press_box(H_PLAYER, &x, &y, &w, &h, now);
        shadow(x, y, w, h, 22 * k, 56, 0, 24, hexa(0, 0.35f));
        rrect(x, y, w, h, 22 * k, hexc(0x0E0E10));
        draw_frame(x, y, w, h, 22 * k, 0);                  /* no frame - just dark (the design's icon was a demo) */
        if (o > 0.005f) {
            float s = (0.7f + 0.3f * tw_get(&ov_k, now)) * k;
            rrect(x + w * 0.5f - 48 * s, y + h * 0.5f - 48 * s, 96 * s, 96 * s, 48 * s, hexa(0xF5F5F7, 0.2f * o));
            ico("play", x + w * 0.5f, y + h * 0.5f, 42 * s, hexa(0xF5F5F7, o));
        }
    }
    rrect(X0, by, COL_W, 6, 3, C_TRACK);
    if (!A.usb_conn) frac = 0;
    rrect(X0, by, COL_W * frac, 6, 3, C_TEXT);
    rrect(X0 + 10 + (COL_W - 20) * fminf(1, frac) - 10, by - 7, 20, 20, 10, C_TEXT);   /* v25: thumb inside the track */
    if (A.usb_conn && video_loaded()) { fmt_time(a, sizeof a, A.vpos); fmt_time(b, sizeof b, v->len); }   /* v14: duration on the right */
    else { snprintf(a, sizeof a, "\xE2\x80\x93:\xE2\x80\x93\xE2\x80\x93"); snprintf(b, sizeof b, "%s", a); }
    t_left(600, 19, X0, by + 16 + t_lh(600, 19) * 0.5f, a, C_SUB);
    t_right(600, 19, X0 + COL_W, by + 16 + t_lh(600, 19) * 0.5f, b, C_SUB);
    {
        static const char *IC[5] = { "back10", "prev", NULL, "next", "fwd10" };
        static const float IS[5] = { 30, 38, 0, 38, 30 };
        for (i = 0; i < 5; i++) {
            k = press_k(H_BACK + i, now);
            if (i == 2) {
                rrect(ctl_cx(2) - 44 * k, cy - 44 * k, 88 * k, 88 * k, 44 * k, col_mix(C_INV, hexc(0), press_dim(k)));
                ico(A.vplaying && A.usb_conn ? "pause" : "play", ctl_cx(2), cy, (A.vplaying && A.usb_conn ? 36 : 38) * k, C_ONINV);
            } else ico(IC[i], ctl_cx(i), cy, IS[i] * k, col_alpha(C_TEXT, 1 - press_dim(k)));
        }
    }
    gfx_xform_pop();
    clip_push(LIST_X, list_y(), LIST_W, ROW_Y + ROW_H - list_y());
    for (i = (int)(sc.off / 82); i < NVIDEOS && i < (int)(sc.off / 82) + SLOTS && A.usb_conn; i++) {   /* list (without a stick - empty) */
        float x = LIST_X + 10, y = list_y() + i * 82 - sc.off, w = LIST_W - 20, h = 80, bg = i == A.vsel ? tw_get(&item_bg, now) : 0;
        float h1 = t_lh(700, 25), h2 = t_lh(600, 21), ty, lw;
        col vio = C_ACCENT;                         /* v17: selected video - accent */
        k = press_box(H_ITEM0 + i % SLOTS, &x, &y, &w, &h, now);
        ty = y + h * 0.5f - (h1 + 2 + h2) * 0.5f;
        if (VIDEOS[i].len > 0) fmt_time(a, sizeof a, VIDEOS[i].len); else a[0] = 0;
        lw = t_width(700, 22, a);
        if (bg > 0.005f) rrect(x, y, w, h, 16 * k, col_alpha(C_INNER, bg));
        rrect(x + 14 * k, y + 12 * k, 56 * k, 56 * k, 13 * k, col_mix(C_ACC_SOFT, vio, bg));
        ico("video", x + 42 * k, y + h * 0.5f, 26 * k, col_mix(vio, C_ONACCENT, bg));
        t_fit(700, 25, x + 84 * k, ty + h1 * 0.5f, w - 84 - 14 - lw - 14, VIDEOS[i].name,
              col_alpha(col_mix(C_TEXT, vio, bg), 1 - press_dim(k)));
        t_left(600, 21, x + 84 * k, ty + h1 + 2 + h2 * 0.5f, "Video file \xC2\xB7 USB", C_SUB);
        t_right(700, 22, x + w - 14 * k, y + h * 0.5f, a, C_SUB);
    }
    clip_pop();
}

/* video frame whole in the rectangle, radius r; bars - fill the margins black (full screen: nothing under the video).
 * 0 - no frame */
static int draw_frame(float x, float y, float w, float h, float r, int bars)
{
    float u1, v1, fw, fh, fx, fy;
    int vw, vh;
    gfx_tex *t;
    if (!A.usb_conn || !(t = vplayer_frame(&u1, &v1, &vw, &vh))) return 0;
    fw = w; fh = w * vh / vw;
    if (fh > h) { fh = h; fw = h * vw / vh; }
    fx = x + (w - fw) * 0.5f; fy = y + (h - fh) * 0.5f;
    if (bars) {
        if (fy > y) { rrect(x, y, w, fy - y + 1, 0, hexc(0)); rrect(x, fy + fh - 1, w, y + h - fy - fh + 1, 0, hexc(0)); }
        if (fx > x) { rrect(x, y, fx - x + 1, h, 0, hexc(0)); rrect(fx + fw - 1, y, x + w - fx - fw + 1, h, 0, hexc(0)); }
    }
    gfx_image_round(t, D(fx), D(fy), D(fw), D(fh), 0, 0, u1, v1, C_WHITE, D(r));
    return 1;
}

int video_fs_covers(double now) { return A.vfull && tw_get(&fs_op, now) > 0.999f && tw_get(&fs_k, now) > 0.999f; }

static void state(double now)
{
    int i;
    int pl = A.vplaying && A.usb_conn;                      /* v14: doesn't play without a stick */
    tw_set(&ov_op, pl ? 0 : 1, 350, &EASE, now);
    tw_set(&ov_k, pl ? 0 : 1, 450, &SPRING, now);
    (void)i;
}

static void update(double now)
{
    {   /* slider - only when it moved by a pixel (otherwise a redraw on every loop pass) */
        static int last_px = -1;
        const video_t *v = vid_at(A.vsel);
        int px = v->len > 0 ? (int)(COL_W * A.vpos / v->len) : 0;
        if (px != last_px) { last_px = px; damage_d(X0 - 12, bar_y() - 10, COL_W + 24, 28); }
    }
    if (vplayer_new_frame() && !A.vfull) damage_d(X0, ROW_Y, COL_W, PL_H);
    if ((int)A.vpos != last_sec || video_loaded() != last_loaded) {   /* time / "-:--" (video closed) */
        last_sec = (int)A.vpos; last_loaded = video_loaded();
        damage_d(X0, bar_y() + 8, COL_W, 30);
    }
    if (A.vsel != last_sel) {
        last_sel = A.vsel; tw_init(&item_bg, 0); tw_set(&item_bg, 1, 300, &EASE, now); screen_dirty();
        if (!sc.drag) {                             /* the selected video left the visible part - scroll to it */
            float winh = ROW_Y + ROW_H - list_y(), y = A.vsel * 82;
            if (y < sc.off || y + 80 > sc.off + winh) {
                sc.off = fminf(sc.max, fmaxf(0, y - (winh - 80) * 0.5f)); sc.v = 0;
                layout_again();
            }
        }
    }
    if (media_gen() != last_gen) {                  /* stick: new list */
        last_gen = media_gen();
        sc.max = NVIDEOS * 82 - 2 - (ROW_Y + ROW_H - list_y()); if (sc.max < 0) sc.max = 0;
        if (sc.off > sc.max) sc.off = sc.max;
        screen_dirty(); layout_again(); damage_d(ST_X, ST_Y, ST_W, ST_H);
    }
    if (sc_update(&sc, now) || sc.off != last_off) { last_off = sc.off; damage_d(LIST_X, ROW_Y, LIST_W, ROW_H); layout_again(); }
    if (A.usb_conn != last_usb || v_none() != last_none) {   /* stick / "no videos" after scanning */
        last_usb = A.usb_conn; last_none = v_none();
        screen_dirty(); layout_again(); damage_d(ST_X, ST_Y, ST_W, ST_H);
    }
    state(now);
    if (tw_active(&ov_op, now) || tw_active(&ov_k, now) || tw_active(&item_bg, now) || press_active(now))
        damage_d(ST_X, ST_Y, ST_W, ST_H);
}

static void vtoggle(double now) { A.vplaying = !A.vplaying; state(now); }

static void click(int id, double now)
{
    if (id == H_BROWSE) { A.usb_path = 0; A.usb_img = -1; go_screen(S_USB, now); }
    else if (id == H_FULL) { A.vfull = 1; A.vctl_t = now; video_fs_layout(now); }
    else if (id == H_PLAYER || id == H_PLAY) vtoggle(now);
    else if (id == H_BACK) A.vpos = fmax(0, A.vpos - 10);
    else if (id == H_FWD) A.vpos = fmin(vid_at(A.vsel)->len, A.vpos + 10);
    else if (id == H_PREV) { if (A.vpos > 3) A.vpos = 0; else if (NVIDEOS) { A.vsel = (A.vsel + NVIDEOS - 1) % NVIDEOS; A.vpos = 0; A.vplaying = 1; } }
    else if (id == H_NEXT) { if (NVIDEOS) { A.vsel = (A.vsel + 1) % NVIDEOS; A.vpos = 0; A.vplaying = 1; } }
    else if (id >= H_ITEM0 && id < H_ITEM0 + SLOTS && slot_row[id - H_ITEM0] >= 0) { A.vsel = slot_row[id - H_ITEM0]; A.vpos = 0; A.vplaying = 1; }
    state(now);
    damage_d(ST_X, ST_Y, ST_W, ST_H);
}

static void enter(double now)
{
    int i;
    last_sec = last_sel = -1;
    state(now);
    tw_init(&ov_op, ov_op.to); tw_init(&ov_k, ov_k.to);
    (void)i;
    tw_init(&item_bg, 1); last_gen = -1; last_off = -1; sc.v = 0; sc.drag = 0;
}

static void drag(int id, float x, float y, int phase, double now)
{
    if (id == H_LIST) { sc_drag(&sc, y, phase, now); return; }
    if (id == H_SEEK) { A.vpos = floor(slider_value(X0, COL_W, x) * vid_at(A.vsel)->len); A.vdrag = phase < 2; }   /* seek - on release */
}

/* steering wheel: previous/next - player buttons, hold - seek -10/+10 s */
static int hw_btn(int d, int held)
{
    if (!NVIDEOS) return -1;
    return held ? (d > 0 ? H_FWD : H_BACK) : d > 0 ? H_NEXT : H_PREV;
}

const screen_def SCR_VIDEO = { layout, draw_static, draw_dyn, update, click, enter, drag, hw_btn };

/* ================================================================= full screen (1280x750 design) */

enum { F_TAP = HIT_FS, F_EXIT, F_BACK, F_PLAY, F_FWD, F_SEEK, F_EXIT2, F_BRIGHT, F_VOL };
#define FS_SEEK_Y 630.0f
#define FS_ROW_CY 696.0f
/* v23: vertical brightness (left) and volume (right) sliders: 64x280, a 20/800 number above them */
#define FS_SL_W 64.0f
#define FS_SL_H 280.0f
static float fs_sl_y(void) { return 375 - 170 + t_lh(800, 20) + 10; }
static tween fs_bh, fs_vh;                                        /* fill height (.15s ease-out) */
static int fs_was, fs_b0, fs_v0, fs_m0;                           /* brightness/volume before full screen - restore on exit */

static int ctl_on(double now) { return !A.vplaying || now - A.vctl_t < 3.5; }

void video_fs_layout(double now)
{
    int i;
    for (i = 0; i < 10; i++) set_hit(HIT_FS + i, 0, 0, 0, 0);
    if (!A.vfull) return;
    set_hit(F_TAP, 0, 0, 1280, 750);
    if (!ctl_on(now)) return;
    set_hit(F_EXIT, 24, 24, 56, 56);   /* v26: padding 24 */
    set_hit(F_BACK, 430, 375 - 42, 84, 84);
    set_hit(F_PLAY, 578, 375 - 62, 124, 124);
    set_hit(F_FWD, 766, 375 - 42, 84, 84);
    set_drag(F_SEEK, 20, FS_SEEK_Y - 8, 1240, 48);
    set_hit(F_EXIT2, 1198, FS_ROW_CY - 26, 52, 52);
    set_drag(F_BRIGHT, 34, fs_sl_y(), FS_SL_W, FS_SL_H);
    set_drag(F_VOL, 1280 - 34 - FS_SL_W, fs_sl_y(), FS_SL_W, FS_SL_H);
}

void video_fs_update(double now)
{
    int c = A.vfull && ctl_on(now);
    if (A.vfull != fs_was) {                                       /* enter: remember; exit: restore (user's decision) */
        fs_was = A.vfull;
        if (A.vfull) { fs_b0 = A.bright; fs_v0 = A.vol; fs_m0 = A.muted; }
        else { set_bright(fs_b0); A.vol = fs_v0; A.muted = fs_m0; }
    }
    tw_set(&fs_bh, (A.bright - 1) / 9.0f, 150, &EASE_OUT, now);
    tw_set(&fs_vh, A.muted ? 0 : A.vol / 31.0f, 150, &EASE_OUT, now);
    if (A.vfull && (tw_active(&fs_bh, now) || tw_active(&fs_vh, now))) damage_full();
    tw_set(&fs_op, (float)A.vfull, 350, &EASE, now);
    tw_set(&fs_k, (float)A.vfull, 450, &ENTER, now);
    tw_set(&fs_ctl, (float)c, 350, &EASE, now);
    if (c != last_ctl) { last_ctl = c; video_fs_layout(now); }
    if (tw_active(&fs_op, now) || tw_active(&fs_k, now) || tw_active(&fs_ctl, now) || (A.vfull && press_active(now)))
        damage_full();
    else if (A.vfull && vplayer_new_frame()) damage_full();
    else if (A.vfull && c) {                                       /* slider - when it moved by a pixel */
        static int last_px = -1;
        const video_t *v = vid_at(A.vsel);
        int px = v->len > 0 ? (int)(1220 * A.vpos / v->len) : 0;
        if (px != last_px) { last_px = px; damage_d(0, 560, 1280, 190); }
    }
}

static void fs_btn(int id, float cx, float cy, float d, col bg, const char *icon, float isz, double now)
{
    float k = press_k(id, now), r = d * 0.5f * k;
    rrect(cx - r, cy - r, r * 2, r * 2, r, col_mix(bg, hexa(0, bg.a), press_dim(k)));
    ico(icon, cx, cy, isz * k, hexc(0xF5F5F7));
}

void video_fs_draw(double now)
{
    const video_t *v = vid_at(A.vsel);
    float o = tw_get(&fs_op, now), s = 1.04f - 0.04f * tw_get(&fs_k, now), co = tw_get(&fs_ctl, now);
    float frac = (v->len > 0 ? (float)(A.vpos / v->len) : 0);
    char a[16], b[24];
    if (o < 0.005f) return;
    gfx_xform_push(D(640), D(375), s, 0, o);
    if (!video_fs_covers(now)) rrect(0, 0, 1280, 750, 0, hexc(0));   /* enter/exit: black over the screen */
    if (!draw_frame(0, 0, 1280, 750, 0, 1) && video_fs_covers(now)) rrect(0, 0, 1280, 750, 0, hexc(0));
    if (co > 0.005f) {
        gfx_xform_push(0, 0, 1, 0, co);
        gfx_vgrad(0, 0, D(1280), D(142), hexa(0, 0.75f), 1);
        gfx_vgrad(0, D(560), D(1280), D(190), hexa(0, 0.8f), 0);
        {
            float k = press_k(F_EXIT, now), r = 28 * k;
            rrect_b(52 - r, 52 - r, r * 2, r * 2, r, hexa(0x1E1E22, 0.55f), hexa(0xFFFFFF, 0.14f));
            ico("chev_l_w22", 52, 52, 26 * k, hexc(0xF5F5F7));
        }
        {
            float h1 = t_lh(800, 24), h2 = t_lh(600, 19), y = 52 - (h1 + 2 + h2) * 0.5f;
            t_top(800, 24, 96, y, v->name, hexc(0xF5F5F7), 0);
            t_top(600, 19, 96, y + h1 + 2, "Video \xC2\xB7 USB", hexc(0xC7C7CC), 0);
        }
        fs_btn(F_BACK, 472, 375, 84, hexa(0x1E1E22, 0.45f), "back10", 36, now);
        fs_btn(F_PLAY, 640, 375, 124, hexa(0xF5F5F7, 0.18f), A.vplaying ? "pause" : "play", A.vplaying ? 52 : 54, now);
        fs_btn(F_FWD, 808, 375, 84, hexa(0x1E1E22, 0.45f), "fwd10", 36, now);
        rrect(30, FS_SEEK_Y + 13, 1220, 6, 3, hexa(0xFFFFFF, 0.25f));
        rrect(30, FS_SEEK_Y + 13, 1220 * frac, 6, 3, hexc(0xF5F5F7));
        shadow(30 + 1200 * fminf(1, frac), FS_SEEK_Y + 6, 20, 20, 10, 8, 0, 2, hexa(0, 0.4f));   /* v25: thumb inside the track */
        rrect(30 + 1200 * fminf(1, frac), FS_SEEK_Y + 6, 20, 20, 10, hexc(0xF5F5F7));
        if (video_loaded()) { fmt_time(a, sizeof a, A.vpos); fmt_time(b, sizeof b, v->len); }   /* v14: duration on the right */
        else { snprintf(a, sizeof a, "\xE2\x80\x93:\xE2\x80\x93\xE2\x80\x93"); snprintf(b, sizeof b, "%s", a); }   /* video closed: -:-- */
        t_left(700, 21, 30, FS_ROW_CY, a, hexc(0xF5F5F7));
        t_right(700, 21, 1180, FS_ROW_CY, b, hexc(0xC7C7CC));
        {
            float k = press_k(F_EXIT2, now);
            ico("fs_exit", 1224, FS_ROW_CY, 28 * k, hexa(0xF5F5F7, 1 - press_dim(k)));
        }
        {   /* sliders: glass rgba(255,255,255,.18) + border, fill from the bottom #F5F5F7, icon (difference) */
            int i;
            for (i = 0; i < 2; i++) {
                float x = i ? 1280 - 34 - FS_SL_W : 34, y = fs_sl_y(), f = tw_get(i ? &fs_vh : &fs_bh, now);
                char n[8];
                snprintf(n, sizeof n, "%d", i ? (A.muted ? 0 : A.vol) : A.bright);
                text_tnum(1);
                t_center(800, 20, x + FS_SL_W * 0.5f, y - 10 - t_lh(800, 20) * 0.5f, n, hexc(0xF5F5F7));
                text_tnum(0);
                rrect_b(x, y, FS_SL_W, FS_SL_H, 32, hexa(0xFFFFFF, 0.18f), hexa(0xFFFFFF, 0.14f));
                if (f > 0.001f) {
                    clip_push(x, y + FS_SL_H * (1 - f), FS_SL_W, FS_SL_H * f);   /* overflow: hidden on the rounded one */
                    rrect(x, y, FS_SL_W, FS_SL_H, 32, hexc(0xF5F5F7));
                    clip_pop();
                }
                gfx_blend_difference(1);
                ico(i ? "speaker_w22" : "sun", x + 19 + 13, y + FS_SL_H - 18 - 13, 26, C_WHITE);
                gfx_blend_difference(0);
            }
        }
        gfx_xform_pop();
    }
    gfx_xform_pop();
}

void video_fs_click(int id, double now)
{
    int c = ctl_on(now);
    if (id == F_TAP) A.vctl_t = c ? -999 : now;
    else if (id == F_EXIT || id == F_EXIT2) A.vfull = 0;
    else if (id == F_PLAY) { A.vplaying = !A.vplaying; A.vctl_t = now; state(now); }
    else if (id == F_BACK) { A.vpos = fmax(0, A.vpos - 10); A.vctl_t = now; }
    else if (id == F_FWD) { A.vpos = fmin(vid_at(A.vsel)->len, A.vpos + 10); A.vctl_t = now; }
    video_fs_layout(now);
    damage_full();
}

void video_fs_drag(int id, float x, float y, int phase, double now)
{
    (void)phase;
    if (id == F_BRIGHT || id == F_VOL) {                         /* vertical: 0 at the bottom, 1 at the top */
        float u = 1 - (y - fs_sl_y()) / FS_SL_H;
        u = u < 0 ? 0 : u > 1 ? 1 : u;
        if (id == F_BRIGHT) set_bright(1 + (int)lroundf(u * 9));
        else { A.vol = (int)lroundf(u * 31); A.muted = 0; }
        A.vctl_t = now;
        damage_full();
        return;
    }
    if (id != F_SEEK) return;
    A.vpos = floor(slider_value(30, 1220, x) * vid_at(A.vsel)->len);
    A.vdrag = phase < 2;                                           /* seek - on release */
    A.vctl_t = now;
    damage_d(0, 560, 1280, 190);
}
