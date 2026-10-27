/* Bluetooth (S_BT) and Sound (S_SOUND) - shared module. Design: Main.dc.html, tabBt /
 * tabSound + extra(). Bluetooth - real stack (bt.h), sound - MCU (core, mcu_sync). */
#include "version.h"
#include "ui_int.h"
#include "i18n.h"
#include "bt.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { H_CLOSE = HIT_SCREEN, H_BT, H_DISC, H_SEARCH, H_ACT0, H_REM0 = H_ACT0 + 8, H_AV0 = H_REM0 + 8,
       H_RESET = H_AV0 + 8, H_KEY, H_MUTE, H_SPD0, H_VOL0 = H_SPD0 + 4, H_EQ0 = H_VOL0 + 3 };

#define PX PX0                       /* v26: padding 24 */
#define PY PY0
#define CW PCW
#define GAP2 (cur_screen() == S_SOUND ? 24.0f : 18.0f)   /* two columns: Bluetooth - gap 18, Sound - 24 */
#define COLW ((CW - GAP2) / 2)
#define C2 (PX + COLW + GAP2)
#define BT_BOX (PY + 52 + 18)        /* below the header 52, gap 18 */
#define BT_LH (BT_BOX + 84 + 18)      /* list header row (48); v36 CarPlay / AA Auto / Off - absent (owner) */
#define BT_ROWS (BT_LH + 56)          /* device rows */
#define EQ_MAX 7                  /* hardware (MCU 0x21): -7..+7; the design has -6..+6 - see DEVIATIONS.md */

static tween bt_tw, disc_tw, list_op, conn_bg[8], key_tw, mute_tw, spd_th, vol_v[3], eq_v[3], spin_op;
static int last_disc_s = -1, last_found = -1, last_srch = -1;

static const char *BT_ICON[4] = { "bt_phone", "bt_tablet", "bt_speaker", "laptop" };

/* available devices: found by scan, excluding paired */
static int avail(const btdev **out)
{
    int i, j, n = 0;
    if (!A.bt_on) return 0;
    for (i = 0; i < A.n_extra; i++) {
        int skip = 0;
        for (j = 0; j < A.n_paired; j++) if (!strcmp(A.paired[j].id, A.extra[i].id)) skip = 1;
        if (!skip && n < 8) out[n++] = &A.extra[i];
    }
    return n;
}

static const char *act_label(int i)
{
    int conn = A.bt_on && !strcmp(A.bt_conn, A.paired[i].id), pend = A.bt_on && !strcmp(A.bt_pending, A.paired[i].id);
    return conn ? "Disconnect" : pend ? "\xE2\x80\xA6" : "Connect";
}
static float act_w(int i) { return t_width(700, 18, act_label(i)) + 32; }
static const char *search_label(void) { return A.searching ? "Searching" : "Search"; }
static float search_w(void) { return 16 + 18 + 8 + t_width(700, 18, search_label()) + 16; }

/* ---- Sound: geometry */
static float snd_box1(void) { return PY + 52 + 8 + 18 + t_lh(700, 19) + 10; }
static float snd_row_h(void) { return t_lh(700, 20) + 46; }
/* v5: General - padding 4, rows 56; Volume - padding 4 20 14, slider 4 below the label */
static float vol_row_h(void) { return t_lh(700, 20) + 44; }
static float snd_box2(void) { return snd_box1() + 178 + 10 + t_lh(700, 19) + 10; }   /* single column: gap 10 */
static float snd_vol_y(int i) { return snd_box2() + 4 + i * (6 + vol_row_h()) + 6; }
static float snd_bottom(void) { return snd_box2() + 4 + 3 * (6 + vol_row_h()) + 14; }
static float snd_eq_y(int i)
{
    float H = snd_bottom() - snd_box1(), gap = (H - 28 - 3 * snd_row_h()) / 4;
    return snd_box1() + 14 + gap * (i + 1) + snd_row_h() * i;
}
static const int VMIN[3] = { 0, 0, 5 }, VMAX[3] = { 31, 25, 25 };
static int *vol_ptr(int i) { return i == 0 ? &A.vol : i == 1 ? &A.phone_vol : &A.def_vol; }

static void layout(void)
{
    int i, s = cur_screen();
    set_hit(H_CLOSE, PX, PY, 52, 52);
    if (s == S_BT) {
        const btdev *av[8];
        int n = avail(av);
        set_hit(H_BT, PX + CW - 76, PY + 4, 76, 44);
        set_hit(H_DISC, C2 + COLW - 20 - 76, BT_BOX + 20, 76, 44);
        set_hit(H_SEARCH, C2 + COLW - search_w(), BT_LH + 2, search_w(), 44);
        for (i = 0; i < A.n_paired && i < 8; i++) {
            float y = BT_ROWS + i * 72;
            set_hit(H_REM0 + i, PX + COLW - 10 - 44, y + 10, 44, 44);
            set_hit(H_ACT0 + i, PX + COLW - 10 - 44 - 12 - act_w(i), y + 10, act_w(i), 44);
        }
        for (i = 0; i < n; i++) set_hit(H_AV0 + i, C2, BT_ROWS + i * 72, COLW, 64);
    } else {
        float b1 = snd_box1();
        set_hit(H_RESET, PX + CW - reset_w(), PY + 2, reset_w(), 48);
        set_hit(H_KEY, PX + COLW - 20 - 76, b1 + 10, 76, 44);
        set_hit(H_MUTE, PX + COLW - 20 - 76, b1 + 67, 76, 44);
        for (i = 0; i < 4; i++) set_hit(H_SPD0 + i, PX + COLW - 20 - 270 + 3 + i * 66, b1 + 126, 66, 40);
        for (i = 0; i < 3; i++) {
            set_drag(H_VOL0 + i, PX + 20, snd_vol_y(i) + t_lh(700, 20) + 4 - 6, COLW - 40, 52);
            set_drag(H_EQ0 + i, C2 + 20, snd_eq_y(i) + t_lh(700, 20) + 6 - 6, COLW - 40, 52);
        }
    }
}

/* ================================================================= Bluetooth */

static void bt_static(void)
{
    panel(ST_X, ST_Y, ST_W, ST_H, PANEL_R);
    head_static(PX, PY, 0x1F9FD6, "bluetooth_w22", 22, "Bluetooth");
}

static void mmss(char *b, size_t n, int x) { snprintf(b, n, "%02d:%02d", x / 60, x % 60); }

static void bt_dyn(double now)
{
    const btdev *av[8];
    int i, n = avail(av);
    float lo = tw_get(&list_op, now);
    char buf[48];
    close_btn(H_CLOSE, PX, PY, now);
    {
        float x = PX + CW - 76, y = PY + 4, w = 76, h = 44, k = press_box(H_BT, &x, &y, &w, &h, now);
        (void)k;
        toggle_draw_c(x, y, tw_get(&bt_tw, now), C_ACCENT);
    }
    gfx_xform_push(D(692), D(336), 1, 0, lo);
    /* This device */
    rrect(PX, BT_BOX, COLW, 84, 20, C_INNER);
    rrect(PX + 20, 135, 44, 44, 13, hexc(0x1F9FD6));
    ico("laptop", PX + 42, 157, 22, C_WHITE);
    {
        float h1 = t_lh(700, 17), h2 = t_lh(800, 22), y = 157 - (h1 + 3 + h2) * 0.5f;   /* v12: name 22 */
        t_top(700, 17, PX + 78, y, "This device", C_SUB, 0);
        t_top(800, 22, PX + 78, y + h1 + 3, HU_NAME, C_TEXT, 0);
    }
    /* Discoverable */
    {
        int vis = A.bt_on && A.disc;
        float h1 = t_lh(700, 17), h2 = t_lh(800, 22), y = 157 - (h1 + 3 + h2 + (vis ? 4 + 6 : 0)) * 0.5f;   /* v19: + bar 6 */
        double el = now - A.disc_t;
        rrect(C2, BT_BOX, COLW, 84, 20, C_INNER);
        t_top(700, 17, C2 + 20, y, "Discoverable", C_SUB, 0);
        if (vis) { int left = 180 - (int)(el); char t[12]; mmss(t, sizeof t, left < 0 ? 0 : left); snprintf(buf, sizeof buf, tr("Visible \xC2\xB7 %s"), t); }
        else snprintf(buf, sizeof buf, "Hidden");
        t_top(800, 22, C2 + 20, y + h1 + 3, buf, vis ? C_ACCENT : C_TEXT, 0);
        {
            float x = C2 + COLW - 20 - 76, yy = 135, w = 76, h = 44;
            press_box(H_DISC, &x, &yy, &w, &h, now);
            toggle_draw_c(x, yy, tw_get(&disc_tw, now), C_ACCENT);
        }
        if (vis) {                                    /* v19: remaining-time bar under the text: 6px, up to 220, accent */
            float pct = (float)fmax(0, 1 - el / 180.0), by = y + h1 + 3 + h2 + 4;
            rrect(C2 + 20, by, 220, 6, 3, C_TRACK);
            if (pct > 0) { clip_push(C2 + 20, by, 220 * pct, 6); rrect(C2 + 20, by, 220, 6, 3, C_ACCENT); clip_pop(); }
        }
    }
    /* Paired devices (doesn't fit - clipped by the panel bottom) */
    clip_push(ST_X, BT_LH, ST_W, ST_Y + ST_H - BT_LH);
    t_left(700, 19, PX, BT_LH + 24, "Paired devices", C_SUB);
    for (i = 0; i < A.n_paired && i < 8; i++) {
        const btdev *d = &A.paired[i];
        int conn = A.bt_on && !strcmp(A.bt_conn, d->id), pend = A.bt_on && !strcmp(A.bt_pending, d->id);
        float y = BT_ROWS + i * 72, bg = tw_get(&conn_bg[i], now), aw = act_w(i), ax = PX + COLW - 10 - 44 - 12 - aw;
        float h1 = t_lh(700, 21), h2 = t_lh(600, 17), ty = y + 32 - (h1 + 2 + h2) * 0.5f;
        if (bg > 0.005f) rrect(PX, y, COLW, 64, 18, col_alpha(C_INNER, bg));
        rrect(PX + 12, y + 10, 44, 44, 13, hexc(0x1F9FD6));
        ico(BT_ICON[d->kind & 3], PX + 34, y + 32, 22, C_WHITE);
        t_fit(700, 21, PX + 68, ty + h1 * 0.5f, ax - 12 - PX - 68, d->name, C_TEXT);
        t_left(600, 17, PX + 68, ty + h1 + 2 + h2 * 0.5f, pend ? "Connecting\xE2\x80\xA6" : conn ? "Connected" : "Not connected",
               conn ? C_OK : pend ? C_ACCENT : C_SUB);
        {
            float x = ax, yy = y + 10, w = aw, h = 44, k = press_box(H_ACT0 + i, &x, &yy, &w, &h, now);
            rrect(x, yy, w, h, 22 * k, C_INNER);
            t_center(700, 18, x + w * 0.5f, yy + h * 0.5f, act_label(i), col_alpha(C_TEXT, 1 - press_dim(k)));
        }
        {
            float x = PX + COLW - 54, yy = y + 10, w = 44, h = 44, k = press_box(H_REM0 + i, &x, &yy, &w, &h, now);
            rrect(x, yy, w, h, 22 * k, hexa(0xFF453A, 0.14f));
            ico("trash", x + w * 0.5f, yy + h * 0.5f, 18 * k, hexc(0xFF453A));
        }
    }
    if (!A.n_paired) t_top(600, 19, PX + 12, BT_ROWS + 12, "No paired devices", C_SUB, 0);
    /* Available devices */
    {
        float lw = t_width(700, 19, "Available devices"), so = tw_get(&spin_op, now);
        t_left(700, 19, C2, BT_LH + 24, "Available devices", C_SUB);
        if (so > 0.005f) ico_rot("spinner", C2 + lw + 10 + 9, BT_LH + 24, 18, col_alpha(C_ACCENT, so), (float)fmod(now * 360.0, 360.0));
    }
    {
        float x = C2 + COLW - search_w(), y = BT_LH + 2, w = search_w(), h = 44, k = press_box(H_SEARCH, &x, &y, &w, &h, now);
        col c = col_alpha(C_TEXT, 1 - press_dim(k));
        rrect(x, y, w, h, 22 * k, C_INNER);
        ico("search", x + (16 + 9) * k, y + h * 0.5f, 18 * k, c);
        t_left(700, 18, x + (16 + 18 + 8) * k, y + h * 0.5f, search_label(), c);
    }
    for (i = 0; i < n; i++) {
        int pairing = !strcmp(A.pairing, av[i]->id);
        float x = C2, y = BT_ROWS + i * 72, w = COLW, h = 64, k = press_box(H_AV0 + i, &x, &y, &w, &h, now);
        float h1 = t_lh(700, 21), h2 = t_lh(600, 17), ty = y + h * 0.5f - (h1 + 2 + h2) * 0.5f;
        rrect(x + 12 * k, y + 10 * k, 44 * k, 44 * k, 13 * k, C_INNER);
        ico("bluetooth_w20", x + 34 * k, y + h * 0.5f, 22 * k, C_TEXT);          /* v12: found ones - generic BT icon */
        t_fit(700, 21, x + 68 * k, ty + h1 * 0.5f, w - 80, av[i]->name, col_alpha(C_TEXT, 1 - press_dim(k)));
        t_left(600, 17, x + 68 * k, ty + h1 + 2 + h2 * 0.5f, pairing ? "Pairing\xE2\x80\xA6" : "Tap to pair", pairing ? C_ACCENT : C_SUB);
    }
    if (!n) t_top(600, 19, C2 + 12, BT_ROWS + 12, !A.bt_on ? "Bluetooth is off" : A.searching ? "Searching\xE2\x80\xA6" : "No devices found", C_SUB, 0);
    clip_pop();
    gfx_xform_pop();
}

/* ================================================================= Sound */

static void sound_static(void)
{
    float b1 = snd_box1(), b2 = snd_box2(), bot = snd_bottom(), lh = t_lh(700, 19);
    panel(ST_X, ST_Y, ST_W, ST_H, PANEL_R);
    head_static(PX, PY, 0xE0458F, "sliders_w22", 22, "Sound");
    t_left(700, 19, PX, b1 - 10 - lh * 0.5f, "General", C_SUB);
    t_left(700, 19, C2, b1 - 10 - lh * 0.5f, "Equalizer", C_SUB);
    t_left(700, 19, PX, b2 - 10 - lh * 0.5f, "Volume", C_SUB);
    rrect(PX, b1, COLW, 178, 20, C_INNER);
    rrect(PX + 20, b1 + 60, COLW - 40, 1, 0, C_TRACK);
    rrect(PX + 20, b1 + 117, COLW - 40, 1, 0, C_TRACK);
    row_text(PX + 20, b1 + 32, COLW - 40 - 86, "Key tone", "Click when you tap the screen");
    row_text(PX + 20, b1 + 89, COLW - 40 - 86, "Mute", NULL);
    row_text(PX + 20, b1 + 146, 200, "Speed volume", NULL);
    rrect(PX, b2, COLW, bot - b2, 20, C_INNER);
    rrect(C2, b1, COLW, bot - b1, 20, C_INNER);
}

static void sign_str(char *b, size_t n, int v)
{
    if (v > 0) snprintf(b, n, "+%d", v); else if (v < 0) snprintf(b, n, "\xE2\x88\x92%d", -v); else snprintf(b, n, "0");
}

static void sound_dyn(double now)
{
    static const char *SPD[4] = { "Off", "Low", "Middle", "High" };
    static const char *VL[3] = { "Media volume", "Phone volume", "Default volume" };
    const char *EL[3] = { "Bass", i18n_lang() == LANG_EN ? "Middle" : tr("eq:Middle"), "Treble" };   /* "Middle" of speed-dependent volume - a different translation */
    float b1 = snd_box1(), x;
    char v[16];
    int i;
    close_btn(H_CLOSE, PX, PY, now);
    reset_btn(H_RESET, PX + CW - reset_w(), PY + 2, now);
    toggle_draw_c(PX + COLW - 96, b1 + 10, tw_get(&key_tw, now), C_ACCENT);
    toggle_draw_c(PX + COLW - 96, b1 + 67, tw_get(&mute_tw, now), hexc(0xFF453A));
    seg_plain(PX + COLW - 20 - 270, b1 + 123, 4, 66, 40, 3, tw_get(&spd_th, now), SPD, 17, H_SPD0, now);
    for (i = 0; i < 3; i++) {
        float f = (tw_get(&vol_v[i], now) - VMIN[i]) / (VMAX[i] - VMIN[i]);
        int m = i == 0 && A.muted;
        snprintf(v, sizeof v, "%d", *vol_ptr(i));
        slider_row_m(PX + 20, snd_vol_y(i), COLW - 40, 4, VL[i], v, f, i == 1 ? "phone_w22" : m ? "vol_muted" : "vol_media",
                   m ? C_MUTED : C_TEXT);
    }
    for (i = 0; i < 3; i++) {
        float e = tw_get(&eq_v[i], now), sy, w = COLW - 40, l, fw;
        x = C2 + 20;
        sign_str(v, sizeof v, A.eqv[i]);
        sy = slider_row(x, snd_eq_y(i), w, EL[i], v, 0, NULL, C_TEXT);
        l = e < 0 ? 0.5f + e / (2 * EQ_MAX) : 0.5f;        /* fill from the center */
        fw = fabsf(e) / (2 * EQ_MAX);
        if (fw > 0.001f) { clip_push(x + w * l, sy, w * fw, 40); rrect(x, sy, w, 40, 20, C_TEXT); clip_pop(); }
        gfx_blend_difference(1);
        rrect(x + w * 0.5f - 1, sy + 10, 2, 20, 1, hexa(0xFFFFFF, 0.55f));
        gfx_blend_difference(0);
    }
}

static void sound_state(double now)
{
    int i;
    tw_set(&key_tw, (float)A.key_tone, 400, &SPRING, now);
    tw_set(&mute_tw, (float)A.muted, 400, &SPRING, now);
    tw_set(&spd_th, (float)A.speed_vol, SPRING_MS, &SPRING_SOFT, now);
    for (i = 0; i < 3; i++) {
        tw_set(&vol_v[i], (float)*vol_ptr(i), 180, &EASE_OUT, now);
        tw_set(&eq_v[i], (float)A.eqv[i], 180, &EASE_OUT, now);
    }
}

static void bt_state(double now)
{
    int i;
    tw_set(&bt_tw, (float)A.bt_on, 400, &SPRING, now);
    tw_set(&disc_tw, (float)(A.bt_on && A.disc), 400, &SPRING, now);
    tw_set(&list_op, A.bt_on ? 1 : 0.4f, 350, &EASE, now);
    tw_set(&spin_op, (float)A.searching, 300, &EASE, now);
    for (i = 0; i < 8; i++) tw_set(&conn_bg[i], i < A.n_paired && A.bt_on && !strcmp(A.bt_conn, A.paired[i].id) ? 1 : 0, 300, &EASE, now);
}

/* ================================================================= common */

static void draw_static(void)
{
    int s = cur_screen();
    if (s == S_BT) bt_static(); else sound_static();
}

static void draw_dyn(double now)
{
    int s = cur_screen();
    if (s == S_BT) bt_dyn(now); else sound_dyn(now);
}

static void update(double now)
{
    int s = cur_screen();
    if (s == S_BT) {
        int ds = A.disc ? (int)(now - A.disc_t) : -1;
        bt_state(now);
        if (ds != last_disc_s) { last_disc_s = ds; damage_d(C2, BT_BOX, COLW, 84); }
        if (A.disc) damage_d(C2, BT_BOX + 80, COLW, 4);
        clear_screen_hits(); layout();                    /* labels/lists also change on the timer (tick) */
        if (A.found_n != last_found || A.searching != last_srch) {
            last_found = A.found_n; last_srch = A.searching;
            damage_d(ST_X, BT_LH - 15, ST_W, 654 - BT_LH + 15);
        }
        if (A.searching || tw_active(&spin_op, now)) damage_d(C2 + 120, BT_LH + 8, 60, 32);
        if (tw_active(&bt_tw, now) || tw_active(&disc_tw, now) || tw_active(&list_op, now) || any_active(conn_bg, 8, now)
            || press_active(now))
            damage_d(ST_X, ST_Y, ST_W, ST_H);
    } else {
        if (tw_active(&key_tw, now) || tw_active(&mute_tw, now) || tw_active(&spd_th, now) || any_active(vol_v, 3, now)
            || any_active(eq_v, 3, now) || press_active(now))
            damage_d(ST_X, ST_Y, ST_W, ST_H);
        sound_state(now);                                 /* volume also changes via the buttons at the bottom */
    }
}

static void bt_changed(double now)
{
    bt_state(now);
    clear_screen_hits(); layout();
    damage_d(ST_X, ST_Y, ST_W, ST_H);
}

static void click(int id, double now)
{
    int s = cur_screen();
    if (id == H_CLOSE) { close_screen(now); return; }
    if (s == S_BT) {
        if (id == H_BT) {                     /* state will come from the stack (bt_poll) */
            if (A.bt_on) { bt_power(0); A.bt_on = 0; A.bt_pending[0] = 0; A.pairing[0] = 0; A.searching = 0; A.disc = 0; }
            else bt_power(1);
            base_dirty();
        } else if (id == H_DISC) {
            if (!A.bt_on) return;
            if (A.disc) A.disc = 0; else { A.disc = 1; A.disc_t = now; }
            bt_discoverable(A.disc);
        } else if (id == H_SEARCH) {
            if (!A.bt_on || A.searching) return;
            A.searching = 1; A.n_extra = 0;
            bt_search(1);
        } else if (id >= H_ACT0 && id < H_ACT0 + 8) {
            int i = id - H_ACT0;
            if (!A.bt_on || i >= A.n_paired) return;
            if (!strcmp(A.bt_conn, A.paired[i].id)) bt_disconnect(A.paired[i].id);
            else { snprintf(A.bt_pending, sizeof A.bt_pending, "%s", A.paired[i].id); A.pend_t = now; bt_connect(A.paired[i].id); }
        } else if (id >= H_REM0 && id < H_REM0 + 8) {
            int i = id - H_REM0;
            if (i >= A.n_paired) return;
            if (!strcmp(A.bt_conn, A.paired[i].id)) bt_disconnect(A.paired[i].id);
            bt_remove(A.paired[i].id);
        } else if (id >= H_AV0 && id < H_AV0 + 8) {
            const btdev *av[8];
            int n = avail(av), i = id - H_AV0;
            if (i < n && !A.pairing[0]) { snprintf(A.pairing, sizeof A.pairing, "%s", av[i]->id); A.pair_t = now; bt_pair(av[i]->id); }
        }
        bt_changed(now);
        return;
    }
    if (id == H_RESET) {
        A.muted = 0; A.vol = 12; A.phone_vol = 15; A.def_vol = 12; A.speed_vol = 0; A.key_tone = 1;
        A.eqv[0] = A.eqv[1] = A.eqv[2] = 0;
        hud_show(3, NULL, now);
    } else if (id == H_KEY) A.key_tone = !A.key_tone;
    else if (id == H_MUTE) toggle_muted(now);
    else if (id >= H_SPD0 && id < H_SPD0 + 4) A.speed_vol = id - H_SPD0;
    sound_state(now);
}

static void drag(int id, float x, float y, int phase, double now)
{
    (void)y;
    if (phase == 2) return;
    if (id >= H_VOL0 && id < H_VOL0 + 3) {
        int i = id - H_VOL0, v = VMIN[i] + (int)lroundf(slider_value(PX + 20, COLW - 40, x) * (VMAX[i] - VMIN[i]));
        if (i == 0) { if (v != A.vol || A.muted) set_volume(v, now); }
        else *vol_ptr(i) = v;
    } else if (id >= H_EQ0 && id < H_EQ0 + 3)
        A.eqv[id - H_EQ0] = -EQ_MAX + (int)lroundf(slider_value(C2 + 20, COLW - 40, x) * 2 * EQ_MAX);
    sound_state(now);
}

static void enter(double now)
{
    int i;
    last_disc_s = last_found = last_srch = -1;
    sound_state(now); bt_state(now);
    tw_init(&key_tw, key_tw.to); tw_init(&mute_tw, mute_tw.to); tw_init(&spd_th, spd_th.to);
    for (i = 0; i < 3; i++) { tw_init(&vol_v[i], vol_v[i].to); tw_init(&eq_v[i], eq_v[i].to); }
    tw_init(&bt_tw, bt_tw.to); tw_init(&disc_tw, disc_tw.to); tw_init(&list_op, list_op.to); tw_init(&spin_op, spin_op.to);
    for (i = 0; i < 8; i++) tw_init(&conn_bg[i], conn_bg[i].to);
}

const screen_def SCR_SETTINGS = { layout, draw_static, draw_dyn, update, click, enter, drag };
