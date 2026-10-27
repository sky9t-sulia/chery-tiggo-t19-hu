/* Radio screen (design v8-v10, is.radio + radioVals): region, FM/AM and power, 120px frequency with state (seek/scan)
 * and a heart, scale with an arrow, favorites under the scale (up to 4 tiles, if >4 - 3 + "more"); on the right tabs
 * Stations / Favorites, scanning. Data and commands - MCU (NOTES_mcu.md): A.fav/A.found/A.scanning/A.seeking
 * are filled by the core (mcu_sync). */
#include "ui_int.h"
#include "i18n.h"
#include "mcu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { H_SHADE = HIT_SCREEN, H_FM, H_AM, H_POWER, H_DOWN, H_UP, H_STOP, H_HEART, H_TUNE, H_SCAN, H_CANCEL, H_EMPTY_SCAN,
       H_ON, H_TAB_ST, H_TAB_FAV, H_MORE, H_LIST, H_FAV0, H_ROW0 = H_FAV0 + 4, H_ACT0 = H_ROW0 + 12,
       H_REG = H_ACT0 + 12, H_REG0 };
#define SLOTS 12                  /* buttons of visible list rows (circular) */

typedef struct { float lo, hi, step; const char *unit; } band_t;
/* regions (design v10 regions) - only those present in the MCU (0x36 [code]), ranges and step as in the stock unit
 * (DEVIATIONS: the design also has Japan and Russia, and some ranges differ). fm_div - MCU FM units per 1 MHz */
typedef struct { int code; const char *name; band_t b[2]; int fm_div; } region_t;
#define NREG 4
static const region_t REG[NREG] = {
    { 1, "Europe",        { { 87.5f, 108, 0.05f, "MHz" }, { 531, 1629, 9, "kHz" } }, 20 },
    { 2, "USA",           { { 87.5f, 107.9f, 0.2f, "MHz" }, { 530, 1710, 10, "kHz" } }, 10 },
    { 3, "Latin America", { { 87.5f, 108, 0.1f, "MHz" }, { 530, 1710, 10, "kHz" } }, 10 },
    { 0, "Asia",          { { 87.5f, 108, 0.1f, "MHz" }, { 531, 1629, 9, "kHz" } }, 10 },
};
static const region_t *reg(void) { int i; for (i = 0; i < NREG; i++) if (REG[i].code == A.region) return &REG[i]; return &REG[0]; }
#define BANDS (reg()->b)
int radio_fm_div(void) { return reg()->fm_div; }

static tween band_th, needle, pwr, tab_th;
static scroller sc_l;
static float last_f = -1, last_lo = -1;
static int seek_held, slot_row[SLOTS], last_sig = -1, fav_tab, reg_open;
static tween reg_tw;

#define X0 PX0                    /* v26: padding 24 */
#define Y0 PY0
#define CW PCW
#define PWR_X (X0 + CW - 58)
#define SEG_X (PWR_X - 12 - 210)
#define ROW_Y (Y0 + 58 + 18)      /* header 58, gap 18 */
#define ROW_H (PBOT - ROW_Y)
#define LW (CW - 24 - 400)        /* left column */
#define CX (X0 + LW * 0.5f)
#define LX (X0 + CW - 400)        /* right panel 400 */
#define LWID 400.0f
#define FAV_W 136.0f
#define HDR_H 72.0f               /* panel header: padding 12, tabs 48 */

static float *cur(void) { return A.am ? &A.am_f : &A.fm_f; }
static int bi(void) { return A.am; }
/* design v10: FM with two decimals if the region step is below 0.1 (Europe: 101.50) */
static void fmt_band(char *b, size_t n, const band_t *bd, float v)
{
    if (bd->step >= 1) snprintf(b, n, "%d", (int)lroundf(v));
    else snprintf(b, n, bd->step < 0.1f ? "%.2f" : "%.1f", v);
}
void radio_fmt(char *b, size_t n, float v) { fmt_band(b, n, &BANDS[A.am], v); }
#define fmt_f radio_fmt
static float shown_f(void) { return A.scanning ? A.scan_f : *cur(); }
static int same(float a, float b) { return fabsf(a - b) < 0.01f; }
static int fav_index(float f) { int i; for (i = 0; i < A.nfav[bi()]; i++) if (same(A.fav[bi()][i], f)) return i; return -1; }
static int ftab(void) { return fav_tab && !A.scanning; }

static float frac_of(float f);

/* no RDS names yet (FM isn't received on the bench) - the design for this case: frequency only */
static const char *rds_name(float f) { (void)f; return ""; }

/* caption on Dashboard (v9 dashSub): off -> "Radio off", else RDS name, else "Favorite" / "FM radio" */
const char *station_name(char *b, size_t n)
{
    const char *r = rds_name(*cur());
    if (!A.radio_on) snprintf(b, n, "Radio off");
    else if (r[0]) snprintf(b, n, "%s", r);
    else if (fav_index(*cur()) >= 0) snprintf(b, n, "Favorite");
    else snprintf(b, n, tr("%s radio"), A.am ? "AM" : "FM");
    return b;
}

/* ---- shared with Dashboard */
float radio_frac(void) { return frac_of(shown_f()); }
int radio_is_fav(void) { return fav_index(*cur()) >= 0; }

void radio_fav_toggle(double now)
{
    if (A.scanning) return;
    if (fav_index(*cur()) < 0 && A.nfav[bi()] >= 20) { hud_show(10, "Full \xC2\xB7 20 of 20", now); return; }
    if (fav_index(*cur()) >= 0) { hud_show(10, "Removed", now); mcu_fav_remove(A.am, fav_index(*cur()) + 1); }
    else { hud_show(10, "Added", now); mcu_fav_toggle(); }
}

/* previous/next (v9 rJump): through favorites; no favorites - through found; none of those - seek */
void radio_jump(int d)
{
    int b = bi(), i, n = A.nfav[b];
    if (A.scanning || !A.radio_on) return;
    if (n) {
        i = fav_index(*cur());
        i = i >= 0 ? (i + d + n) % n : d > 0 ? 0 : n - 1;
        mcu_pick_fav(i + 1);
    } else if (A.nfound[b]) {
        n = A.nfound[b];
        for (i = 0; i < n && !same(A.found[b][i], *cur()); i++) {}
        if (i < n) i = (i + d + n) % n;
        else if (d > 0) { for (i = 0; i < n && A.found[b][i] <= *cur(); i++) {} if (i >= n) i = 0; }
        else { for (i = n - 1; i >= 0 && A.found[b][i] >= *cur(); i--) {} if (i < 0) i = n - 1; }
        mcu_pick_found(i + 1);
    } else { A.seek_dir = d; mcu_seek(d > 0); }
}

/* ---- left column geometry (flex) */
static float bottom_h(void) { return 44 + 8 + t_lh(600, 18) + 20 + t_lh(700, 19) + 10 + 88; }
static float top_h(void) { return ROW_H - bottom_h(); }
static float cb_y(void) { return ROW_Y + (top_h() - 232) * 0.5f; }          /* status 44 + frequency 120 + 8 + 60 */
static float arrow_cy(void) { return ROW_Y + top_h() * 0.5f; }
static float tick_y(void) { return ROW_Y + top_h(); }
static float fav_hdr_y(void) { return tick_y() + 44 + 8 + t_lh(600, 18) + 20; }
static float fav_y(void) { return fav_hdr_y() + t_lh(700, 19) + 10; }
static float frac_of(float f) { const band_t *b = &BANDS[bi()]; return (f - b->lo) / (b->hi - b->lo); }
static int fav_vis(void) { return A.nfav[bi()] > 4 ? 3 : A.nfav[bi()]; }            /* favorite tiles under the scale */

/* ---- right panel */
static float list_top(void) { return ROW_Y + HDR_H + (A.scanning ? 2 + 8 + 8 + t_lh(600, 17) + 14 : 0); }
static float list_h(void) { return ROW_Y + ROW_H - list_top(); }
static float scan_w(void) { return 12 + 22 + 8 + t_width(700, 18, "Scan") + 16; }
static float cancel_w(void) { return 18 + t_width(700, 18, "Cancel") + 18; }
static int list_n(void) { return ftab() ? A.nfav[bi()] : A.nfound[bi()]; }
static float list_f(int r) { return ftab() ? A.fav[bi()][r] : A.found[bi()][r]; }
static float row_w(void) { return list_n() * 66 - 2 + 10 > list_h() ? 380 - 8 : 380; }

static const char *status_text(char *b, size_t n)
{
    if (A.scanning) snprintf(b, n, tr("Scanning %s"), A.am ? "AM" : "FM");
    else if (A.seeking) snprintf(b, n, "%s", A.seek_dir > 0 ? "Seeking up" : "Seeking down");
    else b[0] = 0;
    return b;
}
static float stop_w(void) { return 14 + t_width(800, 17, "Stop") + 14; }
static float chip_w(const char *t) { return 16 + 10 + 10 + t_width(700, 19, t) + 10 + (A.seeking ? stop_w() : 8) + 6; }

/* right panel empty states: 0 - "Find stations", 1 - "No stations found", 2 - "No favorites yet" */
static const char *empty_text(int kind, char *b, size_t n)
{
    if (kind == 0) snprintf(b, n, tr("Scan sweeps the whole %s band (about 20 s) and lists up to 40 stations."), A.am ? "AM" : "FM");
    else if (kind == 1) snprintf(b, n, "Check the antenna, or try again somewhere with better reception.");
    else snprintf(b, n, "Tap the heart next to a station to save it here. Up to 20 per band.");
    return b;
}
static int wrap_lines(const char *t, int w, float size, float maxw)
{
    char line[256];
    const char *p = t;
    int lines = 0;
    while (*p) {
        size_t n = 0, last = 0;
        while (p[n]) {
            memcpy(line, p, n + 1); line[n + 1] = 0;
            if (p[n] == ' ') { line[n] = 0; if (t_width(w, size, line) > maxw) break; last = n; }
            n++;
        }
        if (!p[n]) { memcpy(line, p, n); line[n] = 0; if (t_width(w, size, line) <= maxw || !last) last = n; }
        if (!last) last = n;
        lines++; p += last; while (*p == ' ') p++;
    }
    return lines;
}
static float empty_h(int kind)
{
    char t[160];
    return 64 + 10 + t_lh(700, 22) + 10 + wrap_lines(empty_text(kind, t, sizeof t), 600, 18, 332) * t_lh(600, 18)
           + (kind < 2 ? 10 + 6 + 52 : 0);
}
static float empty_btn_w(int kind)
{
    char t[24];
    snprintf(t, sizeof t, kind == 0 ? tr("Scan %s") : "Scan again", A.am ? "AM" : "FM");
    return 26 + t_width(800, 20, t) + 26;
}
static int empty_kind(void)          /* -1 - there is a list */
{
    int b = bi();
    if (A.scanning) return -1;
    if (ftab()) return A.nfav[b] ? -1 : 2;
    if (!A.found_known[b]) return 0;
    return A.nfound[b] ? -1 : 1;
}

/* ---- region: button in the header (left of FM/AM), 420 menu below it */
static float reg_w(void) { return 18 + 22 + 10 + t_width(700, 19, reg()->name) + 10 + 20 + 16; }
#define REG_X (SEG_X - 12 - reg_w())
static float pop_hdr_h(void) { return 8 + t_lh(800, 21) + 2 + t_lh(600, 17) + 10; }
static float pop_h(void) { return 10 + pop_hdr_h() + NREG * 62 + 10; }
#define POP_W 420.0f
#define POP_Y (Y0 + 68)
static float pop_x(void) { return REG_X + reg_w() - POP_W; }

static void layout(void)
{
    int i, b = bi(), ek;
    if (reg_open) {               /* region menu open: panel dimming closes it, header is above it */
        set_hit(H_SHADE, ST_X, ST_Y, ST_W, ST_H);
        set_hit(H_FM, SEG_X + 5, Y0 + 5, 100, 48);
        set_hit(H_AM, SEG_X + 105, Y0 + 5, 100, 48);
        set_hit(H_POWER, PWR_X, Y0, 58, 58);
        set_hit(H_REG, REG_X, Y0, reg_w(), 58);
        for (i = 0; i < NREG; i++) set_hit(H_REG0 + i, pop_x() + 10, POP_Y + 10 + pop_hdr_h() + i * 62, POP_W - 20, 62);
        return;
    }
    set_hit(H_REG, REG_X, Y0, reg_w(), 58);
    set_hit(H_FM, SEG_X + 5, Y0 + 5, 100, 48);
    set_hit(H_AM, SEG_X + 105, Y0 + 5, 100, 48);
    set_hit(H_POWER, PWR_X, Y0, 58, 58);
    if (!A.radio_on) { set_hit(H_ON, X0 + CW * 0.5f - 66, ROW_Y + ROW_H * 0.5f - 66 - 40, 132, 132); return; }
    set_hit(H_DOWN, X0, arrow_cy() - 42, 84, 84);
    set_hit(H_UP, X0 + LW - 84, arrow_cy() - 42, 84, 84);
    if (A.seeking) {
        char t[32];
        set_hit(H_STOP, CX + chip_w(status_text(t, sizeof t)) * 0.5f - 6 - stop_w(), cb_y() + 7, stop_w(), 30);
    }
    if (!A.scanning) {
        const char *n = rds_name(*cur());
        float w = n[0] ? fminf(t_width(700, 26, n), LW - 2 * 84 - 24 - 14 - 56) : 0, total = w + (n[0] ? 14 : 0) + 56;
        set_hit(H_HEART, CX - total * 0.5f + total - 56, cb_y() + 44 + 120 + 8 + 2, 56, 56);
        set_drag(H_TUNE, X0 - 10, tick_y() - 10, LW + 20, 64);
    }
    for (i = 0; i < fav_vis(); i++) set_hit(H_FAV0 + i, X0 + i * (FAV_W + 12), fav_y(), FAV_W, 88);
    if (A.nfav[b] > 4) set_hit(H_MORE, X0 + 3 * (FAV_W + 12), fav_y(), LW - 3 * (FAV_W + 12), 88);
    /* right panel */
    set_hit(H_TAB_ST, LX + 12 + 3, ROW_Y + 12 + 3, 124, 42);
    set_hit(H_TAB_FAV, LX + 12 + 127, ROW_Y + 12 + 3, 124, 42);
    if (A.scanning) set_hit(H_CANCEL, LX + LWID - 12 - cancel_w(), ROW_Y + 13, cancel_w(), 46);
    else if (A.found_known[b] && !ftab()) set_hit(H_SCAN, LX + LWID - 12 - scan_w(), ROW_Y + 13, scan_w(), 46);
    ek = empty_kind();
    if (ek == 0 || ek == 1) {
        float h = empty_h(ek), y = list_top() + (list_h() - h) * 0.5f, w = empty_btn_w(ek);
        set_hit(H_EMPTY_SCAN, LX + LWID * 0.5f - w * 0.5f, y + h - 52, w, 52);
    } else if (ek < 0 && !A.scanning) {
        int first = (int)(sc_l.off / 66), r;
        set_scroll(H_LIST, LX, list_top(), LWID, list_h());
        for (r = 0; r < SLOTS; r++) slot_row[r] = -1;
        for (r = first; r < list_n() && r < first + SLOTS; r++) {
            float y0 = list_top() + r * 66 - sc_l.off, y1 = y0 + 64;
            if (y0 < list_top()) y0 = list_top();
            if (y1 > list_top() + list_h()) y1 = list_top() + list_h();
            if (y1 <= y0) continue;
            slot_row[r % SLOTS] = r;
            set_hit(H_ROW0 + r % SLOTS, LX + 10, y0, row_w() - 6 - 52 - 6, y1 - y0);
            set_hit(H_ACT0 + r % SLOTS, LX + 10 + row_w() - 6 - 52, fmaxf(y0, list_top() + r * 66 - sc_l.off + 6), 52, 52);
        }
    }
}

static void head_title(void)
{
    screen_head(X0, Y0 + 29, 0xFF8A2A, "app_radio_w22", "Radio", C_TEXT);   /* v26 */
}

static void draw_static(void)
{
    panel(ST_X, ST_Y, ST_W, ST_H, PANEL_R);
    head_title();
}

static void heart_ico(int filled, float cx, float cy, float s, col c) { ico(filled ? "heart" : "heart_o", cx, cy, s, c); }

/* frequency + unit on the baseline; x - left edge */
static void freq_unit(float x, float cy, float f, int fw_, float fsz, int uw_, float usz, col c, float uop, float gap)
{
    char t[16];
    font *f1 = F(fw_, fsz), *f2 = F(uw_, usz);
    float w, fy, base;
    fmt_f(t, sizeof t, f);
    w = text_width(f1, t, 0);
    fy = D(cy) - text_line_height(f1) * 0.5f;
    base = fy + text_ascent(f1);
    text_draw(f1, D(x), fy, t, c, 0);
    text_draw(f2, D(x) + w + D(gap), base - text_ascent(f2), tr(BANDS[bi()].unit), col_alpha(c, uop), 0);
}

static void draw_left(double now)
{
    const band_t *bd = &BANDS[bi()];
    char f[16], t[64], lo[16], hi[16];
    float cb = cb_y(), k, sf = shown_f();
    int i, b = bi();
    col orange = C_ACCENT, chip = C_ACCENT;                     /* v17: accent */
    for (i = 0; i < 2; i++) {      /* arrows: 0.35 while scanning; ring on the arrow of the current seek */
        int id = i ? H_UP : H_DOWN;
        float x = i ? X0 + LW - 84 : X0, y = arrow_cy() - 42, w = 84, h = 84, op = A.scanning ? 0.35f : 1;
        k = press_box(id, &x, &y, &w, &h, now);
        if (A.seeking && A.seek_dir == (i ? 1 : -1)) {
            shadow(x, y, w, h, 42 * k, 22, 0, 0, C_ACC_GLOW);
            rrect(x - 3, y - 3, w + 6, h + 6, 45 * k, orange);
        }
        rrect(x, y, w, h, 42 * k, col_alpha(C_INNER, op));
        ico(i ? "chev_r_w22" : "chev_l_w22", x + w * 0.5f, y + h * 0.5f, 34 * k, col_alpha(C_TEXT, op * (1 - press_dim(k))));
    }
    if (A.scanning || A.seeking) {  /* status: "Scanning FM" / "Seeking up" + Stop */
        float w = chip_w(status_text(t, sizeof t)), x = CX - w * 0.5f, y = cb + 2;
        float pulse = (int)(now * 4) % 2 ? 1 : 0.35f;
        rrect(x, y, w, 40, 20, C_ACC_SOFT);
        rrect(x + 16, y + 15, 10, 10, 5, col_alpha(C_ACCENT, pulse));
        t_left(700, 19, x + 36, y + 20, t, chip);
        if (A.seeking) {
            float sw = stop_w(), bx = x + w - 6 - sw, by = y + 5, bh = 30, kk = press_box(H_STOP, &bx, &by, &sw, &bh, now);
            rrect(bx, by, sw, bh, 15 * kk, col_mix(orange, hexc(0), press_dim(kk)));
            t_center(800, 17, bx + sw * 0.5f, by + bh * 0.5f, "Stop", C_WHITE);
        }
    }
    {   /* frequency 120 + unit 26 */
        font *ff = F(800, 120), *fu = F(700, 26);
        float fw, uw, x0, fy, base;
        fmt_f(f, sizeof f, sf);
        fw = text_width(ff, f, D(-5.4f)); uw = text_width(fu, tr(bd->unit), 0);
        x0 = D(CX) - (fw + D(14) + uw) * 0.5f;
        fy = D(cb + 44) + (D(120) - text_line_height(ff)) * 0.5f;
        base = fy + text_ascent(ff);
        text_draw(ff, x0, fy, f, C_TEXT, D(-5.4f));
        text_draw(fu, x0 + fw + D(14), base - text_ascent(fu), tr(bd->unit), C_SUB, 0);
    }
    if (!A.scanning) {  /* name + heart */
        const char *n = rds_name(*cur());
        float y = cb + 44 + 120 + 8, w = n[0] ? fminf(t_width(700, 26, n), LW - 2 * 84 - 24 - 14 - 56) : 0;
        float total = w + (n[0] ? 14 : 0) + 56, nx = CX - total * 0.5f, x = nx + total - 56, yy = y + 2, hw = 56, hh = 56;
        int fav = fav_index(*cur()) >= 0;
        if (n[0]) t_fit(700, 26, nx, y + 30, w, n, C_SUB);
        k = press_box(H_HEART, &x, &yy, &hw, &hh, now);
        rrect(x, yy, hw, hh, 28 * k, C_INNER);
        heart_ico(fav, x + hw * 0.5f, yy + hh * 0.5f, 28 * k, fav ? C_ACCENT : C_TEXT);
    }
    {   /* scale: 81 ticks, arrow */
        float ty = tick_y(), nxp, ly = ty + 44 + 8 + t_lh(600, 18) * 0.5f;
        for (i = 0; i <= 80; i++) {
            float h = i % 10 == 0 ? 40 : i % 5 == 0 ? 28 : 16;
            rrect(X0 + i * (LW - 2) / 80, ty + 44 - h, 2, h, 1, C_TICK);
        }
        nxp = X0 + tw_get(&needle, now) * LW;
        shadow(nxp - 2, ty - 4, 4, 48, 2, 14, 0, 0, C_ACC_GLOW);
        rrect(nxp - 2, ty - 4, 4, 48, 2, orange);
        fmt_f(lo, sizeof lo, bd->lo); fmt_f(hi, sizeof hi, bd->hi);
        t_left(600, 18, X0, ly, lo, C_SUB);
        snprintf(t, sizeof t, "%d", (int)lroundf((bd->lo + bd->hi) * 0.5f));      /* mid: middle of the range */
        t_center(600, 18, CX, ly, t, C_SUB);
        t_right(600, 18, X0 + LW, ly, hi, C_SUB);
    }
    {   /* favorites: up to 4 tiles; more - 3 + "more" */
        float hy = fav_hdr_y() + t_lh(700, 19) * 0.5f, fy = fav_y();
        int many = A.nfav[b] > 4, hidden_cur = 0;
        snprintf(t, sizeof t, tr("Favorites \xC2\xB7 %s"), A.am ? "AM" : "FM");
        t_left(700, 19, X0, hy, t, C_SUB);
        snprintf(t, sizeof t, "%d / 20", A.nfav[b]);
        t_right(600, 18, X0 + LW, hy, t, C_MUTED);
        if (!A.nfav[b]) {
            float lh1 = t_lh(700, 21), lh2 = t_lh(600, 18), ty = fy + 44 - (lh1 + 2 + lh2) * 0.5f;
            dash_rect(X0, fy, LW, 88, 20, C_BORDER);
            heart_ico(0, X0 + 22 + 14, fy + 44, 28, C_SUB);
            t_top(700, 21, X0 + 22 + 28 + 16, ty, "No favorites yet", C_TEXT, 0);
            t_top(600, 18, X0 + 22 + 28 + 16, ty + lh1 + 2, "Tap the heart to save the station you\xE2\x80\x99re on", C_SUB, 0);
            return;
        }
        for (i = 0; i < fav_vis(); i++) {
            float x = X0 + i * (FAV_W + 12), y = fy, w = FAV_W, h = 88;
            int sel = same(A.fav[b][i], *cur()) && !A.scanning;
            k = press_box(H_FAV0 + i, &x, &y, &w, &h, now);
            rrect_b(x, y, w, h, 20 * k, sel ? C_ACCENT : C_INNER, C_BORDER);
            freq_unit(x + 14 * k, y + h * 0.5f, A.fav[b][i], 800, 27, 700, 15, sel ? C_ONACCENT : C_TEXT, 0.7f, 5);
        }
        for (i = 3; many && i < A.nfav[b]; i++) if (same(A.fav[b][i], *cur()) && !A.scanning) hidden_cur = 1;
        if (many) {   /* "+N / All favorites" */
            float x = X0 + 3 * (FAV_W + 12), y = fy, w = LW - 3 * (FAV_W + 12), h = 88;
            col fg = hidden_cur ? C_ONACCENT : C_TEXT;
            float l1 = t_lh(800, 27), l2 = t_lh(700, 17), ty;
            k = press_box(H_MORE, &x, &y, &w, &h, now);
            rrect_b(x, y, w, h, 20 * k, hidden_cur ? C_ACCENT : C_INNER, C_BORDER);
            ty = y + h * 0.5f - (l1 + 2 + l2) * 0.5f;
            snprintf(t, sizeof t, "+%d", A.nfav[b] - 3);
            t_top(800, 27, x + 18, ty, t, fg, 0);
            t_top(700, 17, x + 18, ty + l1 + 2, hidden_cur ? "Playing \xC2\xB7 all favorites" : "All favorites", col_alpha(fg, 0.8f), 0);
            ico("chev_r_w24", x + w - 16 - 12, y + h * 0.5f, 24, fg);
        } else if (A.nfav[b] <= 2) {   /* few favorites - hint */
            float x = X0 + A.nfav[b] * (FAV_W + 12), w = fmaxf(220, LW - A.nfav[b] * (FAV_W + 12));
            dash_rect(x, fy, w, 88, 20, C_BORDER);
            heart_ico(0, x + 20 + 12, fy + 44, 24, C_SUB);
            t_left(600, 18, x + 20 + 24 + 14, fy + 44, "Tap the heart to add more", C_SUB);
        }
    }
}

static void empty_state(int kind, double now)
{
    char t[160], bt[24];
    float h = empty_h(kind), y = list_top() + (list_h() - h) * 0.5f, cx = LX + LWID * 0.5f;
    static const char *TITLE[3] = { "Find stations", "No stations found", "No favorites yet" };
    rrect(cx - 32, y, 64, 64, 32, kind == 1 ? C_SEG : C_ACC_SOFT);
    if (kind == 2) heart_ico(0, cx, y + 32, 30, C_ACCENT);
    else ico(kind == 0 ? "waves" : "nosignal", cx, y + 32, 30, kind == 0 ? C_ACCENT : C_SUB);
    y += 64 + 10;
    t_center(700, 22, cx, y + t_lh(700, 22) * 0.5f, TITLE[kind], C_TEXT);
    y += t_lh(700, 22) + 10;
    y += t_wrap_center(600, 18, cx, y, 332, empty_text(kind, t, sizeof t), C_SUB) + 10 + 6;
    if (kind < 2) {
        float w = empty_btn_w(kind), x = cx - w * 0.5f, bh = 52, k = press_box(H_EMPTY_SCAN, &x, &y, &w, &bh, now);
        snprintf(bt, sizeof bt, kind == 0 ? tr("Scan %s") : "Scan again", A.am ? "AM" : "FM");
        rrect(x, y, w, bh, 26 * k, kind == 0 ? C_ACCENT : C_SEG);
        t_center(800, 20, x + w * 0.5f, y + bh * 0.5f, bt, kind == 0 ? C_ONACCENT : C_TEXT);
    }
}

static void draw_right(double now)
{
    static char lab[2][24];
    const char *TL[2] = { lab[0], lab[1] };
    char t[48], f[16];
    int b = bi(), r, ek;
    float k;
    rrect(LX, ROW_Y, LWID, ROW_H, 20, C_INNER);
    /* tabs Stations N / Favorites N (pad 3, no border) */
    if (A.nfound[b]) snprintf(lab[0], sizeof lab[0], tr("Stations %d"), A.nfound[b]); else snprintf(lab[0], sizeof lab[0], "Stations");
    if (A.nfav[b]) snprintf(lab[1], sizeof lab[1], tr("Favorites %d"), A.nfav[b]); else snprintf(lab[1], sizeof lab[1], "Favorites");
    seg_plain(LX + 12, ROW_Y + 12, 2, 124, 42, 3, tw_get(&tab_th, now), TL, 18, H_TAB_ST, now);
    if (A.scanning || (A.found_known[b] && !ftab())) {
        int sc = A.scanning;
        float w = sc ? cancel_w() : scan_w(), x = LX + LWID - 12 - w, y = ROW_Y + 13, h = 46;
        k = press_box(sc ? H_CANCEL : H_SCAN, &x, &y, &w, &h, now);
        rrect(x, y, w, h, 23 * k, sc ? C_ACCENT : C_SEG);
        if (sc) t_center(700, 18, x + w * 0.5f, y + h * 0.5f, "Cancel", C_ONACCENT);
        else {
            ico("scan", x + (12 + 11) * k, y + h * 0.5f, 22 * k, C_TEXT);
            t_left(700, 18, x + (12 + 22 + 8) * k, y + h * 0.5f, "Scan", C_TEXT);
        }
    }
    if (A.scanning) {   /* progress: frequency on the left, "Found N · X%" on the right */
        const band_t *bd = &BANDS[b];
        float py = ROW_Y + HDR_H + 2, pct = fminf(1, fmaxf(0, (A.scan_f - bd->lo) / (bd->hi - bd->lo)));
        rrect(LX + 22, py, LWID - 44, 8, 4, C_TRACK);
        if (pct > 0) rrect(LX + 22, py, (LWID - 44) * pct, 8, 4, C_ACCENT);
        fmt_f(f, sizeof f, A.scan_f);
        snprintf(t, sizeof t, "%s %s", f, bd->unit);
        t_left(600, 17, LX + 22, py + 16 + t_lh(600, 17) * 0.5f, t, C_SUB);
        snprintf(t, sizeof t, tr("Found %d \xC2\xB7 %d%%"), A.scan_n, (int)lroundf(pct * 100));
        t_right(600, 17, LX + LWID - 22, py + 16 + t_lh(600, 17) * 0.5f, t, C_SUB);
        return;
    }
    ek = empty_kind();
    if (ek >= 0) { empty_state(ek, now); return; }
    clip_push(LX, list_top(), LWID, list_h());
    {
        int first = (int)(sc_l.off / 66);
        float rw = row_w();
        col orange = C_ACCENT;
        for (r = first; r < list_n() && r < first + SLOTS; r++) {
            float x = LX + 10, y = list_top() + r * 66 - sc_l.off, w = rw, h = 64;
            int sel = same(list_f(r), *cur());
            const char *n = rds_name(list_f(r));
            if (y > list_top() + list_h()) break;
            if (sel) rrect(x, y, w, h, 14, C_ACC_SOFT);
            k = press_k(H_ROW0 + r % SLOTS, now);
            freq_unit(x + 14, y + h * 0.5f, list_f(r), 800, 25, 700, 14, col_alpha(sel ? orange : C_TEXT, 1 - press_dim(k)), 0.7f, 4);
            if (n[0]) t_fit(700, 19, x + 14 + 112 + 14, y + h * 0.5f, w - 140 - 6 - 52 - 6, n, C_TEXT);
            {   /* Stations: heart outline (add to favorites); Favorites: filled (remove) */
                float bx = x + w - 6 - 52, by = y + 6, bw = 52, bh = 52, kk = press_box(H_ACT0 + r % SLOTS, &bx, &by, &bw, &bh, now);
                if (ftab()) heart_ico(1, bx + bw * 0.5f, by + bh * 0.5f, 24 * kk, col_alpha(C_ACCENT, 1 - press_dim(kk)));
                else heart_ico(0, bx + bw * 0.5f, by + bh * 0.5f, 24 * kk, col_alpha(C_SUB, 1 - press_dim(kk)));
            }
        }
        if (list_n() * 66 - 2 + 10 > list_h()) {   /* scrollbar-width: thin */
            float lh = list_h(), mx = sc_l.max, th = fmaxf(30, lh * lh / (lh + mx));
            rrect(LX + LWID - 8, list_top() + (lh - th) * (mx > 0 ? sc_l.off / mx : 0), 8, th, 4, C_TRACK);
        }
    }
    clip_pop();
}

static void draw_off(double now)
{
    char t[48], f[16];
    float cx = X0 + CW * 0.5f, x = cx - 66, y = ROW_Y + ROW_H * 0.5f - 66 - 40, w = 132, h = 132;
    float k = press_box(H_ON, &x, &y, &w, &h, now);
    rrect_b(x, y, w, h, 66 * k, C_INNER, C_BORDER);
    ico("power_w22", x + w * 0.5f, y + h * 0.5f, 56 * k, C_ACCENT);
    y = ROW_Y + ROW_H * 0.5f - 66 - 40 + 132 + 14 + 8;
    t_center(800, 32, cx, y + t_lh(800, 32) * 0.5f, "Radio is off", C_TEXT);
    y += t_lh(800, 32) + 14;
    fmt_f(f, sizeof f, *cur());
    snprintf(t, sizeof t, tr("Tap to resume %s %s %s"), A.am ? "AM" : "FM", f, tr(BANDS[bi()].unit));
    t_center(600, 21, cx, y + t_lh(600, 21) * 0.5f, t, C_SUB);
}

static void reg_button(double now)
{
    float x = REG_X, y = Y0, w = reg_w(), h = 58, k = press_box(H_REG, &x, &y, &w, &h, now), o = tw_get(&reg_tw, now);
    rrect_b(x, y, w, h, 29 * k, reg_open ? C_SEG : C_INNER, C_BORDER);
    ico("globe", x + (18 + 11) * k, y + h * 0.5f, 22 * k, C_TEXT);
    t_left(700, 19, x + (18 + 22 + 10) * k, y + h * 0.5f, reg()->name, C_TEXT);
    /* the arrow rotates 180 (0.3 s): ours has no rotation - a down/up change with a transition (DEVIATIONS) */
    ico("chev_d", x + w - (16 + 10) * k, y + h * 0.5f, 20 * k, col_alpha(C_TEXT, 1 - o));
    ico("chev_u", x + w - (16 + 10) * k, y + h * 0.5f, 20 * k, col_alpha(C_TEXT, o));
}

static void reg_popup(double now)
{
    float x = pop_x(), y = POP_Y, h = pop_h(), ty = y + 10 + 8;
    int i;
    char t[64], a[16], b[16];
    shadow(x, y, POP_W, h, 24, 60, 0, 24, hexa(0, 0.45f));
    rrect_b(x, y, POP_W, h, 24, A.light ? hexa(0xFFFFFF, 0.95f) : hexa(0x24242A, 0.94f), C_BORDER);
    t_top(800, 21, x + 10 + 12, ty, "Radio region", C_TEXT, 0);
    t_top(600, 17, x + 10 + 12, ty + t_lh(800, 21) + 2, "Sets the FM/AM range and tuning step", C_SUB, 0);
    for (i = 0; i < NREG; i++) {
        const region_t *r = &REG[i];
        float ox = x + 10, oy = y + 10 + pop_hdr_h() + i * 62, ow = POP_W - 20, oh = 62, k = press_box(H_REG0 + i, &ox, &oy, &ow, &oh, now);
        float l1 = t_lh(700, 20), l2 = t_lh(600, 16), cy = oy + oh * 0.5f - (l1 + 1 + l2) * 0.5f;
        int sel = r->code == A.region;
        if (sel) rrect(ox, oy, ow, oh, 16 * k, C_SEG);
        t_top(700, 20, ox + 14 * k, cy, r->name, C_TEXT, 0);
        fmt_band(a, sizeof a, &r->b[0], r->b[0].lo); fmt_band(b, sizeof b, &r->b[0], r->b[0].hi);
        snprintf(t, sizeof t, tr("FM %s\xE2\x80\x93%s \xC2\xB7 AM %d\xE2\x80\x93%d kHz"), a, b, (int)r->b[1].lo, (int)r->b[1].hi);
        t_top(600, 16, ox + 14 * k, cy + l1 + 1, t, C_SUB, 0);
        if (sel) ico("tick", ox + ow - (14 + 12) * k, oy + oh * 0.5f, 24 * k, C_ACCENT);
    }
}

static void set_region(int code, double now)
{
    int b;
    reg_open = 0; tw_set(&reg_tw, 0, 300, &EASE, now);
    if (code == A.region) return;
    for (b = 0; b < 2; b++) { A.nfav[b] = A.nfound[b] = 0; A.found_known[b] = 0; }   /* the MCU erases the lists on a region change */
    A.scanning = A.seeking = 0; fav_tab = 0; sc_l.off = 0;
    radio_set_region(code);
    mcu_set_region(code);
}

/* region without an MCU command (and when the MCU itself reported it): frequencies - into the new range and grid */
void radio_set_region(int code)
{
    int b;
    A.region = code;
    for (b = 0; b < 2; b++) {
        const band_t *bd = &BANDS[b];
        float *f = b ? &A.am_f : &A.fm_f, v = bd->lo + roundf((*f - bd->lo) / bd->step) * bd->step;
        v = v < bd->lo ? bd->lo : v > bd->hi ? bd->hi : v;
        *f = b ? roundf(v) : roundf(v * 100) / 100;
    }
    radio_region_sent();
    layout_again();
}

static void draw_dyn(double now)
{
    static const char *const BL[2] = { "FM", "AM" };
    float k, p = tw_get(&pwr, now);
    if (!reg_open) reg_button(now);
    gfx_xform_push(0, 0, 1, 0, A.radio_on ? 1 : 0.5f);          /* FM/AM: 0.5 when the radio is off */
    seg_static(SEG_X, Y0, 2, 100, 48);
    seg_dyn(SEG_X, Y0, 2, 100, 48, tw_get(&band_th, now), BL, NULL, 800, 21, 0, H_FM, now);
    gfx_xform_pop();
    {
        float x = PWR_X, y = Y0, w = 58, h = 58;
        k = press_box(H_POWER, &x, &y, &w, &h, now);
        rrect_b(x, y, w, h, 29 * k, col_mix(C_ACCENT, C_INNER, p), C_BORDER);
        ico("power", x + w * 0.5f, y + h * 0.5f, 24 * k, col_mix(C_ONACCENT, C_TEXT, p));
    }
    if (!A.radio_on) draw_off(now);
    else { draw_left(now); draw_right(now); }
    if (reg_open) {               /* panel dimming, header and menu over it */
        rrect(ST_X, ST_Y, ST_W, ST_H, PANEL_R, hexa(0, 0.25f));
        head_title();
        gfx_xform_push(0, 0, 1, 0, A.radio_on ? 1 : 0.5f);
        seg_static(SEG_X, Y0, 2, 100, 48);
        seg_dyn(SEG_X, Y0, 2, 100, 48, tw_get(&band_th, now), BL, NULL, 800, 21, 0, H_FM, now);
        gfx_xform_pop();
        {
            float x = PWR_X, y = Y0, w = 58, h = 58;
            k = press_box(H_POWER, &x, &y, &w, &h, now);
            rrect_b(x, y, w, h, 29 * k, col_mix(C_ACCENT, C_INNER, p), C_BORDER);
            ico("power", x + w * 0.5f, y + h * 0.5f, 24 * k, col_mix(C_ONACCENT, C_TEXT, p));
        }
        reg_button(now);
        reg_popup(now);
    }
}

static void update(double now)
{
    int sig = reg_open * 20000000 + A.region * 40000000 + A.radio_on * 1000000 + A.scanning * 100000 + A.seeking * 10000 + A.nfav[bi()] * 300 + A.nfound[bi()] * 2
              + A.found_known[bi()] + A.am * 7 + ftab() * 5000000;
    if (sig != last_sig) {                       /* state changed - rebuild zones, update scrolling */
        last_sig = sig;
        sc_l.max = fmaxf(0, list_n() * 66 - 2 + 10 - list_h());
        if (sc_l.off > sc_l.max) sc_l.off = sc_l.max;
        tw_set(&tab_th, (float)ftab(), SPRING_MS, &SPRING_SOFT, now);
        layout_again();
        damage_d(ST_X, ST_Y, ST_W, ST_H);
    }
    if (pwr.to != (float)A.radio_on) tw_set(&pwr, (float)A.radio_on, 300, &EASE, now);   /* radio turned off not by the button (another source) */
    if (shown_f() != last_f) {                   /* the MCU also changes the frequency (seek/scan) */
        int moving = A.scanning || A.seeking;
        last_f = shown_f();
        tw_set(&needle, frac_of(last_f), moving ? 0 : 600, moving ? &LINEAR : &SPRING_NEEDLE, now);
        damage_d(ST_X, ST_Y, ST_W, ST_H);
    }
    /* holding the arrow 450 ms - seek in that direction (design) */
    if ((held_id() == H_UP || held_id() == H_DOWN) && !seek_held && !A.scanning && now - held_since() >= 0.45) {
        seek_held = 1;
        A.seek_dir = held_id() == H_UP ? 1 : -1;
        mcu_seek(A.seek_dir > 0);
        cancel_press(now);
    }
    if (held_id() < 0) seek_held = 0;
    if (sc_update(&sc_l, now) || sc_l.off != last_lo) { last_lo = sc_l.off; damage_d(LX, ROW_Y, LWID, ROW_H); layout_again(); }
    if (A.scanning || A.seeking) damage_d(X0, cb_y(), LW, 48);            /* blinking dot */
    if (tw_active(&band_th, now) || tw_active(&pwr, now) || tw_active(&reg_tw, now) || tw_active(&needle, now) || tw_active(&tab_th, now) || press_active(now))
        damage_d(ST_X, ST_Y, ST_W, ST_H);
}

static void step(int d)
{
    const band_t *b = &BANDS[bi()];
    float v = b->lo + (roundf((*cur() - b->lo) / b->step) + d) * b->step;
    v = v < b->lo ? b->lo : v > b->hi ? b->hi : v;
    *cur() = A.am ? roundf(v) : roundf(v * 100) / 100;
}

static int units(float f) { return A.am ? (int)lroundf(f) : (int)lroundf(f * radio_fm_div()); }   /* MHz/kHz -> MCU units */

static void click(int id, double now)
{
    int b = bi();
    if (id == H_REG || id == H_SHADE) { reg_open = id == H_REG && !reg_open; tw_set(&reg_tw, (float)reg_open, 300, &EASE, now); }
    else if (id >= H_REG0 && id < H_REG0 + NREG) set_region(REG[id - H_REG0].code, now);
    else if (id == H_POWER || id == H_ON) {
        A.radio_on = !A.radio_on;
        tw_set(&pwr, (float)A.radio_on, 300, &EASE, now);
    } else if (id == H_FM || id == H_AM) {
        A.am = id == H_AM;
        tw_set(&band_th, (float)A.am, SPRING_MS, &SPRING_SOFT, now);
        sc_l.off = 0;
    } else if (id == H_DOWN || id == H_UP) {
        if (A.seeking) mcu_tune(A.am, units(*cur()));                    /* tap during seek - stop */
        else if (!A.scanning) step(id == H_UP ? 1 : -1);
    } else if (id == H_STOP || id == H_CANCEL) mcu_tune(A.am, units(*cur()));   /* tuning interrupts seek/scan */
    else if (id == H_HEART) radio_fav_toggle(now); else if (id == H_SCAN || id == H_EMPTY_SCAN) { fav_tab = 0; sc_l.off = 0; mcu_scan(); }
    else if (id == H_TAB_ST) { fav_tab = 0; sc_l.off = 0; }
    else if (id == H_TAB_FAV || id == H_MORE) { if (!A.scanning) { fav_tab = 1; sc_l.off = 0; } }
    else if (id >= H_FAV0 && id < H_FAV0 + 4) mcu_pick_fav(id - H_FAV0 + 1);
    else if (id >= H_ROW0 && id < H_ROW0 + SLOTS && slot_row[id - H_ROW0] >= 0) {
        int r = slot_row[id - H_ROW0];
        if (ftab()) mcu_pick_fav(r + 1); else mcu_pick_found(r + 1);
    } else if (id >= H_ACT0 && id < H_ACT0 + SLOTS && slot_row[id - H_ACT0] >= 0) {
        int r = slot_row[id - H_ACT0];
        if (ftab()) { mcu_fav_remove(A.am, r + 1); hud_show(10, "Removed", now); }   /* MCU 0x38 */ else if (A.nfav[b] >= 20) hud_show(10, "Full \xC2\xB7 20 of 20", now);
        else { mcu_pick_found(r + 1); mcu_fav_toggle(); hud_show(10, "Added", now); }
    }
    layout_again();
    damage_d(ST_X, ST_Y, ST_W, ST_H);
}

static void drag(int id, float x, float y, int phase, double now)
{
    if (id == H_LIST) { sc_drag(&sc_l, y, phase, now); return; }
    if (id == H_TUNE && phase < 2) {
        const band_t *b = &BANDS[bi()];
        float v = b->lo + roundf(slider_value(X0, LW, x) * (b->hi - b->lo) / b->step) * b->step;
        *cur() = A.am ? roundf(v) : roundf(v * 100) / 100;
    }
}

static void enter(double now)
{
    (void)now;
    tw_init(&band_th, (float)A.am);
    tw_init(&pwr, (float)A.radio_on);
    tw_init(&tab_th, (float)ftab());
    reg_open = 0; tw_init(&reg_tw, 0);
    tw_init(&needle, frac_of(shown_f()));
    last_f = shown_f(); last_sig = -1; last_lo = -1;
    sc_l.v = 0; sc_l.drag = 0;
}

const screen_def SCR_RADIO = { layout, draw_static, draw_dyn, update, click, enter, drag };
