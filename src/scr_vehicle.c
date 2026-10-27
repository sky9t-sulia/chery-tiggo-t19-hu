/* Vehicle screen: lights (Auto light, DRL, instrument backlight), safety (lock confirmation),
 * driving (speed alarm + threshold), instrument cluster (language, time). Design: Main.dc.html, is.vehicle.
 * Lights, lock confirmation and speed alarm are sent to the car (ui.c vehicle_sync + mcu_vehicle 0x50).
 * Instrument backlight, language and cluster time (0x53) - UI only for now.
 * Owner 2026-10-25: with ignition off (MCU 0x50 power not 2/3) the car does not accept settings - everything except close is
 * unavailable: opacity 0.4, not clickable (like the design's unavailable sources; DEVIATIONS.md). */
#include "ui_int.h"
#include "i18n.h"
#include "mcu.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

enum { H_CLOSE = HIT_SCREEN, H_RESET, H_AUTO, H_DRL, H_BL, H_LOCK0, H_LANG0 = H_LOCK0 + 2,
       H_HDN = H_LANG0 + 3, H_HUP, H_MDN, H_MUP };

#define PX PX0                       /* v26: padding 24 */
#define PY PY0
#define CW PCW
#define COLW ((CW - 24) / 2)       /* two columns, gap 24 */
#define C2 (PX + COLW + 24)
#define RO_A 0.35f                  /* read-only opacity (Speed alarm): light theme - light gray, dark - dark gray */

static tween auto_tw, drl_tw, spd_tw, bl_v, spd_v, lock_th, lang_th, coll_h, coll_op;
static int last_bl = -1, last_lim = -1, live = -1;
#define VA (A.car_live ? 1.0f : 0.4f)

static float lh15(void) { return t_lh(700, 19); }
static float lh16(void) { return t_lh(700, 20); }
static float box1(void) { return 115 + lh15() + 10; }
static float bl_y(void) { return box1() + 12 + 122 + 10; }                   /* top of the backlight row */
static float box1_h(void) { return 24 + 122 + 66 + lh16(); }
static float box2(void) { return box1() + box1_h() + 18 + lh15() + 10; }
static float coll_full(void) { return 1 + 10 + lh16() + 6 + 40 + 8 + 2; }
static float spd_y(void) { return box1() + 12 + 60 + 1 + 10; }               /* top of the "Alarm speed" row */
static float box3(float ch) { return box1() + 24 + 60 + ch + 18 + lh15() + 10; }

static void layout(void)
{
    float b1 = box1(), b2 = box2(), b3 = box3(A.speed_alarm ? coll_full() : 0), tx = C2 + COLW - 20 - 296;
    int i;
    set_hit(H_CLOSE, PX, PY, 52, 52);
    set_hit(H_RESET, PX + CW - reset_w(), PY + 2, reset_w(), 48);
    set_hit(H_AUTO, PX + COLW - 96, b1 + 12 + 8, 76, 44);
    set_hit(H_DRL, PX + COLW - 96, b1 + 12 + 61 + 8, 76, 44);
    set_drag(H_BL, PX + 20, bl_y() + lh16() + 6 - 6, COLW - 40, 52);
    for (i = 0; i < 2; i++) set_hit(H_LOCK0 + i, PX + COLW - 20 - 238 + 3 + i * 116, b2 + 12 + 10, 116, 40);
    /* Speed alarm is read-only: the car accepts a threshold neither from our HU nor from the stock one (factory bug,
     * confirmed by sniffing 2026-10-23: the stock unit sends the same 0x50[06], the car ignores it). We show it, not edit it. */
    for (i = 0; i < 3; i++) set_hit(H_LANG0 + i, C2 + COLW - 20 - 378 + 3 + i * 124, b3 + 12 + 10, 124, 40);
    set_hit(H_HDN, tx, b3 + 12 + 61 + 10, 40, 44);
    set_hit(H_HUP, tx + 96, b3 + 12 + 61 + 10, 40, 44);
    set_hit(H_MDN, tx + 160, b3 + 12 + 61 + 10, 40, 44);
    set_hit(H_MUP, tx + 256, b3 + 12 + 61 + 10, 40, 44);
}

static void draw_static(void)
{
    float b1 = box1(), b2 = box2(), l = lh15();
    panel(ST_X, ST_Y, ST_W, ST_H, PANEL_R);
    head_static(PX, PY, 0x3B5BDB, "car", 24, "Vehicle");
    gfx_xform_push(0, 0, 1, 0, VA);                                   /* ignition off - unavailable */
    t_left(700, 19, PX, b1 - 10 - l * 0.5f, "Lights", C_SUB);
    t_left(700, 19, PX, b2 - 10 - l * 0.5f, "Security", C_SUB);
    t_left(700, 19, C2, b1 - 10 - l * 0.5f, "Driving", C_SUB);
    rrect(PX, b1, COLW, box1_h(), 20, C_INNER);
    row_text(PX + 20, b1 + 12 + 30, COLW - 136, "Auto light", "Headlights turn on when it gets dark");
    rrect(PX + 20, b1 + 72, COLW - 40, 1, 0, C_TRACK);
    row_text(PX + 20, b1 + 12 + 61 + 30, COLW - 136, "Daytime Running Lights", "DRL");
    rrect(PX + 20, b1 + 133, COLW - 40, 1, 0, C_TRACK);
    rrect(PX, b2, COLW, 84, 20, C_INNER);
    row_text(PX + 20, b2 + 42, COLW - 40 - 248, "Lock confirmation", "Signal when the car locks");
    gfx_xform_pop();
}

static void time_btn(int id, float x, float y, const char *icon, double now)
{
    float w = 40, h = 44, k = press_box(id, &x, &y, &w, &h, now);
    rrect(x, y, w, h, 14 * k, C_INNER);
    ico(icon, x + w * 0.5f, y + h * 0.5f, 18 * k, col_alpha(C_TEXT, 1 - press_dim(k)));
}

static void draw_dyn(double now)
{
    static const char *LOCK[2] = { "Lights + Horn", "Lights only" };
    static const char *LANG[3] = { "English", "\xD0\xA3\xD0\xBA\xD1\x80\xD0\xB0\xD1\x97\xD0\xBD\xD1\x81\xD1\x8C\xD0\xBA\xD0\xB0",
                                   "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9" };
    float b1 = box1(), b2 = box2(), ch = tw_get(&coll_h, now) * coll_full(), co = tw_get(&coll_op, now), b3 = box3(ch);
    float tx = C2 + COLW - 20 - 296, ty = b3 + 12 + 61 + 32;
    char v[16];
    close_btn(H_CLOSE, PX, PY, now);
    gfx_xform_push(0, 0, 1, 0, VA);                                   /* ignition off - unavailable (to the end of the function) */
    reset_btn(H_RESET, PX + CW - reset_w(), PY + 2, now);
    toggle_draw_c(PX + COLW - 96, b1 + 20, tw_get(&auto_tw, now), C_ACCENT);
    toggle_draw_c(PX + COLW - 96, b1 + 81, tw_get(&drl_tw, now), C_ACCENT);
    snprintf(v, sizeof v, "%d", A.backlight);
    slider_row(PX + 20, bl_y(), COLW - 40, "Instrument backlight", v, tw_get(&bl_v, now) / 10, "gauge", C_TEXT);
    seg_plain(PX + COLW - 20 - 238, b2 + 19, 2, 116, 40, 3, tw_get(&lock_th, now), LOCK, 17, H_LOCK0, now);

    /* Driving: block height is animated (max-height .45s), speed threshold inside */
    rrect(C2, b1, COLW, 24 + 60 + ch, 20, C_INNER);
    row_text(C2 + 20, b1 + 42, COLW - 136, "Speed alarm", "Chime when you drive faster than the set speed");
    gfx_xform_push(D(C2 + COLW - 58), D(b1 + 42), 1, 0, RO_A);       /* read-only: gray + opacity */
    toggle_draw_c(C2 + COLW - 96, b1 + 20, tw_get(&spd_tw, now), C_SUB);
    gfx_xform_pop();
    if (ch > 0.5f && co > 0.005f) {
        float y = spd_y();
        clip_push(C2, b1 + 72, COLW, ch);
        gfx_xform_push(D(C2 + COLW * 0.5f), D(y), 1, 0, co * RO_A);   /* read-only - dim */
        rrect(C2 + 20, b1 + 72, COLW - 40, 1, 0, C_TRACK);
        snprintf(v, sizeof v, tr("%d km/h"), A.speed_limit);
        slider_row(C2 + 20, y, COLW - 40, "Alarm speed", v, (tw_get(&spd_v, now) - 30) / 100, "clock", C_SUB);
        gfx_xform_pop();
        clip_pop();
    }
    /* Instrument cluster */
    t_left(700, 19, C2, b3 - 10 - lh15() * 0.5f, "Instrument cluster", C_SUB);
    rrect(C2, b3, COLW, 149, 20, C_INNER);
    t_left(700, 21, C2 + 20, b3 + 42, "Language", C_TEXT);
    seg_plain(C2 + COLW - 20 - 378, b3 + 19, 3, 124, 40, 3, tw_get(&lang_th, now), LANG, 17, H_LANG0, now);
    rrect(C2 + 20, b3 + 72, COLW - 40, 1, 0, C_TRACK);
    t_left(700, 21, C2 + 20, ty, "Instrument time", C_TEXT);
    time_btn(H_HDN, tx, ty - 22, "chev_l_w24", now);
    snprintf(v, sizeof v, "%02d", A.vt_h);
    t_center(800, 24, tx + 68, ty, v, C_TEXT);
    time_btn(H_HUP, tx + 96, ty - 22, "chev_r_w24", now);
    t_center(800, 24, tx + 148, ty, ":", C_SUB);
    time_btn(H_MDN, tx + 160, ty - 22, "chev_l_w24", now);
    snprintf(v, sizeof v, "%02d", A.vt_m);
    t_center(800, 24, tx + 228, ty, v, C_TEXT);
    time_btn(H_MUP, tx + 256, ty - 22, "chev_r_w24", now);
    gfx_xform_pop();
}

static void state(double now)
{
    tw_set(&auto_tw, (float)A.auto_light, 400, &SPRING, now);
    tw_set(&drl_tw, (float)A.drl, 400, &SPRING, now);
    tw_set(&spd_tw, (float)A.speed_alarm, 400, &SPRING, now);
    tw_set(&lock_th, A.lock_lh ? 0 : 1, SPRING_MS, &SPRING_SOFT, now);
    tw_set(&lang_th, (float)A.lang, SPRING_MS, &SPRING_SOFT, now);
    tw_set(&coll_h, (float)A.speed_alarm, 450, &ENTER, now);
    tw_set(&coll_op, (float)A.speed_alarm, 300, &EASE, now);
    if (A.backlight != last_bl) { last_bl = A.backlight; tw_set(&bl_v, (float)A.backlight, 180, &EASE_OUT, now); }
    if (A.speed_limit != last_lim) { last_lim = A.speed_limit; tw_set(&spd_v, (float)A.speed_limit, 180, &EASE_OUT, now); }
}

static void update(double now)
{
    if (A.car_live != live) { live = A.car_live; screen_dirty(); damage_d(ST_X, ST_Y, ST_W, ST_H); }   /* ignition on/off */
    state(now);
    if (tw_active(&auto_tw, now) || tw_active(&drl_tw, now) || tw_active(&spd_tw, now) || tw_active(&lock_th, now)
        || tw_active(&lang_th, now) || tw_active(&coll_h, now) || tw_active(&coll_op, now) || tw_active(&bl_v, now)
        || tw_active(&spd_v, now) || press_active(now))
        damage_d(ST_X, ST_Y, ST_W, ST_H);
}

static void click(int id, double now)
{
    if (id == H_CLOSE) { close_screen(now); return; }
    if (!A.car_live) return;                                    /* ignition off - unavailable */
    if (id == H_RESET) {
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
        A.auto_light = 1; A.drl = 1; A.lock_lh = 1; A.speed_alarm = 0; A.speed_limit = 120; A.lang = 0; A.backlight = 6;
        A.vt_h = tm.tm_hour; A.vt_m = tm.tm_min;
        hud_show(5, NULL, now);
    } else if (id == H_AUTO) { A.auto_light = !A.auto_light; mcu_vehicle(0x04, A.auto_light ? 1 : 2); A.veh_touch = now; }
    else if (id == H_DRL) { A.drl = !A.drl; mcu_vehicle(0x05, A.drl ? 1 : 2); A.veh_touch = now; }
    else if (id >= H_LOCK0 && id < H_LOCK0 + 2) { A.lock_lh = id == H_LOCK0; mcu_vehicle(0x03, A.lock_lh ? 1 : 2); A.veh_touch = now; }
    else if (id >= H_LANG0 && id < H_LANG0 + 3) {               /* HU interface language + cluster (cluster: EN 2, RU 3; Ukr. -> RU) */
        A.lang = id - H_LANG0; mcu_vehicle(0x07, A.lang == 0 ? 2 : 3); A.veh_touch = now;
    } else if (id == H_HUP || id == H_HDN || id == H_MUP || id == H_MDN) {
        if (id == H_HUP) A.vt_h = (A.vt_h + 1) % 24;
        else if (id == H_HDN) A.vt_h = (A.vt_h + 23) % 24;
        else if (id == H_MUP) A.vt_m = (A.vt_m + 1) % 60;
        else A.vt_m = (A.vt_m + 59) % 60;
        mcu_cluster_time(A.vt_h, A.vt_m, A.backlight); A.veh_touch = now;
    }
    state(now);
    layout_again();
    damage_d(ST_X, ST_Y, ST_W, ST_H);
}

static void drag(int id, float x, float y, int phase, double now)
{
    (void)y;
    if (phase == 2 || !A.car_live) return;
    if (id == H_BL) {
        static int last = -1;
        A.backlight = (int)lroundf(slider_value(PX + 20, COLW - 40, x) * 10);
        if (A.backlight != last) { last = A.backlight; mcu_cluster_backlight(A.backlight); A.veh_touch = now; }
    }
    state(now);
}

static void enter(double now)
{
    time_t t = time(NULL); struct tm tm;
    localtime_r(&t, &tm); A.vt_h = tm.tm_hour; A.vt_m = tm.tm_min;   /* cluster time = system time (which we sync with the car) */
    last_bl = last_lim = -1; live = A.car_live;
    state(now);
    tw_init(&auto_tw, auto_tw.to); tw_init(&drl_tw, drl_tw.to); tw_init(&spd_tw, spd_tw.to);
    tw_init(&lock_th, lock_th.to); tw_init(&lang_th, lang_th.to); tw_init(&coll_h, coll_h.to); tw_init(&coll_op, coll_op.to);
    tw_init(&bl_v, bl_v.to); tw_init(&spd_v, spd_v.to);
}

const screen_def SCR_VEHICLE = { layout, draw_static, draw_dyn, update, click, enter, drag };
