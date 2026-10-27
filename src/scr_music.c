/* Music screen: Now Playing (equalizer), Bluetooth/USB source, cover 230 + title, slider, buttons;
 * for USB a playlist on the right (300 tracks, A–Z / Date, scrolling). Design: Main.dc.html, is.music.
 * v14: a disconnected source - button 0.35 and a placeholder; none at all - "No music source" screen. */
#include "ui_int.h"
#include "i18n.h"
#include "media.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { H_BT = HIT_SCREEN, H_USB, H_SHUF, H_PREV, H_PLAY, H_NEXT, H_REP, H_AZ, H_DATE, H_SEEK, H_LIST, H_ROW0,
       H_GOBT = H_ROW0 + 16 };
#define SLOTS 16                  /* buttons of visible playlist rows (circular) */

static tween src_th, shuf_bg, rep_bg, sort_th;
static int last_sec = -1, last_trk = -1, slot_row[SLOTS], last_gen = -1, last_loaded = -1;
#define sort_date A.pl_date
#define order playlist()
static scroller sc;
static float last_off = -1;

#define X0 PX0                    /* v26: padding 24 */
#define Y0 PY0
#define SEG_X (X0 + PCW - 290)
#define ROW_Y (Y0 + 58 + 18)      /* header 58 (source switch) + 18 */
#define ROW_H (PBOT - ROW_Y)
#define PL_W 440.0f
#define PL_X (X0 + PCW - PL_W)
#define LIST_Y (ROW_Y + 14 + t_lh(700, 24) + 2 + t_lh(600, 20) + 10)   /* playlist header */
#define LIST_H (ROW_Y + ROW_H - LIST_Y)
#define RW 412.0f                 /* row: 440 - padding 10*2 - scrollbar 8 */
#define PITCH 82.0f               /* row 80 (padding 12, tile 56) + gap 2 */

/* A–Z/Date sort - centered in the header (align-items: center) */
static float sort_y(void) { return ROW_Y + 14 + (t_lh(700, 24) + 2 + t_lh(600, 20) - 46) * 0.5f; }
static int show_pl(void) { return A.src_usb && music_usb_av(); }   /* playlist - only USB with music */
static float col_w(void) { return show_pl() ? PCW - 28 - PL_W : PCW; }

/* ---- "No music source" (v14 mNone): centered: circle 96, title, caption, two 380x132 cards */
#define NS_BOT PBOT
static float ns_h(void) { return 96 + 12 + 6 + t_lh(800, 34) + 12 + t_lh(600, 21) + 12 + 22 + 132; }
static float ns_y(void) { float top = Y0 + 52; return top + (NS_BOT - top - ns_h()) * 0.5f; }   /* below the header 52 */
static float card_y(void) { return ns_y() + ns_h() - 132; }
#define CARD_X (X0 + PCW * 0.5f - 389)
/* v16: for Bluetooth cover 300 and larger text, below it a "phone · Bluetooth" pill */
static float art_sz(void) { return A.src_usb ? 230 : 300; }
static float t_sz(void) { return A.src_usb ? 36 : 46; }
static float a_sz(void) { return A.src_usb ? 24 : 28; }
static float al_sz(void) { return A.src_usb ? 20 : 22; }
static int show_from(void) { return !A.src_usb && music_bt_av(); }
static float blk_y(void) { return ROW_Y + (ROW_H - (art_sz() + 30 + 6 + 12 + t_lh(600, 20) + 18 + 104)) * 0.5f; }
static float bar_y(void) { return blk_y() + art_sz() + 30; }
static float btn_cy(void) { return bar_y() + 6 + 12 + t_lh(600, 20) + 18 + 52; }
static float btn_cx(int i)
{
    static const float S[5] = { 64, 80, 104, 80, 64 };
    float g = (col_w() - 392) / 4, x = X0;
    int k;
    for (k = 0; k < i; k++) x += S[k] + g;
    return x + S[i] * 0.5f;
}

static void layout(void)
{
    int i;
    if (!music_any()) { set_hit(H_GOBT, CARD_X, card_y(), 380, 132); return; }
    if (music_bt_av()) set_hit(H_BT, SEG_X + 5, Y0 + 5, 140, 48);
    if (music_usb_av()) set_hit(H_USB, SEG_X + 145, Y0 + 5, 140, 48);
    if (!np_now(0).av) goto playlist;                           /* pointer-events: none */
    set_hit(H_SHUF, btn_cx(0) - 32, btn_cy() - 32, 64, 64);
    set_hit(H_PREV, btn_cx(1) - 40, btn_cy() - 40, 80, 80);
    set_hit(H_PLAY, btn_cx(2) - 52, btn_cy() - 52, 104, 104);
    set_hit(H_NEXT, btn_cx(3) - 40, btn_cy() - 40, 80, 80);
    set_hit(H_REP, btn_cx(4) - 32, btn_cy() - 32, 64, 64);
    if (A.src_usb) set_drag(H_SEEK, X0 - 10, bar_y() - 13, col_w() + 20, 32);   /* phone: no seeking (DEVIATIONS) */
playlist:
    if (!show_pl()) return;
    set_hit(H_AZ, PL_X + PL_W - 14 - 150 + 3, sort_y() + 3, 72, 40);
    set_hit(H_DATE, PL_X + PL_W - 14 - 150 + 75, sort_y() + 3, 72, 40);
    set_scroll(H_LIST, PL_X, LIST_Y, PL_W, LIST_H);
    for (i = 0; i < SLOTS; i++) slot_row[i] = -1;
    {
        int first = (int)(sc.off / PITCH), r;
        for (r = first; r < NTRACKS && r < first + SLOTS; r++) {
            float y0 = LIST_Y + r * PITCH - sc.off, y1 = y0 + 80;
            if (y0 < LIST_Y) y0 = LIST_Y;
            if (y1 > LIST_Y + LIST_H) y1 = LIST_Y + LIST_H;
            if (y1 <= y0) continue;
            slot_row[r % SLOTS] = r;
            set_hit(H_ROW0 + r % SLOTS, PL_X + 10, y0, RW, y1 - y0);
        }
    }
}

/* player rows: what doesn't fit scrolls as a marquee (2 s still, 40 px/s left to the end, 2 s still, restart).
 * Static ones are drawn into the cache layer (draw_static), scrolling ones every frame (draw_dyn). */
static double mq_t0;
static int mq_any;

static void title_line(int w, float size, float x, float y, float maxw, const char *s, col c, float ls, double now, int dyn)
{
    if (marquee(w, size, x, y, maxw, s, c, ls, mq_t0, now, dyn)) mq_any = 1;
}

static void player_text(double now, int dyn)
{
    np_t n = np_now(now);
    float as = art_sz(), ts = t_sz(), tx = X0 + as + 28, mw = col_w() - as - 28, h1 = ts * 1.08f, h2 = t_lh(600, a_sz());
    int alb = n.av && !n.idle && n.al[0];                         /* album - if present (the phone often doesn't provide it) */
    float h3 = alb ? 6 + t_lh(500, al_sz()) : 0, h4 = show_from() ? 22 + 48 : 0;
    float ty = blk_y() + as * 0.5f - (h1 + 10 + h2 + h3 + h4) * 0.5f, y;
    if (!dyn) mq_any = 0;
    title_line(800, ts, tx, ty + (h1 - t_lh(800, ts)) * 0.5f, mw, n.t, C_TEXT, -0.02f * ts, now, dyn);
    title_line(600, a_sz(), tx, ty + h1 + 10, mw, n.a, C_SUB, 0, now, dyn);
    y = ty + h1 + 10 + h2;
    if (alb) { title_line(500, al_sz(), tx, y + 6, mw, n.al, C_MUTED, 0, now, dyn); y += h3; }
    if (show_from() && !dyn) {                                     /* pill: circle 34 with phone, name, "Bluetooth" */
        const char *nm = bt_phone_name();
        float nw = t_width(700, 19, nm), w = 8 + 34 + 12 + nw + 12 + t_width(600, 18, "Bluetooth") + 18;
        y += 22;
        rrect(tx, y, w, 48, 24, C_INNER);
        rrect(tx + 8, y + 7, 34, 34, 17, C_ACCENT);                  /* v19 */
        ico("device_w22", tx + 25, y + 24, 18, C_ONACCENT);
        t_left(700, 19, tx + 54, y + 24, nm, C_TEXT);
        t_left(600, 18, tx + 54 + nw + 12, y + 24, "Bluetooth", C_SUB);
    }
}

static void nosrc_static(void)
{
    float y = ns_y(), cx = X0 + PCW * 0.5f, l1 = t_lh(800, 34), l2 = t_lh(600, 21);
    screen_head(X0, Y0 + 26, 0xFF4F6A, "app_music_w22", "Music", C_TEXT);   /* v26 */
    rrect(cx - 48, y, 96, 96, 48, hexa(0xFF4F6A, 0.16f));
    ico("app_music", cx, y + 48, 46, hexc(0xFF4F6A));
    y += 96 + 12 + 6;
    t_center(800, 34, cx, y + l1 * 0.5f, "No music source", C_TEXT);
    y += l1 + 12;
    t_center(600, 21, cx, y + l2 * 0.5f, A.usb_conn ? "Connect your phone or add music to the USB drive." : "Connect your phone or plug in a USB drive to start playing.", C_SUB);   /* v15 phSub */
    {   /* second card - dashed, not a button */
        float x = CARD_X + 380 + 18, cy = card_y() + 66, h1 = t_lh(800, 23), h2 = t_lh(600, 18), ty = cy - (h1 + 4 + h2) * 0.5f;
        dash_rect(x, card_y(), 380, 132, 24, C_BORDER);
        rrect(x + 22, cy - 32, 64, 64, 18, C_ACCENT);
        ico("usb_full_w22", x + 54, cy, 30, C_ONACCENT);
        t_top(800, 23, x + 22 + 64 + 18, ty, A.usb_conn ? "No music on USB" : "Insert a USB drive", C_TEXT, 0);   /* v15 phUsbTitle/Sub */
        t_top(600, 18, x + 22 + 64 + 18, ty + h1 + 4, A.usb_conn ? "Add MP3, FLAC, AAC, WAV" : "MP3, FLAC, AAC, WAV", C_SUB, 0);
    }
}

static void nosrc_dyn(double now)
{
    float x = CARD_X, y = card_y(), w = 380, h = 132, k = press_box(H_GOBT, &x, &y, &w, &h, now);
    float cy = y + h * 0.5f, h1 = t_lh(800, 23), h2 = t_lh(600, 18), ty = cy - (h1 + 4 + h2) * 0.5f;
    rrect_b(x, y, w, h, 24 * k, C_INNER, C_BORDER);
    rrect(x + 22 * k, cy - 32 * k, 64 * k, 64 * k, 18 * k, C_ACCENT);
    ico("bluetooth_w22", x + 54 * k, cy, 30 * k, C_ONACCENT);
    t_top(800, 23, x + (22 + 64 + 18) * k, ty, "Connect a phone", C_TEXT, 0);
    t_fit(600, 18, x + (22 + 64 + 18) * k, ty + h1 + 4 + h2 * 0.5f, w - 22 - 64 - 18 - 18 - 24 - 22,
          A.bt_on ? "Open Bluetooth settings" : "Bluetooth is off \xC2\xB7 open settings", C_SUB);
    ico("chev_r_w24", x + w - (22 + 12) * k, cy, 24 * k, col_alpha(C_TEXT, 0.6f));
}

static art_bg bg = { { 0, 0, 0 }, -1 };   /* panel background - blurred cover, like the dashboard card (owner 2026-10-26) */

static void draw_static(void)
{
    panel(ST_X, ST_Y, ST_W, ST_H, PANEL_R);
    if (!music_any()) { nosrc_static(); return; }
    art_bg_draw(&bg, ST_X, ST_Y, ST_W, ST_H, PANEL_R);
    screen_head(X0, Y0 + 29, 0xFF4F6A, "app_music_w22", "Music", C_TEXT);   /* v26: instead of "Now Playing" */
    seg_static(SEG_X, Y0, 2, 140, 48);
    player_text(0, 0);
    if (np_now(0).av && !np_now(0).live) rrect(X0, bar_y(), col_w(), 6, 3, C_TRACK);   /* without a source - in draw_dyn, with opacity */
    if (show_pl()) {
        char sub[48];
        float th = t_lh(700, 24), y = ROW_Y + 14;
        rrect(PL_X, ROW_Y, PL_W, ROW_H, 20, C_INNER);
        t_top(700, 24, PL_X + 22, y, "Playlist", C_SUB, 0);
        snprintf(sub, sizeof sub, tr_n("%d songs \xC2\xB7 USB drive", (long)(NTRACKS)), NTRACKS);
        t_top(600, 20, PL_X + 22, y + th + 2, sub, C_MUTED, 0);
    }
}

static void round_btn(int id, float cx, float cy, float d, col bg, const char *icon, float isz, col fg, double now)
{
    float k = press_k(id, now), r = d * 0.5f * k;
    if (bg.a > 0.005f) rrect(cx - r, cy - r, r * 2, r * 2, r, bg);
    if (k < 1) rrect(cx - r, cy - r, r * 2, r * 2, r, hexa(0, press_dim(k) * (bg.a > 0.005f ? 1 : 0)));
    ico(icon, cx, cy, isz * k, col_alpha(fg, 1 - (bg.a > 0.005f ? 0 : press_dim(k))));
}

static void draw_row(int r, double now)
{
    static const char *MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int i = order[r], cur = i == A.trk;
    const track_t *t = &TRACKS[i];
    float x = PL_X + 10, y = LIST_Y + r * PITCH - sc.off, w = RW, h = 80, k = press_box(H_ROW0 + r % SLOTS, &x, &y, &w, &h, now);
    float h1 = t_lh(700, 25), h2 = t_lh(600, 21), ty = y + h * 0.5f - (h1 + 2 + h2) * 0.5f, lw;
    char len[16], sub[160];
    col pink = C_ACCENT;                                         /* v17: selected track - accent */
    fmt_time(len, sizeof len, t->len);
    if (sort_date) {
        time_t tt = (time_t)t->added; struct tm tm; localtime_r(&tt, &tm);
        snprintf(sub, sizeof sub, tr("Added %d %s %d \xC2\xB7 %s"), tm.tm_mday, tr(MON[tm.tm_mon]), tm.tm_year + 1900, t->a);
    } else snprintf(sub, sizeof sub, "%s \xC2\xB7 %s", t->a, t->al);
    lw = t_width(700, 22, len);
    if (cur) rrect(x, y, w, h, 16 * k, C_INNER);
    rrect(x + 14 * k, y + 12 * k, 56 * k, 56 * k, 13 * k, cur ? pink : C_ACC_SOFT);
    ico(cur && A.playing ? "pl_bars" : "app_music_w22", x + 42 * k, y + h * 0.5f, 26 * k, cur ? C_ONACCENT : pink);
    t_fit(700, 25, x + 84 * k, ty + h1 * 0.5f, w - 84 - 14 - lw - 14, t->t, col_alpha(cur ? pink : C_TEXT, 1 - press_dim(k)));
    t_fit(600, 21, x + 84 * k, ty + h1 + 2 + h2 * 0.5f, w - 84 - 14 - lw - 14, sub, C_SUB);
    t_right(700, 22, x + w - 14 * k, y + h * 0.5f, len, C_SUB);
}

static void draw_dyn(double now)
{
    static const char *SRC_LABEL[2] = { "Bluetooth", "USB" };
    static const char *SRC_ICON[2] = { "bluetooth", "usb" };
    static const char *SORT[2] = { "A\xE2\x80\x93Z", "Date" };
    np_t n = np_now(now);
    char a[16], b[24];
    float frac = n.len > 0 ? (float)(n.pos / n.len) : 0, sc_ = tw_get(&A.art_sc, now), cy = btn_cy(), by = bar_y();
    float hw = 56 + t_width(800, 28, "Music"), W = col_w();
    float src_op[2] = { music_bt_av() ? 1 : 0.35f, music_usb_av() ? 1 : 0.35f };
    int i;
    if (!music_any()) { nosrc_dyn(now); return; }
    for (i = 0; i < 4; i++) {                       /* equalizer next to "Now Playing" */
        float h = tw_get(&A.eq[i], now);
        rrect(X0 + hw + 14 + i * 7, Y0 + 29 + 9 - h, 4, h, 2, C_ACCENT);   /* v26: after the title, gap 14 */
    }
    seg_item_op = src_op;
    seg_dyn(SEG_X, Y0, 2, 140, 48, tw_get(&src_th, now), SRC_LABEL, SRC_ICON, 700, 20, 18, H_BT, now);
    {                                                /* cover 230 / 300 (BT), shadow 0 24px 56px rgba(0,0,0,.35) */
        float as = art_sz(), s = as * sc_, x = X0 + as * 0.5f - s * 0.5f, y = blk_y() + as * 0.5f - s * 0.5f;
        shadow(x, y, s, s, 26 * sc_, 56, 0, 24, hexa(0, 0.35f));
        gfx_tex *art = n.av && !n.idle && A.src_usb ? media_art(A.trk) : n.av && !n.idle ? np_aa_art() : NULL;   /* cover from MP3 - if present */
        if (art) {
            art_draw(art, x, y, s, 26 * sc_);
        } else {
            rrect(x, y, s, s, 26 * sc_, n.av ? hexc(n.hue) : C_SEG);
            ico("music_w13", X0 + as * 0.5f, blk_y() + as * 0.5f, (A.src_usb ? 84 : 110) * sc_, hexa(0xFFFFFF, 0.85f));
        }
    }
    if (mq_any) player_text(now, 1);
    gfx_xform_push(0, 0, 1, 0, n.av ? 1 : 0.35f);                /* v14: slider, time, buttons - 0.35 without a source */
    int live = n.av && !n.idle && n.live;
    if (live) live_row(X0, by + 22, n.playing);                  /* live broadcast - like the dashboard's dLive (DEVIATIONS: not in the Music design) */
    else {
    if (!n.av) rrect(X0, by, W, 6, 3, C_TRACK);
    rrect(X0, by, W * frac, 6, 3, C_TEXT);
    if (A.src_usb) rrect(X0 + 10 + (W - 20) * fminf(1, frac) - 10, by - 7, 20, 20, 10, C_TEXT);   /* v25: thumb inside the track; v31: USB only (canSeek) */
    if (n.av && !n.idle && audio_loaded()) { fmt_time(a, sizeof a, n.pos); fmt_time(b, sizeof b, n.len); }   /* v14: duration on the right */
    else { snprintf(a, sizeof a, "\xE2\x80\x93:\xE2\x80\x93\xE2\x80\x93"); snprintf(b, sizeof b, "%s", a); }
    t_left(600, 20, X0, by + 6 + 12 + t_lh(600, 20) * 0.5f, a, C_SUB);
    t_right(600, 20, X0 + W, by + 6 + 12 + t_lh(600, 20) * 0.5f, b, C_SUB);
    }
    round_btn(H_SHUF, btn_cx(0), cy, 64, col_alpha(C_TRACK, tw_get(&shuf_bg, now)), "shuffle", 28, C_TEXT, now);
    round_btn(H_PREV, btn_cx(1), cy, 80, hexa(0, 0), "prev", 44, C_TEXT, now);
    round_btn(H_PLAY, btn_cx(2), cy, 104, C_INV, n.playing ? "pause" : "play", n.playing ? 42 : 44, C_ONINV, now);
    round_btn(H_NEXT, btn_cx(3), cy, 80, hexa(0, 0), "next", 44, C_TEXT, now);
    round_btn(H_REP, btn_cx(4), cy, 64, col_alpha(C_TRACK, tw_get(&rep_bg, now)), "repeat", 28, C_TEXT, now);
    gfx_xform_pop();
    if (show_pl()) {
        int first = (int)(sc.off / PITCH), r;
        seg_plain(PL_X + PL_W - 14 - 150, sort_y(), 2, 72, 40, 3, tw_get(&sort_th, now), SORT, 17, H_AZ, now);
        clip_push(PL_X, LIST_Y, PL_W, LIST_H);
        for (r = first; r < NTRACKS && r < first + SLOTS; r++) {
            if (LIST_Y + r * PITCH - sc.off > LIST_Y + LIST_H) break;
            draw_row(r, now);
        }
        {   /* scrollbar-width: thin, c.track */
            float th = fmaxf(30, LIST_H * LIST_H / (LIST_H + sc.max));
            rrect(PL_X + PL_W - 8, LIST_Y + (LIST_H - th) * (sc.max > 0 ? sc.off / sc.max : 0), 8, th, 4, C_TRACK);
        }
        clip_pop();
    }
}

static void update(double now)
{
    np_t n = np_now(now);
    static int last_st = -1;
    {
        const gfx_tex *art = !music_any() || !n.av || n.idle ? NULL : A.src_usb ? media_art(A.trk) : np_aa_art();
        if (art_bg_update(&bg, art, art ? (long)n.id * 4096 + (long)art->id : 0)) screen_dirty();
    }
    tw_set(&src_th, (float)A.src_usb, SPRING_MS, &SPRING_SOFT, now);   /* source changed without a press (BT started playing from the phone) */
    if (media_gen() != last_gen) {                              /* USB stick: new tracks */
        last_gen = media_gen();
        sc.max = NTRACKS * PITCH - 2 + 10 - LIST_H;
        if (sc.max < 0) sc.max = 0;
        if (sc.off > sc.max) sc.off = sc.max;
        screen_dirty(); layout_again(); damage_d(ST_X, ST_Y, ST_W, ST_H);
    }
    int st = music_any() * 8 + n.av * 4 + A.src_usb * 2 + A.usb_conn + music_usb_av() * 16;
    if (st != last_st) { last_st = st; screen_dirty(); layout_again(); damage_d(ST_X, ST_Y, ST_W, ST_H); }
    {   /* thumb moved by a pixel - redraw the bar, also when paused (dragging the slider after video left a trail) */
        static int last_px = -1;
        int px = n.len > 0 ? (int)(col_w() * fmin(1, n.pos / n.len)) : 0;
        if (px != last_px) { last_px = px; damage_d(X0 - 12, bar_y() - 10, col_w() + 24, 28); }
    }
    if ((int)n.pos != last_sec || audio_loaded() != last_loaded) {   /* time / "-:--" (track not open) */
        last_sec = (int)n.pos; last_loaded = audio_loaded();
        damage_d(X0, bar_y() + 8, col_w(), 30);
    }
    if (n.id != last_trk) {
        last_trk = n.id; mq_t0 = now; screen_dirty(); damage_d(ST_X, ST_Y, ST_W, ST_H);
        if (show_pl() && !sc.drag) {                /* the playing track left the visible part - scroll to it */
            float y = playlist_pos(A.trk) * PITCH;
            if (y < sc.off || y + 80 > sc.off + LIST_H) {
                sc.off = fminf(sc.max, fmaxf(0, y - (LIST_H - 80) * 0.5f)); sc.v = 0;
                layout_again();
            }
        }
    }
    if (mq_any) damage_d(X0 + art_sz() + 28, blk_y(), col_w() - art_sz() - 28, art_sz());   /* marquee */
    if (any_active(A.eq, 4, now)) damage_d(X0 + 140, Y0 + 10, 80, 40);
    if (tw_active(&src_th, now)) damage_d(SEG_X, Y0, 290, 58);
    if (tw_active(&sort_th, now)) damage_d(PL_X + PL_W - 14 - 150 - 8, sort_y() - 8, 150 + 16, 46 + 16);   /* whole switch + margin for the spring */
    if (tw_active(&shuf_bg, now) || tw_active(&rep_bg, now)) damage_d(X0, btn_cy() - 60, col_w(), 120);
    if (tw_active(&A.art_sc, now)) damage_d(X0 - 70, blk_y() - 50, 450, 430);
    if (sc_update(&sc, now) || sc.off != last_off) { last_off = sc.off; damage_d(PL_X, LIST_Y, PL_W, LIST_H); layout_again(); }
    if (press_active(now)) damage_d(PL_X, LIST_Y, PL_W, LIST_H);
}

static void click(int id, double now)
{
    if (id == H_GOBT) { go_screen(S_BT, now); return; }
    if (id == H_BT || id == H_USB) {
        np_source(id == H_USB, now);
        tw_set(&src_th, (float)A.src_usb, SPRING_MS, &SPRING_SOFT, now);
        screen_dirty(); layout_again(); damage_d(ST_X, ST_Y, ST_W, ST_H);
    } else if (id == H_SHUF) { A.shuffle = !A.shuffle; np_mode(); tw_set(&shuf_bg, (float)A.shuffle, 300, &EASE, now); }
    else if (id == H_REP) { A.repeat = !A.repeat; np_mode(); tw_set(&rep_bg, (float)A.repeat, 300, &EASE, now); }
    else if (id == H_PLAY) { np_play(now); damage_d(PL_X, LIST_Y, PL_W, LIST_H); }
    else if (id == H_NEXT) { np_next(now); screen_dirty(); }
    else if (id == H_PREV) { np_prev(now); screen_dirty(); }
    else if (id == H_AZ || id == H_DATE) {
        sort_date = id == H_DATE;
        tw_set(&sort_th, (float)sort_date, SPRING_MS, &SPRING_SOFT, now);
        damage_d(PL_X, LIST_Y, PL_W, LIST_H);
    } else if (id >= H_ROW0 && id < H_ROW0 + SLOTS && slot_row[id - H_ROW0] >= 0) {
        A.trk = order[slot_row[id - H_ROW0]]; A.pos = 0; A.playing = 1; A.m_loaded = 1;
        tw_set(&A.art_sc, 1, 700, &SPRING, now);
        screen_dirty();
    }
}

static void drag(int id, float x, float y, int phase, double now)
{
    (void)x;
    if (id == H_LIST) sc_drag(&sc, y, phase, now);
    else if (id == H_SEEK && A.src_usb && phase == 2) {           /* released - seek; when paused - also play (owner) */
        np_seek(floor(slider_value(X0, col_w(), x) * trk_at(A.trk)->len));
        if (!A.playing) np_play(now);
    }
    else if (id == H_SEEK && A.src_usb) A.pos = floor(slider_value(X0, col_w(), x) * trk_at(A.trk)->len);
}

static void enter(double now)
{
    last_sec = last_trk = -1; last_off = -1; mq_t0 = now;
    /* selected source absent but another exists - open on it (owner: no stick but BT present - not "No USB drive") */
    if (A.src_usb && !music_usb_av() && music_bt_av()) np_source(0, now);
    else if (!A.src_usb && !music_bt_av() && music_usb_av()) np_source(1, now);
    tw_init(&src_th, (float)A.src_usb);
    tw_init(&shuf_bg, (float)A.shuffle);
    tw_init(&rep_bg, (float)A.repeat);
    tw_init(&sort_th, (float)sort_date);
    sc.max = NTRACKS * PITCH - 2 + 10 - LIST_H; sc.v = 0; sc.drag = 0;
    if (sc.max < 0) sc.max = 0;
    last_gen = media_gen();
    if (sc.off > sc.max) sc.off = sc.max;
}

/* steering wheel: previous/next - player buttons (hold - the core does the seeking, the button stays pressed) */
static int hw_btn(int d, int held) { return !held && music_any() && !A.radio_on ? (d > 0 ? H_NEXT : H_PREV) : -1; }

const screen_def SCR_MUSIC = { layout, draw_static, draw_dyn, update, click, enter, drag, hw_btn };
