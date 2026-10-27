/* Phone screen: dial keypad, Recents / Contacts lists (phone over Bluetooth: PBAP -> vCard, bt_nfore.c),
 * call screen (Calling… / timer, Mute / Speakers / End). Calls - HFP (bt.h). Design: Main.dc.html, is.phone. */
#include "ui_int.h"
#include "i18n.h"
#include "bt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

enum { H_LIST = HIT_SCREEN, H_K0, H_CALL = H_K0 + 12, H_DEL, H_REC, H_CON, H_MUTE, H_SPK, H_END, H_P0 };
#define SLOTS 16                   /* buttons of visible rows (circular) */

static const char *KEYS = "123456789*0#";
static const char *SUBS[12] = { "", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ", "", "+", "" };

static tween tab_th, del_op, call_op, call_k, mute_b, spk_b, ring;
static int last_el = -1, lp_done, slot_row[SLOTS], *by_name, by_cap, sorted_n = -1, last_sig = -1;
static scroller sc;
static float last_off = -1;

#define KX 147                     /* v26: padding 24 */
#define KEY_W ((390 - 24) / 3.0f)
#define CALLS_X 580
#define SEG_X (605 + 632 - 270)
#define LIST_X 605                 /* list: padding 24, after the header (58) and gap 14; scrolling */
#define LIST_Y 115
#define LIST_H 514
#define ROW_W 618
#define PITCH 74

static int call_shown(void) { return A.bt_call == 2 || A.bt_call == 3; }   /* outgoing or in conversation */
static int list_n(void) { return A.ph_contacts ? A.n_cont : A.n_rec; }

static int cmp_name(const void *a, const void *b) { return strcasecmp(PB[*(const int *)a].name, PB[*(const int *)b].name); }
static void sort_contacts(void)
{
    int i;
    int *g = grow(by_name, &by_cap, A.n_cont, sizeof *by_name);
    if (!g) return;
    by_name = g;
    for (i = 0; i < A.n_cont; i++) by_name[i] = i;
    if (A.n_cont) qsort(by_name, (size_t)A.n_cont, sizeof(int), cmp_name);
    sorted_n = A.n_cont;
}

/* call time as in the design: today - 12:40, yesterday - Yesterday, this week - Mon, earlier - 12 May */
static void when(char *b, size_t n, long long t)
{
    static const char *WD[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    struct tm c, now_tm;
    time_t tt, nt = time(NULL);
    long days;
    memset(&c, 0, sizeof c);
    c.tm_year = (int)(t / 100000000LL) - 1900; c.tm_mon = (int)(t / 1000000 % 100) - 1; c.tm_mday = (int)(t / 10000 % 100);
    c.tm_hour = (int)(t / 100 % 100); c.tm_min = (int)(t % 100); c.tm_isdst = -1;
    tt = mktime(&c);
    localtime_r(&nt, &now_tm);
    now_tm.tm_hour = now_tm.tm_min = now_tm.tm_sec = 0;
    days = (long)((mktime(&now_tm) - tt + 86399) / 86400);           /* 0 - today */
    if (days <= 0) snprintf(b, n, "%02d:%02d", c.tm_hour, c.tm_min);
    else if (days == 1) snprintf(b, n, "Yesterday");
    else if (days < 7) snprintf(b, n, "%s", WD[c.tm_wday]);
    else snprintf(b, n, "%d %s", c.tm_mday, tr(MON[(c.tm_mon + 12) % 12]));
}

/* list row r: name, caption, number to dial, missed */
static void row_info(int r, const char **name, char *sub, size_t sn, const char **num, int *missed)
{
    if (A.ph_contacts) {
        const pb_contact *c = &PB[by_name[r]];
        *name = c->name[0] ? c->name : c->num[0];
        snprintf(sub, sn, "%s", c->kind[0] == 'h' ? "Home" : c->kind[0] == 'w' ? "Work" : "Mobile");
        *num = c->num[0]; *missed = 0;
    } else {
        const pb_call *c = &REC[r];
        const char *nm = c->name[0] ? c->name : pb_name_of(c->num);
        char w[24];
        *name = nm ? nm : c->num;
        when(w, sizeof w, c->t);
        snprintf(sub, sn, "%s \xC2\xB7 %s", c->dir == 0 ? "Missed" : pb_kind_of(c->num), w);
        *num = c->num; *missed = c->dir == 0;
    }
}

static void call_hits(void);
static void layout(void)
{
    int i, r, first = (int)(sc.off / PITCH);
    clear_screen_hits();
    if (call_shown()) { call_hits(); return; }
    for (i = 0; i < 12; i++) set_hit(H_K0 + i, KX + (i % 3) * (KEY_W + 12), 121 + (i / 3) * 78, KEY_W, 66);
    set_hit(H_CALL, 303, 551, 78, 78);
    if (A.dial[0]) set_hit(H_DEL, 421, 562, 56, 56);
    set_hit(H_REC, SEG_X + 5, 48, 130, 48);
    set_hit(H_CON, SEG_X + 135, 48, 130, 48);
    set_scroll(H_LIST, LIST_X, LIST_Y, 636, LIST_H);
    for (i = 0; i < SLOTS; i++) slot_row[i] = -1;
    for (r = first; r < list_n() && r < first + SLOTS; r++) {   /* visible part of the row */
        float y0 = LIST_Y + r * PITCH - sc.off, y1 = y0 + 70;
        if (y0 < LIST_Y) y0 = LIST_Y;
        if (y1 > LIST_Y + LIST_H) y1 = LIST_Y + LIST_H;
        if (y1 <= y0) continue;
        slot_row[r % SLOTS] = r;
        set_hit(H_P0 + r % SLOTS, LIST_X, y0, ROW_W, y1 - y0);
    }
}

/* call screen: vertical column centered in the 1140x636 area */
#define CALL_TOP 128
static void call_hits(void)
{
    float by = CALL_TOP + 124 + 20 + 43.2f + t_lh(600, 23) + 36;
    clear_screen_hits();
    set_hit(H_MUTE, 692 - 39 - 114, by, 78, 78);
    set_hit(H_SPK, 692 - 39, by, 78, 78);
    set_hit(H_END, 692 - 39 + 114, by, 78, 78);
}

static void draw_static(void)
{
    panel(ST_X, ST_Y, 440, ST_H, PANEL_R);
    panel(CALLS_X, ST_Y, 682, ST_H, PANEL_R);
    screen_head(605, 72, 0x2DBE6C, "app_phone_w22", "Phone", C_TEXT);   /* v26 */
    seg_static(SEG_X, 43, 2, 130, 48);
}

static void draw_row(int r, double now)
{
    const char *name, *num;
    char sub[64], ini[8];
    int missed;
    float x = LIST_X, y = LIST_Y + r * PITCH - sc.off, w = ROW_W, h = 70, k;
    row_info(r, &name, sub, sizeof sub, &num, &missed);
    k = press_box(H_P0 + r % SLOTS, &x, &y, &w, &h, now);
    person_initial(ini, sizeof ini, name);
    rrect(x + 12 * k, y + 10 * k, 50 * k, 50 * k, 25 * k, hexc(person_color(name)));
    t_center(800, 23, x + 37 * k, y + h * 0.5f, ini, C_WHITE);
    {
        float l1 = t_lh(700, 22), l2 = t_lh(500, 18), ty = y + h * 0.5f - (l1 + 2 + l2) * 0.5f;
        t_fit(700, 22, x + (12 + 50 + 16) * k, ty + l1 * 0.5f, w - 12 - 50 - 16 - 12 - 46 - 16, name, missed ? hexc(0xFF453A) : C_TEXT);
        t_top(500, 18, x + (12 + 50 + 16) * k, ty + l1 + 2, sub, C_SUB, 0);
    }
    rrect(x + w - (12 + 46) * k, y + h * 0.5f - 23 * k, 46 * k, 46 * k, 23 * k, hexa(0x2DBE6C, 0.18f));
    ico("phone_w22", x + w - (12 + 23) * k, y + h * 0.5f, 20 * k, hexc(0x2DBE6C));
}

static void draw_dyn(double now)
{
    static const char *const TL[2] = { "Recents", "Contacts" };
    float k, co = tw_get(&call_op, now);
    int i, r, first = (int)(sc.off / PITCH);
    char d[2] = { 0, 0 };
    /* number */
    {
        font *f = F(700, 38);
        const char *s = A.dial[0] ? A.dial : tr("Enter number");
        float w = text_width(f, s, D(1.52f));
        text_draw(f, D(342) - w * 0.5f, D(75) - text_line_height(f) * 0.5f, s, A.dial[0] ? C_TEXT : C_MUTED, D(1.52f));
    }
    for (i = 0; i < 12; i++) {
        float x = KX + (i % 3) * (KEY_W + 12), y = 121 + (i / 3) * 78, w = KEY_W, h = 66;
        k = press_box(H_K0 + i, &x, &y, &w, &h, now);
        rrect(x, y, w, h, 33 * k, col_mix(C_INNER, hexa(0, C_INNER.a + 0.1f), press_dim(k) / PRESS_DIM));
        d[0] = KEYS[i];
        t_center(700, 27, x + w * 0.5f, y + h * 0.5f - 6, d, C_TEXT);
        if (SUBS[i][0]) {
            font *f = F(800, 14);
            float sw = text_width(f, SUBS[i], D(14 * 0.14f));
            text_draw(f, D(x + w * 0.5f) - sw * 0.5f, D(y + h * 0.5f + 14.2f) - text_line_height(f) * 0.5f, SUBS[i], C_MUTED, D(14 * 0.14f));
        }
    }
    {   /* call: 78, accent (v17), shadow 0 10px 30px accentGlow */
        float x = 303, y = 551, w = 78, h = 78;
        k = press_box(H_CALL, &x, &y, &w, &h, now);
        shadow(x, y, w, h, w * 0.5f, 30, 0, 10, C_ACC_GLOW);
        rrect(x, y, w, h, w * 0.5f, col_mix(C_ACCENT, hexc(0), press_dim(k)));
        ico("phone", x + w * 0.5f, y + h * 0.5f, 32 * k, C_ONACCENT);
    }
    {
        float o = tw_get(&del_op, now);
        k = press_k(H_DEL, now);
        if (o > 0.005f) ico("backspace", 449, 590, 30 * k, col_alpha(C_TEXT, o * (1 - press_dim(k))));
    }
    seg_dyn(SEG_X, 43, 2, 130, 48, tw_get(&tab_th, now), TL, NULL, 700, 20, 0, H_REC, now);
    clip_push(LIST_X, LIST_Y, 636, LIST_H);
    if (list_n() * PITCH - 4 > LIST_H) {   /* scrollbar-width: thin, color c.track */
        float th = LIST_H * LIST_H / (LIST_H + sc.max), ty = LIST_Y + (LIST_H - th) * (sc.max > 0 ? sc.off / sc.max : 0);
        rrect(LIST_X + 636 - 8, ty, 8, th, 4, C_TRACK);
    }
    for (r = first; r < list_n() && r < first + SLOTS; r++) {
        if (LIST_Y + r * PITCH - sc.off > LIST_Y + LIST_H) break;
        draw_row(r, now);
    }
    clip_pop();
    if (co > 0.005f) {      /* call screen: opacity .4s, scale 1.04 -> 1 (.55s cubic-bezier(.34,1.4,.64,1)) */
        float s = 1.04f - 0.04f * tw_get(&call_k, now), el = (float)(now - A.bt_call_t);
        float y = CALL_TOP, rg = tw_get(&ring, now) * 26;
        const char *nm = pb_name_of(A.bt_call_num);
        char st[16], ini[8];
        int j;
        static const char *LB[3] = { "Mute", "Speakers", "End" };
        if (!nm) nm = A.bt_call_num[0] ? A.bt_call_num : "Unknown";
        person_initial(ini, sizeof ini, nm);
        gfx_xform_push(D(692), D(336), s, 0, co);
        gfx_glass(D(ST_X), D(ST_Y), D(ST_W), D(ST_H), D(PANEL_R), HUD_TINT, HUD_A, GLASS_EDGE);
        if (rg > 0.5f) rrect(692 - 62 - rg, y - rg, 124 + rg * 2, 124 + rg * 2, 62 + rg, C_ACC_SOFT);
        rrect(692 - 62, y, 124, 124, 62, hexc(person_color(nm)));
        t_center(800, 46, 692, y + 62, ini, C_WHITE);
        y += 124 + 12 + 8;
        t_center(800, 36, 692, y + 21.6f, nm, C_TEXT);
        y += 43.2f + 12;
        if (A.bt_call != 3) snprintf(st, sizeof st, "Calling\xE2\x80\xA6");
        else snprintf(st, sizeof st, "%02d:%02d", (int)el / 60, (int)el % 60);
        t_center(600, 23, 692, y + t_lh(600, 23) * 0.5f, st, C_SUB);
        y += t_lh(600, 23) + 36;
        for (j = 0; j < 3; j++) {
            float bx = 692 - 39 + (j - 1) * 114, by = y, bw = 78, bh = 78, kk;
            float on = j == 0 ? tw_get(&mute_b, now) : j == 1 ? tw_get(&spk_b, now) : 0;
            col bg = j == 2 ? hexc(0xFF3B30) : col_mix(C_INNER, C_ACCENT, on);
            col fg = j == 2 ? C_WHITE : col_mix(C_TEXT, C_ONACCENT, on);
            kk = press_box(H_MUTE + j, &bx, &by, &bw, &bh, now);
            if (j == 2) shadow(bx, by, bw, bh, bw * 0.5f, 30, 0, 10, hexa(0xFF3B30, 0.45f));
            rrect(bx, by, bw, bh, bw * 0.5f, bg);
            if (j == 2) ico_rot("phone", bx + bw * 0.5f, by + bh * 0.5f, 32 * kk, fg, 135);
            else ico(j ? "speaker" : "mic_off", bx + bw * 0.5f, by + bh * 0.5f, 30 * kk, fg);
            t_center(600, 18, 692 - 39 + (j - 1) * 114 + 39, y + 78 + 8 + t_lh(600, 18) * 0.5f, LB[j], C_SUB);
        }
        gfx_xform_pop();
    }
}

static void list_changed(void)
{
    sc.max = list_n() * PITCH - 4 - LIST_H;
    if (sc.max < 0) sc.max = 0;
    if (sc.off > sc.max) sc.off = sc.max;
}

static void update(double now)
{
    int el = A.bt_call == 3 ? (int)(now - A.bt_call_t) : -1;
    int sig = A.n_cont * 7 + A.n_rec * 131 + A.ph_contacts * 99991 + call_shown() * 1000003;
    if (sig != last_sig) {                         /* contacts/call log arrived, tab or call changed */
        last_sig = sig;
        if (sorted_n != A.n_cont) sort_contacts();
        list_changed();
        tw_set(&call_op, (float)call_shown(), 400, &EASE, now);
        tw_set(&call_k, (float)call_shown(), 550, &SPRING_SOFT, now);
        layout_again();
        damage_d(ST_X, ST_Y, ST_W, ST_H);
    }
    if (tw_active(&tab_th, now)) damage_d(SEG_X, 44, 270, 58);
    sc_update(&sc, now);
    if (sc.off != last_off) { last_off = sc.off; damage_d(LIST_X, LIST_Y, 636, LIST_H); if (!call_shown()) layout(); }
    /* "0" hold 450 ms -> "+" (and the 0 itself is not added) */
    if (held_id() == H_K0 + 10 && !lp_done && now - held_since() >= 0.45) {
        size_t n = strlen(A.dial);
        lp_done = 1;
        if (n < 16) { A.dial[n] = '+'; A.dial[n + 1] = 0; }
        cancel_press(now);
        tw_set(&del_op, 1, 250, &EASE, now);
        layout();
        damage_d(ST_X, ST_Y, 440, 120);
    }
    if (held_id() < 0) lp_done = 0;
    if (tw_active(&del_op, now)) damage_d(415, 555, 70, 70);
    tw_set(&mute_b, (float)A.mute, 300, &EASE, now);
    tw_set(&spk_b, (float)A.bt_sco, 300, &EASE, now);       /* Speakers - voice goes through the car */
    if (tw_active(&call_op, now) || tw_active(&call_k, now) || tw_active(&ring, now) || tw_active(&mute_b, now) || tw_active(&spk_b, now))
        damage_d(ST_X, ST_Y, ST_W, ST_H);
    if (call_shown()) {
        /* calling: ring pulses every 3 ticks (transition .9s); conversation - timer */
        if (A.bt_call == 2) {
            int ph = (int)(now * 1000 / (TICK_MS * 3)) % 2;
            tw_set(&ring, (float)ph, 900, &EASE, now);
        } else tw_set(&ring, 0, 900, &EASE, now);
        if (el != last_el) { last_el = el; damage_d(500, 300, 380, 60); }
    }
}

static void dial(const char *num)
{
    if (!num || !num[0]) return;
    A.mute = 0;
    snprintf(A.bt_call_num, sizeof A.bt_call_num, "%s", num);
    bt_dial(num);
}

static void click(int id, double now)
{
    size_t n = strlen(A.dial);
    if (id >= H_K0 && id < H_K0 + 12) { if (n < 16) { A.dial[n] = KEYS[id - H_K0]; A.dial[n + 1] = 0; } }
    else if (id == H_DEL) { if (n) A.dial[n - 1] = 0; }
    else if (id == H_CALL || (id >= H_P0 && id < H_P0 + SLOTS && slot_row[id - H_P0] >= 0)) {
        if (!music_bt_av()) { hud_show(7, "No phone connected", now); return; }   /* design: hudKind 'nope' */
        if (id == H_CALL) { if (n) dial(A.dial); }
        else {
            const char *name, *num;
            char sub[64];
            int missed;
            row_info(slot_row[id - H_P0], &name, sub, sizeof sub, &num, &missed);
            dial(num);
        }
        return;
    } else if (id == H_REC || id == H_CON) {
        A.ph_contacts = id == H_CON;
        tw_set(&tab_th, (float)A.ph_contacts, SPRING_MS, &SPRING_SOFT, now);
        sc.off = 0;
        damage_d(590, 100, 660, 540);
    }
    else if (id == H_MUTE) { A.mute = !A.mute; bt_mic_mute(A.mute); }
    else if (id == H_SPK) bt_audio_car(!A.bt_sco);
    else if (id == H_END) { bt_hangup(); A.dial[0] = 0; return; }
    tw_set(&del_op, A.dial[0] ? 1 : 0, 250, &EASE, now);
    layout();
    damage_d(ST_X, ST_Y, 440, 120);
}

static void enter(double now)
{
    (void)now;
    tw_init(&tab_th, (float)A.ph_contacts);
    tw_init(&del_op, A.dial[0] ? 1 : 0);
    tw_init(&call_op, (float)call_shown()); tw_init(&call_k, (float)call_shown());
    tw_init(&mute_b, (float)A.mute); tw_init(&spk_b, (float)A.bt_sco); tw_init(&ring, 0);
    if (sorted_n != A.n_cont) sort_contacts();
    list_changed();
    sc.v = 0; sc.drag = 0;
    last_off = -1; last_sig = -1;
    if (call_shown()) call_hits(); else layout();
}

static void drag(int id, float x, float y, int phase, double now)
{
    (void)x;
    if (id == H_LIST) sc_drag(&sc, y, phase, now);
}

const screen_def SCR_PHONE = { layout, draw_static, draw_dyn, update, click, enter, drag };
