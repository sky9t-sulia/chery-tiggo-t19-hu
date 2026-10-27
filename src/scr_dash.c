/* Dashboard screen: music (big card); on the right - a switchable widget (Connections / Photos · USB) and radio.
 * Design: Main.dc.html, is.dashboard + widgets (v26: photo - full card, no video widget). */
#include "ui_int.h"
#include "i18n.h"
#include "media.h"
#include "bt.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { H_PREV = HIT_SCREEN, H_PLAY, H_NEXT, H_MUSIC_HEAD, H_ART, H_W0, H_W1, H_W2, H_PHOTO, H_RADIO,
       H_RPREV, H_RFAV, H_RNEXT, H_RON, H_W3, H_IUSB, H_IBT, H_SHUF, H_REP, H_SEEK,
       H_SWAP };                    /* last: on overlap the higher number wins - the swap button is over the photo */

static int last_sec = -1;
#define NW 2                        /* widgets: Connections, Photos · USB */
static tween w_op, w_s, w_dy, w_spin, ph_cur;              /* photo: the new one fades in over the old (1.2 s) */
static int ph_prev = -1;
static int w_phase;                 /* 0 in, 1 out, 2 pre */
static double w_t, ph_t0;
static tween shuf_bg, rep_bg;
static int ph_idx = -1;

#define WX 846
#define WY 92                       /* v31: header 38 (swap button 50 with margin -6), padding 4, gap 6; +1 per v26 measurement */
#define WW 400
#define WH 220
#define WHEAD_CY 62                 /* middle of the widget header row: 18 + border 1 + padding 24 + 19 */
/* v26 (measured from the design render): right cards have different heights - radio doesn't fit in half, flex gives it
 * more: widget 18..322, radio 340..654 */
/* v31 music: header 38 + margin 20, block with margin-top 18 (119..629) - centered: row (cover 230 | text),
 * margin 30, bar 6, margin 12, time 20/600, margin 18, buttons 104 (shuffle 64, prev 80, play 104, next 80, repeat 64) */
#define MX 147.0f                   /* card content: 122 + border 1 + padding 24, width 640 */
#define MW 640.0f
#define ART 230.0f
#define TX (MX + ART + 28)
static float art_y(void) { return 119 + (510 - (ART + 30 + 6 + 12 + t_lh(600, 20) + 18 + 104)) * 0.5f; }
static float bar_y(void) { return art_y() + ART + 30; }
static float btn_cy(void) { return bar_y() + 6 + 12 + t_lh(600, 20) + 18 + 52; }
static float btn_cx(int i)
{
    static const float S[5] = { 64, 80, 104, 80, 64 };
    float x = MX;
    int k;
    for (k = 0; k < i; k++) x += S[k] + (MW - 392) / 4;
    return x + S[i] * 0.5f;
}
#define CARD1_H 304.0f
#define CARD2_Y 340.0f
#define CARD2_H 314.0f

static void layout(void)
{
    int wi = A.widget % NW;
    if (music_dash_play()) {
        float cy = btn_cy();
        set_hit(H_MUSIC_HEAD, MX, 43, 260, 38);
        set_hit(H_ART, MX, art_y(), ART, ART);
        if (np_now(0).av) {                                 /* pointer-events: none without a source */
            set_hit(H_SHUF, btn_cx(0) - 32, cy - 32, 64, 64);
            set_hit(H_PREV, btn_cx(1) - 40, cy - 40, 80, 80);
            set_hit(H_PLAY, btn_cx(2) - 52, cy - 52, 104, 104);
            set_hit(H_NEXT, btn_cx(3) - 40, cy - 40, 80, 80);
            set_hit(H_REP, btn_cx(4) - 32, cy - 32, 64, 64);
            if (A.src_usb) set_drag(H_SEEK, MX - 10, bar_y() - 13, MW + 20, 32);   /* canSeek: !bt */
        }
    } else {   /* v15 dashIdle: "USB music" / "Phone audio" */
        if (music_usb_av()) set_hit(H_IUSB, 146, 480, 314, 150);
        if (music_bt_av()) set_hit(H_IBT, 474, 480, 314, 150);
    }
    if (wi == 0) {   /* Connections rows: 48, gap 2 (v11) */
        set_hit(H_W0, WX, WY, WW, 48); set_hit(H_W1, WX, WY + 50, WW, 48); set_hit(H_W2, WX, WY + 100, WW, 48); set_hit(H_W3, WX, WY + 150, WW, 48);
    }
    else set_hit(H_PHOTO, 830, 18, 432, CARD1_H);           /* v26: photo - whole card */
    set_hit(H_SWAP, 1150, 18, 112, WY - 18);                /* over the photo; owner: zone wider than the 50 button - card corner down to the rows */
    if (A.radio_on) {
        set_hit(H_RADIO, 854, CARD2_Y + 25, 384, 630 - CARD2_Y - 25);   /* v35: no ‹ ♥ › buttons - the whole card opens Radio */
    } else {                                               /* v15: off - the whole card opens Radio, button 104 in the center */
        set_hit(H_RADIO, 854, CARD2_Y + 25, 384, 630 - CARD2_Y - 25);
        set_hit(H_RON, 994, 457, 104, 104);
    }
}

/* v31: tile 38 (radius 11, icon 20), gap 12, 21/700 */
static void head(float x, float y, col c, const char *icon, const char *title)
{
    rrect(x, y, 38, 38, 11, c);
    ico(icon, x + 19, y + 19, 20, C_WHITE);
    t_left(700, 21, x + 50, y + 19, title, C_SUB);
}

/* title/artist/album: long ones as a marquee (static - into the cache layer, scrolling - every frame) */
static double mq_t0;
static int mq_any, mq_trk = -1;
/* v31: right of the cover (centered in the row): title 36/800 (line-height 1.08, -0.02em), artist 24/600 (+10),
 * album 20/500 (+6, if present), for Bluetooth - phone pill (+22, 48) */
static int show_from(void) { return !A.src_usb && music_bt_av(); }
static void track_text(double now, int dyn)
{
    np_t n = np_now(now);
    int alb = n.av && !n.idle && n.al[0];
    float mw = MX + MW - TX, h1 = 36 * 1.08f, h2 = t_lh(600, 24), h3 = alb ? 6 + t_lh(500, 20) : 0, h4 = show_from() ? 22 + 48 : 0;
    float ty = art_y() + ART * 0.5f - (h1 + 10 + h2 + h3 + h4) * 0.5f, y;
    if (!dyn) mq_any = 0;
    mq_any |= marquee(800, 36, TX, ty + (h1 - t_lh(800, 36)) * 0.5f, mw, n.t, C_TEXT, -0.02f * 36, mq_t0, now, dyn);
    mq_any |= marquee(600, 24, TX, ty + h1 + 10, mw, n.a, C_SUB, 0, mq_t0, now, dyn);
    y = ty + h1 + 10 + h2;
    if (alb) { mq_any |= marquee(500, 20, TX, y + 6, mw, n.al, C_MUTED, 0, mq_t0, now, dyn); y += h3; }
    if (show_from() && !dyn) {                                     /* pill: circle 34 with phone, name, "Bluetooth" */
        const char *nm = bt_phone_name();
        float nw = t_width(700, 19, nm), w = 8 + 34 + 12 + nw + 12 + t_width(600, 18, "Bluetooth") + 18;
        y += 22;
        rrect(TX, y, w, 48, 24, C_INNER);
        rrect(TX + 8, y + 7, 34, 34, 17, C_ACCENT);
        ico("device_w22", TX + 25, y + 24, 18, C_ONACCENT);
        t_left(700, 19, TX + 54, y + 24, nm, C_TEXT);
        t_left(600, 18, TX + 54 + nw + 12, y + 24, "Bluetooth", C_SUB);
    }
}

/* ---- v15 dashIdle: "Nothing playing" - circle 88, title, caption; two 150 buttons at the bottom (USB music / Phone audio) */
static float idle_y(void)
{
    float h = 88 + 10 + 6 + t_lh(800, 32) + 10 + t_lh(600, 20);
    return 81 + (480 - 81 - h) * 0.5f;                  /* v31: header 38 (43..81) */
}
static void music_idle_static(void)
{
    float y = idle_y(), cx = 467, l1 = t_lh(800, 32), l2 = t_lh(600, 20);
    int any = music_bt_av() || music_usb_av();
    head(MX, 43, C_MUSIC, "music_w24", "Music");
    rrect(cx - 44, y, 88, 88, 44, hexa(0xFF4F6A, 0.16f));
    ico("app_music", cx, y + 44, 42, hexc(0xFF4F6A));
    y += 88 + 10 + 6;
    t_center(800, 32, cx, y + l1 * 0.5f, "Nothing playing", C_TEXT);
    t_center(600, 20, cx, y + l1 + 10 + l2 * 0.5f, any ? "Choose what to listen to" : A.usb_conn ? "No music on the USB drive \xC2\xB7 connect a phone" : "Connect a phone or insert a USB drive", C_SUB);
}
static void idle_btn(int id, float x0, int on, uint32_t tile, const char *icon, const char *title, const char *sub, double now)
{
    float x = x0, y = 480, w = 314, h = 150, k = on ? press_box(id, &x, &y, &w, &h, now) : 1;
    float l1 = t_lh(800, 22), l2 = t_lh(600, 17), ty = y + h - 18 * k - (l1 + 2 + l2);
    gfx_xform_push(0, 0, 1, 0, on ? 1 : 0.4f);
    rrect_b(x, y, w, h, 22 * k, C_INNER, C_BORDER);
    (void)tile;                                                   /* v19: tile - accent */
    rrect(x + 18 * k, y + 18 * k, 52 * k, 52 * k, 15 * k, C_ACCENT);
    ico(icon, x + (18 + 26) * k, y + (18 + 26) * k, 26 * k, C_ONACCENT);
    t_top(800, 22, x + 18 * k, ty, title, C_TEXT, 0);
    t_fit(600, 17, x + 18 * k, ty + l1 + 2 + l2 * 0.5f, w - 36, sub, C_SUB);
    gfx_xform_pop();
}
static void music_idle_dyn(double now)
{
    char ub[32];
    snprintf(ub, sizeof ub, music_usb_av() ? tr_n("%d songs", (long)(NTRACKS)) : A.usb_conn ? "No music files" : "Not connected", NTRACKS);   /* v15 idleUsbSub */
    idle_btn(H_IUSB, 146, music_usb_av(), 0x2F7BF6, "usb_full", "USB music", ub, now);
    idle_btn(H_IBT, 474, music_bt_av(), 0x1F9FD6, "bluetooth_w21", "Phone audio",
             !music_bt_av() ? "Not connected" : A.bt_title[0] ? bt_phone_name() : "Tap to play", now);
}

/* music card background - blurred cover (comp.c art_bg; owner 2026-10-26, DEVIATIONS) */
static art_bg bg = { { 0, 0, 0 }, -1 };
static const gfx_tex *dash_art(void)
{
    np_t n = np_now(0);
    return !music_dash_play() || !n.av || n.idle ? NULL : A.src_usb ? media_art(A.trk) : np_aa_art();
}

static void draw_static(void)
{
    char lab[40], fr[16];
    float fw;
    panel(122, 18, 690, 636, PANEL_R);
    if (music_dash_play()) art_bg_draw(&bg, 122, 18, 690, 636, PANEL_R);
    if (music_dash_play()) {
        snprintf(lab, sizeof lab, tr("Music · %s"), A.src_usb ? "USB" : "Bluetooth");
        head(MX, 43, C_MUSIC, "music_w24", lab);
        track_text(0, 0);
    } else music_idle_static();
    panel(830, 18, 432, CARD1_H, PANEL_R);                 /* widget: card background */
    panel(830, CARD2_Y, 432, CARD2_H, PANEL_R);
    head(854, CARD2_Y + 25, C_RADIO, "radio_w24", "Radio");
    (void)fr; (void)fw;
}

/* ---- radio card (design v9): FM/AM/OFF badge, frequency + caption, mini scale or "Turn on" (v35: no ‹ ♥ ›) */
static tween r_needle;
static float r_last = -1;
static int r_sig = -1;

static void radio_card(double now)
{
    char fr[16], sn[48];
    const char *badge = !A.radio_on ? "OFF" : A.am ? "AM" : "FM";
    float op = A.radio_on ? 1 : 0.4f, k;
    col orange = C_ACCENT;                                 /* v17: accent */
    int i;
    rrect(1200, CARD2_Y + 25, 38, 38, 13, A.radio_on ? C_ACC_SOFT : C_SEG);   /* v35: badge - squircle 38/13 (was a circle), 14/800, ls .02em; not a button */
    {
        font *f = F(800, 14);
        float w = text_width(f, badge, D(0.28f));
        text_draw(f, D(1219) - w * 0.5f, D(CARD2_Y + 44) - text_line_height(f) * 0.5f, badge, A.radio_on ? orange : C_SUB, D(0.28f));
    }
    if (A.radio_on) {   /* v15: off - no frequency or scale */
    gfx_xform_push(0, 0, 1, 0, op);
    radio_fmt(fr, sizeof fr, A.am ? A.am_f : A.fm_f);
    {   /* frequency 56 + unit 20 (on the baseline), caption below it */
        font *f = F(800, 56), *fb = F(700, 20);
        float y = 402.5f + (205 - 86) * 0.5f,   /* v35: no button row - block centered between the header and the scale at the bottom */ fy = D(y) + (D(56) - text_line_height(f)) * 0.5f, base = fy + text_ascent(f);
        float fw = text_width(f, fr, D(-1.68f));
        text_draw(f, D(854), fy, fr, C_TEXT, D(-1.68f));
        text_draw(fb, D(854) + fw + D(8), base - text_ascent(fb), tr(A.am ? "kHz" : "MHz"), C_SUB, 0);
        t_fit(600, 20, 854, y + 56 + 6 + 12, 384, station_name(sn, sizeof sn), C_SUB);
    }
    for (i = 0; i <= 40; i++) {   /* mini scale, 41 ticks */
        float h = i % 10 == 0 ? 20 : i % 5 == 0 ? 13 : 7;
        rrect(854 + i * 382.0f / 40, 629 - h, 2, h, 1, C_TICK);   /* v35: scale at the bottom of the card (654 - border 1 - padding 24) */
    }
    {
        float nx = 854 + tw_get(&r_needle, now) * 384;
        shadow(nx - 2, 604, 4, 25, 2, 10, 0, 0, C_ACC_GLOW);
        rrect(nx - 2, 604, 4, 25, 2, C_ACCENT);
    }
    gfx_xform_pop();
    }
    if (!A.radio_on) {   /* v15: round button 104 in the card center (margin-top -40) */
        float x = 994, y = 457, w = 104, h = 104;
        k = press_box(H_RON, &x, &y, &w, &h, now);
        rrect_b(x, y, w, h, 52 * k, C_INNER, C_BORDER);
        ico("power", x + w * 0.5f, y + h * 0.5f, 44 * k, col_alpha(C_ACCENT, 1 - press_dim(k)));
    }
}

static void radio_update(double now)
{
    float f = radio_frac();
    int sig = A.radio_on * 4 + radio_is_fav() * 2 + A.am + A.nfav[A.am] * 8 + (int)lroundf(A.am ? A.am_f : A.fm_f * 20) * 1024;
    if (f != r_last) { r_last = f; tw_set(&r_needle, f, A.seeking || A.scanning ? 0 : 600, A.seeking || A.scanning ? &LINEAR : &SPRING_NEEDLE, now); }
    if (sig != r_sig) { r_sig = sig; layout_again(); damage_d(830, CARD2_Y, 432, CARD2_H); }
    if (tw_active(&r_needle, now)) damage_d(840, 530, 410, 40);
}

/* ---- widget */
static const char *W_TITLE[NW] = { "Connections", "Photos \xC2\xB7 USB" };
static const char *W_ICON[NW] = { "conn", "app_display_w24" };
static const uint32_t W_COL[NW] = { 0x2F7BF6, 0xC2410C };

/* Connections rows (v11): USB 1, USB 2 (phone in modem mode), Bluetooth (+ phone battery), Internet */
static void conn_rows(double now)
{
    static const char *NAME[4] = { "USB 1", "USB 2", "Bluetooth", "Internet" };
    static const char *ICO[4] = { "usb_full", "usb_full", "bluetooth_w21", "globe_w21" };
    char st[96];
    int i, j;
    const btdev *cd = NULL;
    if (A.bt_on) for (j = 0; j < A.n_paired; j++) if (!strcmp(A.paired[j].id, A.bt_conn)) cd = &A.paired[j];
    for (i = 0; i < 4; i++) {
        float x = WX, y = WY + i * 50, w = WW, h = 48, k = press_box(H_W0 + i, &x, &y, &w, &h, now), bw = 0;
        float op = (i == 1 || i == 3) && !A.net ? 0.45f : i == 2 && !A.bt_on ? 0.45f : i == 0 && !A.usb_conn ? 0.45f : 1;
        int act = i == 0 ? A.usb_conn : i == 2 ? A.bt_on : A.net;     /* v19: active - accent, otherwise gray */
        col sc = C_SUB;
        if (i == 0) { if (A.usb_conn) { snprintf(st, sizeof st, tr("USB drive \xC2\xB7 %.1f GB free"), A.usb_free_gb); sc = C_OK; } else snprintf(st, sizeof st, "Not connected"); }
        else if (i == 1) {
            if (A.net) { snprintf(st, sizeof st, tr("%s \xC2\xB7 Tethering"), A.net_dev[0] ? A.net_dev : tr("Phone")); sc = C_OK; }
            else snprintf(st, sizeof st, "Not connected");
        } else if (i == 2) {
            if (!A.bt_on) snprintf(st, sizeof st, "Off");
            else if (cd) { snprintf(st, sizeof st, tr("%s \xC2\xB7 Connected"), cd->name); sc = C_OK; }
            else snprintf(st, sizeof st, "On \xC2\xB7 No device connected");
        } else if (A.net) { snprintf(st, sizeof st, "Online \xC2\xB7 USB tethering"); sc = C_OK; }
        else snprintf(st, sizeof st, "Offline");
        rrect(x + 8 * k, y + 5 * k, 38 * k, 38 * k, 11 * k, col_alpha(act ? C_ACCENT : hexc(0x5E5E66), op));
        ico(ICO[i], x + 27 * k, y + h * 0.5f, 20 * k, col_alpha(act ? C_ONACCENT : C_WHITE, op));
        if (i == 2 && cd && A.ph_batt >= 0) {   /* phone battery: svg viewBox 26x24. Owner: no percentages (HFP gives
                                                   * only 0..5 - imprecise), bar only, bigger icon (DEVIATIONS) */
            float sk = 1.5f, bx = x + w - 8 * k - 26 * sk, by = y + h * 0.5f - 12 * sk;
            bw = 26 * sk + 12;
            gfx_rect_border(D(bx + 2 * sk), D(by + 6.5f * sk), D(19 * sk), D(11 * sk), D(3 * sk), hexa(0, 0), D(2 * sk), C_TEXT);
            rrect(bx + 23 * sk, by + 10.5f * sk, 2 * sk, 3 * sk, 1 * sk, C_TEXT);
            rrect(bx + 4.5f * sk, by + 9 * sk, 14 * A.ph_batt / 5.0f * sk, 6 * sk, 1.2f * sk, C_TEXT);
        }
        {
            float l1 = t_lh(700, 20), l2 = t_lh(600, 17), ty = y + h * 0.5f - (l1 + 1 + l2) * 0.5f;
            t_top(700, 20, x + (8 + 38 + 12) * k, ty, NAME[i], C_TEXT, 0);
            t_fit(600, 17, x + (8 + 38 + 12) * k, ty + l1 + 1 + l2 * 0.5f, w - 70 - bw, st, sc);
        }
    }
}

#define PH_S 10.0                   /* photo change: design tick/24 = 6 s, owner - 10 s (DEVIATIONS.md) */
static int photo_idx(double now) { return NIMAGES ? (int)((now - ph_t0) / PH_S) % NIMAGES : 0; }   /* photo from the stick */

/* v26: photo over the whole card (cover, 1.2 s change, slow zoom), 110 dimming on top for the header; opacity - like that of
 * the widget content on change (wOp) */
static void photos(double now, float o)
{
    int i, pi = photo_idx(now);
    float x = 831, y = 19, w = 430, h = CARD1_H - 2;             /* inset: 0 inside border 1 */
    if (!NIMAGES) {   /* no photos - the design has no such state, owner: "in the same style" - placeholder like in the Video list (v14) */
        float l1 = t_lh(700, 22), l2 = t_lh(600, 18), hh = 64 + 10 + l1 + 10 + l2, cy = WY + (CARD1_H + 18 - WY - hh) * 0.5f - 8, cx = x + w * 0.5f;
        gfx_xform_push(0, 0, 1, 0, o);
        rrect(cx - 32, cy, 64, 64, 32, C_SEG);
        ico("usb_full", cx, cy + 32, 30, C_SUB);
        t_center(700, 22, cx, cy + 74 + l1 * 0.5f, A.usb_conn ? "No photos" : "No USB drive", C_TEXT);
        t_center(600, 18, cx, cy + 84 + l1 + l2 * 0.5f, A.usb_conn ? "This drive has no photos" : "Insert a USB drive with photos", C_SUB);
        gfx_xform_pop();
        return;
    }
    rrect(x, y, w, h, PANEL_R - 1, hexa(0, o));
    for (i = 0; i < 2; i++) {
        int idx = i ? pi : ph_prev;
        float a = (i ? tw_get(&ph_cur, now) : 1) * o;
        gfx_tex *t;
        if (idx < 0 || a < 0.005f || !(t = media_thumb(idx))) continue;
        {
            double since = now - ph_t0 - floor((now - ph_t0) / PH_S) * PH_S;   /* scale(1.08) over 7 s linear */
            float zs = i ? 1 + 0.08f * (float)fmin(1, since / 7.0) : 1.08f;
            float ar = w / h, ia = (float)t->w / t->h, uw = ar < ia ? ar / ia : 1, vh = ar < ia ? 1 : ia / ar;
            uw /= zs; vh /= zs;
            gfx_image_round(t, D(x), D(y), D(w), D(h), 0.5f - uw * 0.5f, 0.5f - vh * 0.5f, 0.5f + uw * 0.5f, 0.5f + vh * 0.5f,
                            hexa(0xFFFFFF, a), D(PANEL_R - 1));
        }
    }
    gfx_vgrad_round(D(x), D(y), D(w), D(110), hexa(0, 0.55f * o), 1, D(PANEL_R - 1));   /* linear-gradient(rgba(0,0,0,.55), 0) */
}

static void round_btn(int id, float cx, float cy, float d, col bg, const char *icon, float isz, col fg, double now)
{
    float k = press_k(id, now), r = d * 0.5f * k;
    if (bg.a > 0.005f) rrect(cx - r, cy - r, r * 2, r * 2, r, bg);
    if (k < 1) rrect(cx - r, cy - r, r * 2, r * 2, r, hexa(0, press_dim(k) * (bg.a > 0.005f ? 1 : 0)));
    ico(icon, cx, cy, isz * k, col_alpha(fg, 1 - (bg.a > 0.005f ? 0 : press_dim(k))));
}

static void draw_dyn(double now)
{
    np_t n = np_now(now);
    char a[16], b[40], lab[40];
    float hw, sc;
    int i, wi = A.widget % NW;
    if (!music_dash_play()) { music_idle_dyn(now); goto widget; }
    snprintf(lab, sizeof lab, tr("Music · %s"), A.src_usb ? "USB" : "Bluetooth");
    hw = t_width(700, 21, lab);
    for (i = 0; i < 4; i++) {                       /* equalizer: after the caption, margin-left 4 + gap 12, height 18 */
        float h = tw_get(&A.eq[i], now);
        rrect(MX + 50 + hw + 16 + i * 7, 71 - h, 4, h, 2, C_ACCENT);
    }
    sc = tw_get(&A.art_sc, now) * press_k(H_ART, now);
    {                                                /* cover 230, radius 26, shadow 0 24px 56px rgba(0,0,0,.35) */
        float s = ART * sc, x = MX + ART * 0.5f - s * 0.5f, y = art_y() + ART * 0.5f - s * 0.5f;
        shadow(x, y, s, s, 26 * sc, 56, 0, 24, hexa(0, 0.35f));
        gfx_tex *art = n.av && !n.idle && A.src_usb ? media_art(A.trk) : n.av && !n.idle ? np_aa_art() : NULL;   /* cover from MP3 */
        if (art) {
            art_draw(art, x, y, s, 26 * sc);
        } else {
            rrect(x, y, s, s, 26 * sc, n.av ? hexc(n.hue) : C_SEG);
            ico("music_w13", MX + ART * 0.5f, art_y() + ART * 0.5f, 84 * sc, hexa(0xFFFFFF, 0.85f));
        }
    }
    if (mq_any) track_text(now, 1);
    gfx_xform_push(0, 0, 1, 0, n.av ? 1 : 0.35f);  /* mCtlOp: bar, time, buttons */
    {
        float by = bar_y(), frac = n.av && !n.idle && n.len > 0 ? fminf(1, (float)(n.pos / n.len)) : 0, cy = btn_cy(), ty = by + 6 + 12 + t_lh(600, 20) * 0.5f;
        int live = n.av && !n.idle && n.live;
        if (live) live_row(MX, by + 22, n.playing);         /* v35 dLive: live broadcast - instead of bar and time */
        else {
        rrect(MX, by, MW, 6, 3, C_TRACK);
        rrect(MX, by, MW * frac, 6, 3, C_TEXT);
        if (A.src_usb) rrect(MX + 10 + (MW - 20) * frac - 10, by - 7, 20, 20, 10, C_TEXT);   /* thumb - USB only (canSeek) */
        if (n.av && !n.idle && audio_loaded()) { fmt_time(a, sizeof a, n.pos); fmt_time(b, sizeof b, n.len); }   /* duration on the right */
        else { snprintf(a, sizeof a, "\xE2\x80\x93:\xE2\x80\x93\xE2\x80\x93"); snprintf(b, sizeof b, "%s", a); }
        text_tnum(1);
        t_left(600, 20, MX, ty, a, C_SUB);
        t_right(600, 20, MX + MW, ty, b, C_SUB);
        text_tnum(0);
        }
        round_btn(H_SHUF, btn_cx(0), cy, 64, col_alpha(C_TRACK, tw_get(&shuf_bg, now)), "shuffle", 28, C_TEXT, now);
        round_btn(H_PREV, btn_cx(1), cy, 80, hexa(0, 0), "prev", 44, C_TEXT, now);
        round_btn(H_PLAY, btn_cx(2), cy, 104, C_INV, n.playing ? "pause" : "play", n.playing ? 42 : 44, C_ONINV, now);
        round_btn(H_NEXT, btn_cx(3), cy, 80, hexa(0, 0), "next", 44, C_TEXT, now);
        round_btn(H_REP, btn_cx(4), cy, 64, col_alpha(C_TRACK, tw_get(&rep_bg, now)), "repeat", 28, C_TEXT, now);
    }
    gfx_xform_pop();
widget:
    /* v31: widget - header 38 (widget-colored tile 38, 21/700; white for photo), swap button - circle 38 inside a 50 button
     * (180 rotation on swap, spring .5s); photo - background of the whole card under the header */
    {
        float o = tw_get(&w_op, now), sc_ = tw_get(&w_s, now), dy = tw_get(&w_dy, now);
        if (wi == 1 && o > 0.005f) photos(now, o);
        rrect(854, WHEAD_CY - 19, 38, 38, 11, hexc(W_COL[wi]));
        ico(W_ICON[wi], 873, WHEAD_CY, 20, C_WHITE);
        int dark = wi == 1 && NIMAGES;                                     /* header over the photo - light */
        t_left(700, 21, 904, WHEAD_CY, W_TITLE[wi], dark ? hexc(0xF5F5F7) : C_SUB);
        {
            float kk = press_k(H_SWAP, now), r = 19 * kk;   /* v35: squircle 38/13 (was a circle) */
            rrect(1219 - r, WHEAD_CY - r, r * 2, r * 2, 13 * kk, dark ? hexa(0, 0.35f) : C_ACC_SOFT);
            ico_rot("swap", 1219, WHEAD_CY, 21 * kk, col_alpha(dark ? hexc(0xF5F5F7) : C_ACCENT, 1 - press_dim(kk)), tw_get(&w_spin, now));
        }
        /* widget content: exit .15s, enter .35s / .5s (cubic-bezier(.2,.9,.25,1)) */
        if (wi == 0 && o > 0.005f) {
            gfx_xform_push(D(WX + WW * 0.5f), D(WY + WH * 0.5f), sc_, D(dy), o);
            clip_push(830, WY - 20, 432, WH + 30);
            conn_rows(now);
            clip_pop();
            gfx_xform_pop();
        }
    }
    radio_card(now);
}

static void update(double now)
{
    int pi;
    radio_update(now);
    {
        const gfx_tex *art = dash_art();
        if (art_bg_update(&bg, art, art ? (long)np_now(0).id * 4096 + (long)art->id : 0)) screen_dirty();
    }
    if (np_now(now).id + music_dash_play() * 1000000 != mq_trk) { mq_trk = np_now(now).id + music_dash_play() * 1000000; mq_t0 = now; screen_dirty(); layout_again(); damage_d(122, 18, 690, 636); }
    if (mq_any) damage_d(TX - 4, art_y(), MX + MW - TX + 8, ART);   /* marquee */
    np_t n = np_now(now);
    if (n.playing) damage_d(MX - 4, bar_y() - 9, MW + 8, 24);
    if ((int)n.pos != last_sec) { last_sec = (int)n.pos; damage_d(MX - 4, bar_y() - 9, MW + 8, 24 + 12 + t_lh(600, 20) + 4); }
    if (any_active(A.eq, 4, now)) damage_d(194, 50, 430, 24);
    if (tw_active(&A.art_sc, now)) damage_d(MX - 60, art_y() - 40, ART + 120, ART + 130);
    if (tw_active(&shuf_bg, now) || tw_active(&rep_bg, now)) damage_d(MX, btn_cy() - 52, MW, 104);
    /* widget swap */
    if (w_phase == 1 && now - w_t >= 0.16) {
        A.widget++; w_phase = 2; w_t = now;
        tw_set(&w_op, 0, 0, &LINEAR, now); tw_set(&w_s, 0.97f, 0, &LINEAR, now); tw_set(&w_dy, 14, 0, &LINEAR, now);
        layout_again();
    } else if (w_phase == 2 && now - w_t >= 0.04) {
        w_phase = 0;
        tw_set(&w_op, 1, 350, &EASE, now); tw_set(&w_s, 1, 500, &ENTER, now); tw_set(&w_dy, 0, 500, &ENTER, now);
    }
    if (w_phase || tw_active(&w_op, now) || tw_active(&w_s, now) || tw_active(&w_spin, now)) damage_d(830, 18, 432, CARD1_H);
    /* photo: frame change every 6 s (opacity 1.2s), slow zoom - constant */
    pi = photo_idx(now);
    if (pi != ph_idx) { ph_prev = ph_idx; ph_idx = pi; tw_init(&ph_cur, 0); tw_set(&ph_cur, 1, 1200, &EASE, now); }
    if (A.widget % NW == 1) damage_d(830, 18, 432, CARD1_H);      /* photo: zoom and change - whole card */
}

static void click(int id, double now)
{
    if (id == H_PLAY) { np_play(now); screen_dirty(); }
    else if (id == H_SHUF) { A.shuffle = !A.shuffle; np_mode(); tw_set(&shuf_bg, (float)A.shuffle, 300, &EASE, now); }
    else if (id == H_REP) { A.repeat = !A.repeat; np_mode(); tw_set(&rep_bg, (float)A.repeat, 300, &EASE, now); }
    else if (id == H_NEXT) { np_next(now); screen_dirty(); }
    else if (id == H_PREV) { np_prev(now); screen_dirty(); }
    else if (id == H_MUSIC_HEAD || id == H_ART) go_screen(S_MUSIC, now);
    else if (id == H_SWAP) {
        w_phase = 1; w_t = now;
        tw_set(&w_op, 0, 150, &EASE, now); tw_set(&w_s, 0.97f, 150, &EASE, now); tw_set(&w_dy, -8, 150, &EASE, now);
        tw_set(&w_spin, (A.widget + 1) * 180.0f, 500, &SPRING, now);
    } else if (id == H_W0) { if (A.usb_conn) { A.usb_path = 0; go_screen(S_USB, now); } }
    else if (id == H_W2) go_screen(S_BT, now);
    else if (id == H_PHOTO && NIMAGES) { A.usb_path = 3; A.usb_img = A.usb_last = photo_idx(now); go_screen(S_USB, now); }
    else if (id == H_RADIO) go_screen(S_RADIO, now);
    else if (id == H_RPREV || id == H_RNEXT) radio_jump(id == H_RNEXT ? 1 : -1);
    else if (id == H_RFAV) radio_fav_toggle(now);
    else if (id == H_RON) { A.radio_on = 1; layout_again(); }
    else if (id == H_IUSB) { np_source(1, now); A.m_loaded = 1; A.playing = 1; screen_dirty(); layout_again(); }
    else if (id == H_IBT) { np_source(0, now); A.m_loaded = 1; bt_media(1); screen_dirty(); layout_again(); }
}

/* seek (USB only): released - seek; when paused - also play (as in Music) */
static void drag(int id, float x, float y, int phase, double now)
{
    (void)y;
    if (id != H_SEEK || !A.src_usb) return;
    if (phase == 2) { np_seek(floor(slider_value(MX, MW, x) * trk_at(A.trk)->len)); if (!A.playing) np_play(now); }
    else A.pos = floor(slider_value(MX, MW, x) * trk_at(A.trk)->len);
    damage_d(MX - 12, bar_y() - 9, MW + 24, 24 + 12 + t_lh(600, 20) + 4);
}

static void enter(double now)
{
    tw_init(&shuf_bg, (float)A.shuffle);
    tw_init(&rep_bg, (float)A.repeat);
    tw_init(&r_needle, radio_frac()); r_last = radio_frac(); r_sig = -1;
    last_sec = -1;
    if (ph_t0 == 0) ph_t0 = now;
    w_phase = 0;
    tw_init(&w_op, 1); tw_init(&w_s, 1); tw_init(&w_dy, 0); tw_init(&w_spin, A.widget * 180.0f);
    ph_idx = photo_idx(now);
    ph_prev = -1; tw_init(&ph_cur, 1);
}

/* steering wheel: radio playing - radio widget buttons, otherwise music widget */
static int hw_btn(int d, int held)
{
    if (held) return -1;
    if (A.radio_on) return d > 0 ? H_RNEXT : H_RPREV;
    return music_any() ? (d > 0 ? H_NEXT : H_PREV) : -1;
}

const screen_def SCR_DASH = { layout, draw_static, draw_dyn, update, click, enter, drag, hw_btn };
