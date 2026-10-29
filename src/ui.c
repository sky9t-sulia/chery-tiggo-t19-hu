/* UI core (Car Media design): screen transitions, partial redraw, touches (press, hold,
 * drag, scroll), column, bottom climate strip, HUD, theme, wallpaper, brightness, Bluetooth logic,
 * "Running app" window. Screens are in scr_*.c.
 *
 * Layers: GFX_L_BASE (wallpaper, column, bottom strip glass), GFX_L_SCREEN (static part of the current screen),
 * on top - moving/pressable. A frame is drawn only in the changed areas (scissor), taking the age of
 * the back buffer into account; during transitions and theme changes - in full. */
#include "ui.h"
#include "gl.h"
#include "ui_int.h"
#include "aa.h"
#include "aa_wifi.h"
#include "i18n.h"
#include "mcu.h"
#include "fm1388.h"
#include "cam.h"
#include "bt.h"
#include "media.h"
#include "lua_app.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <dirent.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* ================================================================= shared state and the design's demo data */

app_t A;

/* USB stick playlist in display order (A–Z / Date): next / prev / auto-advance follow it */
static int *pl_ord, pl_cap, ord_gen = -1, ord_by = -1;

static int cmp_az(const void *a, const void *b)
{
    int i = *(const int *)a, j = *(const int *)b, r = strcasecmp(TRACKS[i].t, TRACKS[j].t);
    return r ? r : i - j;
}
static int cmp_date(const void *a, const void *b)
{
    double d = TRACKS[*(const int *)b].added - TRACKS[*(const int *)a].added;
    return d > 0 ? 1 : d < 0 ? -1 : *(const int *)a - *(const int *)b;
}
const int *playlist(void)
{
    int i, *g;
    if (ord_gen == media_gen() && ord_by == A.pl_date) return pl_ord;
    if (!(g = grow(pl_ord, &pl_cap, NTRACKS, sizeof *pl_ord))) return pl_ord;
    pl_ord = g;
    for (i = 0; i < NTRACKS; i++) pl_ord[i] = i;
    if (NTRACKS) qsort(pl_ord, (size_t)NTRACKS, sizeof(int), A.pl_date ? cmp_date : cmp_az);
    ord_gen = media_gen(); ord_by = A.pl_date;
    return pl_ord;
}
int playlist_pos(int trk)
{
    const int *o = playlist();
    int i;
    for (i = 0; i < NTRACKS; i++) if (o[i] == trk) return i;
    return 0;
}
int next_track(void)
{
    int r = A.trk;
    if (NTRACKS < 2) return 0;
    if (A.shuffle) { while (r == A.trk) r = rand() % NTRACKS; return r; }
    return playlist()[(playlist_pos(A.trk) + 1) % NTRACKS];
}
int prev_track(void)
{
    if (NTRACKS < 2) return 0;
    return playlist()[(playlist_pos(A.trk) + NTRACKS - 1) % NTRACKS];
}

const char *const SCENE_FILE[N_SCENE] = { "dusk", "ocean", "aurora", "forest", "graphite", "sunset", "lagoon", "midnight", "ember", "rose", "sand" };
const char *const SCENE_NAME[N_SCENE] = { "Dusk", "Ocean", "Aurora", "Forest", "Graphite", "Sunset", "Lagoon", "Midnight", "Ember", "Rose", "Sand" };

static const screen_def *SCREENS[S_COUNT] = {
    &SCR_DASH, &SCR_APPS, &SCR_MUSIC, &SCR_VIDEO, &SCR_RADIO, &SCR_CAMERA, &SCR_DISPLAY, &SCR_PHONE,
    &SCR_USB, &SCR_SETTINGS, &SCR_SETTINGS, &SCR_VEHICLE, &SCR_LUA
};

/* ================================================================= dirty areas and clipping */

typedef struct { float x, y, w, h; } rect;
#define MAXR 48
#define HIST 4
static rect dmg[MAXR], hist[HIST][MAXR];
static int nd, full, nhist[HIST], hist_full[HIST] = { 1, 1, 1, 1 };
static int base_dirty_f = 1, screen_dirty_f = 1;
static int cur_sc[4] = { 0, 0, SCREEN_W, SCREEN_H }, cur_sc_on;   /* current redraw area (screen px) */
static int clip_stack[4][5], clip_n;

void damage_d(float x, float y, float w, float h)
{
    if (full) return;
    if (nd >= MAXR) { full = 1; return; }
    dmg[nd].x = floorf(D(x)) - 2; dmg[nd].y = floorf(D(y)) - 2;
    dmg[nd].w = ceilf(D(w)) + 4; dmg[nd].h = ceilf(D(h)) + 4;
    nd++;
}
void damage_full(void) { full = 1; }
void screen_dirty(void) { screen_dirty_f = 1; }
void base_dirty(void) { base_dirty_f = 1; }
int any_active(const tween *t, int n, double now) { int i; for (i = 0; i < n; i++) if (tw_active(&t[i], now)) return 1; return 0; }

static void apply_scissor(void)
{
    if (cur_sc_on) gfx_scissor(1, cur_sc[0], cur_sc[1], cur_sc[2], cur_sc[3]); else gfx_scissor(0, 0, 0, 0, 0);
}

void clip_push(float x, float y, float w, float h)
{
    int x0 = (int)floorf(D(x)), y0 = (int)floorf(D(y)), x1 = (int)ceilf(D(x + w)), y1 = (int)ceilf(D(y + h));
    if (clip_n < 4) {
        clip_stack[clip_n][0] = cur_sc[0]; clip_stack[clip_n][1] = cur_sc[1];
        clip_stack[clip_n][2] = cur_sc[2]; clip_stack[clip_n][3] = cur_sc[3]; clip_stack[clip_n][4] = cur_sc_on;
        clip_n++;
    }
    if (cur_sc_on) {                                   /* intersection with the current area */
        int ax1 = cur_sc[0] + cur_sc[2], ay1 = cur_sc[1] + cur_sc[3];
        if (x0 < cur_sc[0]) x0 = cur_sc[0];
        if (y0 < cur_sc[1]) y0 = cur_sc[1];
        if (x1 > ax1) x1 = ax1;
        if (y1 > ay1) y1 = ay1;
    }
    cur_sc[0] = x0; cur_sc[1] = y0; cur_sc[2] = x1 - x0; cur_sc[3] = y1 - y0; cur_sc_on = 1;
    apply_scissor();
}

void clip_pop(void)
{
    if (clip_n > 0) {
        clip_n--;
        cur_sc[0] = clip_stack[clip_n][0]; cur_sc[1] = clip_stack[clip_n][1];
        cur_sc[2] = clip_stack[clip_n][2]; cur_sc[3] = clip_stack[clip_n][3]; cur_sc_on = clip_stack[clip_n][4];
    }
    apply_scissor();
}

/* ================================================================= touches */

enum { HT_BUTTON, HT_DRAG, HT_SCROLL };
typedef struct { float x, y, w, h; int type; } hitbox;
static hitbox hit[HIT_MAX];
static int press_id = -1, anim_id = -1, drag_id = -1, scroll_id = -1, scrolling, app_touch;
static float down_x, down_y;
static double down_t;
static tween press_tw;

void set_hit(int id, float x, float y, float w, float h) { hit[id].x = x; hit[id].y = y; hit[id].w = w; hit[id].h = h; hit[id].type = HT_BUTTON; }
void set_drag(int id, float x, float y, float w, float h) { set_hit(id, x, y, w, h); hit[id].type = HT_DRAG; }
void set_scroll(int id, float x, float y, float w, float h) { set_hit(id, x, y, w, h); hit[id].type = HT_SCROLL; }
void clear_screen_hits(void) { memset(&hit[HIT_SCREEN], 0, sizeof(hitbox) * (HIT_MAX - HIT_SCREEN)); }
float press_k(int id, double now) { return id == anim_id ? tw_get(&press_tw, now) : 1.0f; }
float press_dim(float k) { return (1.0f - k) / (1.0f - PRESS_SCALE) * PRESS_DIM; }
int press_active(double now) { return anim_id >= 0 && tw_active(&press_tw, now); }
int held_id(void) { return press_id; }
double held_since(void) { return down_t; }
void cancel_press(double now) { press_id = -1; tw_set(&press_tw, 1, PRESS_MS, &EASE, now); }
void hw_press(int id, int down, double now)
{
    if (press_id >= 0) return;                         /* a finger on the screen takes priority */
    if (down) { anim_id = id; tw_init(&press_tw, 1); tw_set(&press_tw, PRESS_SCALE, PRESS_MS, &EASE, now); }
    else if (anim_id == id) tw_set(&press_tw, 1, PRESS_MS, &EASE, now);
}

/* ================================================================= core: button ids */

enum { B_RAIL0 = 0, B_APPS = 5, B_DASH, B_AC, B_COLD, B_HOT, B_FAN_DN, B_FAN_UP, B_AIR0,
       B_THEME = B_AIR0 + 5, B_BLOWER, B_RECIRC, B_TEMP_DRAG, B_FAN_DRAG, B_APP_CLOSE, B_IN_DEC, B_IN_ACC };
static const int RAIL_SCREEN[5] = { S_MUSIC, S_VIDEO, S_RADIO, S_CAMERA, S_PHONE };
static const char *RAIL_ICON[5] = { "music", "video", "radio", "camera", "phone" };
static const float RAIL_ICON_SZ[5] = { 32, 32, 32, 32, 30 };
static const uint32_t RAIL_COL[5] = { 0xFF4F6A, 0x7B61FF, 0xFF8A2A, 0x14A3A3, 0x2DBE6C };
/* 16 temperature steps: blue (37,99,235) -> gray (175,175,190) -> red (220,38,38), as in the design */
static uint32_t temp_col(int i)
{
    float u = i / 15.0f, a = u < 0.5f ? u * 2 : (u - 0.5f) * 2;
    int r = u < 0.5f ? (int)lroundf(37 + 138 * a) : (int)lroundf(175 + 45 * a);
    int g = u < 0.5f ? (int)lroundf(99 + 76 * a) : (int)lroundf(175 - 137 * a);
    int b = u < 0.5f ? (int)lroundf(235 - 45 * a) : (int)lroundf(190 - 152 * a);
    return (uint32_t)(r << 16 | g << 8 | b);
}
static const char *AIR_NAMES[5] = { "Face", "Face + Feet", "Feet", "Feet + Screen", "Windscreen" };

/* ================================================================= core state */

static struct {
    int screen, pending, phase;          /* phase: 0 pre, 1 in, 2 out */
    double phase_t;
    tween st_op, st_s, st_dy;
    tween r_op[5], r_sc[5], r_glow[5], r_ind[5], r_bg[2];
    int ac; tween ac_fill;
    int temp; tween seg_h[16], seg_op[16];
    int fan, blower; tween fan_h[7], fan_on[7], blower_bg;
    int recirc; tween recirc_bg;
    int air; tween air_x, air_ic[5], air_pill;
    tween clim_a;                         /* without ignition, climate labels and icons are dimmed (owner) */
    int clim_live;                        /* ignition (0x50 >= 2): climate works; otherwise the panel is off and not pressable */
    tween theme_rot;
    int hud_kind, hud_shown; double hud_t; tween hud_op, hud_k; char hud_text[40];
    tween fade; int fading;
    tween app_op, app_k, app_launch, app_prog; int app_pulse;
    double tick_t, last_now;
    long tick;
    int last_min, last_bat;
} U;
static int rev_lock;                               /* reverse: only the Camera screen (owner) */

static gfx_tex wall;
static char assets[256];
const char *ui_assets(void) { return assets; }
#define AIR_STEP 58                    /* v32: airflow mode button 58, gap 0, border 1 */
#define AIR_W (5 * AIR_STEP + 2)
#define FAN_AX 116                     /* fan bars: 60 + 2 + gap 2 + 50 (-) + gap 2 */
static float bb_y = 672, ac_x = 122, ac_w, tp_x, tp_w, fan_x, fan_w, rc_x, air_x0, th_x;

int cur_screen(void) { return U.screen; }
int ui_screen(void) { return U.screen; }

/* ================================================================= core layout */

/* bottom strip: gap 12; A/C, recirculation, airflow, theme - sized by content; temperature and fan
 * share the remainder 2:1 (flex: 2 1 auto / 1 1 auto, base widths by content 178 and 210);
 * v32: fan power 60 and airflow modes 58 - full pill height (over the border) */
static void layout_core(void)
{
    int i;
    float fr;
    ac_w = 2 + 4 + 14 + 10 + 8 + t_width(800, 22, "A/C") + 14 + 4;   /* v20: dot 10 + gap 8 before "A/C" */
    fr = 1140 - (ac_w + 84 + AIR_W + 60) - 5 * 12 - 178 - 210;
    tp_w = 178 + fr * 2 / 3; fan_w = 210 + fr / 3;
    tp_x = ac_x + ac_w + 12;
    fan_x = tp_x + tp_w + 12;
    rc_x = fan_x + fan_w + 12;
    air_x0 = rc_x + 84 + 12;
    th_x = air_x0 + AIR_W + 12;
    for (i = 0; i < 5; i++) set_hit(B_RAIL0 + i, 18, 131 + i * 86, RAIL_BTN, RAIL_BTN);
    set_hit(B_APPS, 16, 598, 72, 58);
    set_hit(B_DASH, 16, 674, 72, 58);
    set_hit(B_AC, ac_x, bb_y, ac_w, PILL_H);
    set_hit(B_COLD, tp_x + 5, bb_y + 7, 46, 46);
    set_drag(B_TEMP_DRAG, tp_x + 53, bb_y, tp_w - 106, PILL_H);
    set_hit(B_HOT, tp_x + tp_w - 51, bb_y + 7, 46, 46);
    set_hit(B_BLOWER, fan_x, bb_y, 60, 60);
    set_hit(B_FAN_DN, fan_x + 64, bb_y + 5, 50, 50);
    set_drag(B_FAN_DRAG, fan_x + FAN_AX, bb_y, fan_w - FAN_AX - 57, PILL_H);
    set_hit(B_FAN_UP, fan_x + fan_w - 55, bb_y + 5, 50, 50);
    set_hit(B_RECIRC, rc_x, bb_y, 84, PILL_H);
    for (i = 0; i < 5; i++) set_hit(B_AIR0 + i, air_x0 + 1 + i * AIR_STEP, bb_y + 1, AIR_STEP, AIR_STEP);
    set_hit(B_THEME, th_x, bb_y, 60, 60);
    if (!U.clim_live) {                                  /* without ignition the climate is not pressable (theme is) */
        static const int CL[] = { B_AC, B_COLD, B_TEMP_DRAG, B_HOT, B_BLOWER, B_FAN_DN, B_FAN_DRAG, B_FAN_UP, B_RECIRC };
        for (i = 0; i < (int)(sizeof CL / sizeof CL[0]); i++) hit[CL[i]].w = 0;
        for (i = 0; i < 5; i++) hit[B_AIR0 + i].w = 0;
    }
}

static float app_close_w(void) { return 20 + 18 + 8 + t_width(700, 19, "Close") + 20; }

static void layout_app(void)
{
    if (A.app_run) set_hit(B_APP_CLOSE, ST_X + ST_W - 16 - app_close_w(), ST_Y + 13, app_close_w(), 50);
    else hit[B_APP_CLOSE].w = 0;
}

void layout_again(void)
{
    clear_screen_hits();
    if (SCREENS[U.screen]->layout) SCREENS[U.screen]->layout();
}

/* ================================================================= bottom layer */

static const btdev *conn_dev(void)
{
    int i;
    if (!A.bt_on || !A.bt_conn[0]) return NULL;
    for (i = 0; i < A.n_paired; i++) if (!strcmp(A.paired[i].id, A.bt_conn)) return &A.paired[i];
    return NULL;
}

/* internet via the phone's USB tethering: rndis_host / cdc_ncm / cdc_ether interface with an IPv4 address (once per 1 s) */
static void net_poll(double now)
{
    static double t;
    DIR *d;
    struct dirent *e;
    int on = 0;
    char p[320], l[320], dev[32] = "";
    if (now - t < 1) return;
    t = now;
    if (!(d = opendir("/sys/class/net"))) return;
    while (!on && (e = readdir(d))) {
        struct ifreq ifr;
        ssize_t n;
        const char *drv;
        int fd;
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/net/%s/device/driver", e->d_name);
        if ((n = readlink(p, l, sizeof l - 1)) <= 0) continue;
        l[n] = 0; drv = strrchr(l, '/'); drv = drv ? drv + 1 : l;
        if (strcmp(drv, "rndis_host") && strcmp(drv, "cdc_ncm") && strcmp(drv, "cdc_ether")) continue;
        if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) continue;
        memset(&ifr, 0, sizeof ifr);
        snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%.15s", e->d_name);
        on = ioctl(fd, SIOCGIFADDR, &ifr) == 0;
        close(fd);
        if (on) {                                        /* phone name: product of the USB device */
            FILE *f;
            snprintf(p, sizeof p, "/sys/class/net/%s/device/../product", e->d_name);
            if ((f = fopen(p, "r"))) { if (fgets(dev, sizeof dev, f)) dev[strcspn(dev, "\n")] = 0; fclose(f); }
        }
    }
    closedir(d);
    if (on != A.net || strcmp(dev, A.net_dev)) { A.net = on; snprintf(A.net_dev, sizeof A.net_dev, "%s", dev); base_dirty_f = 1; layout_again(); }
}

/* USB stick: mounted filesystem in /media/usb* (mounted by the HU system on insertion), once per 1 s */
static void app_close(double now);
static int lua_rescan;
static int app_covers(double now);

static void usb_poll(double now)
{
    static double t;
    FILE *f;
    char line[512], mnt[256] = "";
    if (now - t < 1) return;
    t = now;
    if (getenv("HU_USB_DIR")) snprintf(mnt, sizeof mnt, "%s", getenv("HU_USB_DIR"));   /* testing: a directory instead of a USB stick */
    else if ((f = fopen("/proc/mounts", "r"))) {
        while (!mnt[0] && fgets(line, sizeof line, f)) {
            char dev[128], dir[256];
            if (sscanf(line, "%63s %63s", dev, dir) == 2 && !strncmp(dir, "/media/usb", 10) && !strncmp(dev, "/dev/sd", 7))
                snprintf(mnt, sizeof mnt, "%s", dir);
        }
        fclose(f);
    }
    if (!!mnt[0] != A.usb_conn) {
        A.usb_conn = !!mnt[0];
        if (A.usb_conn && U.last_now > 3) hud_show(11, "Connected", now);   /* inserted (not at startup) */
        if (!A.usb_conn) { A.playing = 0; A.vplaying = 0; }
        base_dirty_f = screen_dirty_f = 1;
        layout_again();
    }
    if (strcmp(A.usb_mnt, mnt)) {                        /* Lua programs come from the USB stick only */
        const lua_app *ra = lua_apps_get(lua_app_running());
        snprintf(A.usb_mnt, sizeof A.usb_mnt, "%s", mnt);
        if (A.app_run && (!ra || ra->usb)) app_close(now);  /* a built-in (NAND) one works without the stick too */
        lua_rescan = 1;
        screen_dirty_f = 1;
    }
    if (lua_rescan && !A.app_run) {                     /* program list - when none is running */
        lua_rescan = 0;
        lua_apps_scan(A.usb_mnt[0] ? A.usb_mnt : NULL);
        screen_dirty_f = 1;
    }
    if (mnt[0]) {
        struct statvfs sv;
        int ok = statvfs(mnt, &sv) == 0;
        float gb = ok ? (float)((double)sv.f_bavail * sv.f_frsize / 1e9) : 0;
        A.usb_total_gb = ok ? (float)((double)sv.f_blocks * sv.f_frsize / 1e9) : 0;
        if (fabsf(gb - A.usb_free_gb) >= 0.05f) { A.usb_free_gb = gb; screen_dirty_f = 1; }
    }
}

void usb_eject(double now)
{
    if (!A.usb_conn) return;
    A.playing = 0; A.vplaying = 0;
    sync();
    if (umount(A.usb_mnt) == 0) {
        hud_show(11, "Safe to remove", now);
        A.usb_conn = 0; A.usb_mnt[0] = 0;
        base_dirty_f = screen_dirty_f = 1;
        layout_again();
    } else fprintf(stderr, "hu: usb: umount %s failed\n", A.usb_mnt);
}

static void draw_base(void)
{
    char clk[8];
    time_t t = time(NULL);
    struct tm tm;
    int bat = conn_dev() != NULL;
    localtime_r(&t, &tm);
    snprintf(clk, sizeof clk, "%02d:%02d", tm.tm_hour, tm.tm_min);

    gfx_wallpaper();
    gfx_glass(0, 0, D(RAIL_W), D(750), 0, RAIL_TINT, RAIL_A, 0);
    gfx_rect(D(RAIL_W) - 1, 0, 1, D(750), 0, C_BORDER);
    t_top(700, 26, (RAIL_W - t_width(700, 26, clk) + 0.26f * 4) * 0.5f, 20, clk, C_TEXT, -0.26f);
    {   /* status row under the clock (owner 2026-10-29): internet over USB (wired icon) · LTE · Bluetooth - one row, icons ~8 apart
         * (centers 26 / 52 / 78, owner), top 63 (clock + 8 + 4), icons 18 (wired 19). Off / no phone = 0.4;
         * phone signal 0..5 (HFP): bars at >=1, >=2, >=4, >=5, the rest 0.3 */
        static const char *SB[4] = { "sig1", "sig2", "sig3", "sig4" };
        static const int LV[4] = { 1, 2, 4, 5 };
        int k;
        ico("png_wired", 26, 73, 19, col_alpha(C_SUB, A.net ? 1 : 0.4f));
        for (k = 0; k < 4; k++) ico(SB[k], 52, 73, 18, col_alpha(C_SUB, !bat ? 0.4f : A.ph_sig >= LV[k] ? 1 : 0.3f));
        ico("bluetooth", 78, 73, 18, col_alpha(C_SUB, bat ? 1 : 0.4f));
    }
    panel(ac_x, bb_y, ac_w, PILL_H, PILL_R);
    panel(tp_x, bb_y, tp_w, PILL_H, PILL_R);
    panel(fan_x, bb_y, fan_w, PILL_H, PILL_R);
    panel(rc_x, bb_y, 84, PILL_H, PILL_R);
    panel(air_x0, bb_y, AIR_W, PILL_H, PILL_R);
    panel(th_x, bb_y, 60, PILL_H, PILL_R);
}

/* ================================================================= column (moving) */

static void draw_rail(double now)
{
    int i;
    for (i = 0; i < 5; i++) {
        float k = press_k(B_RAIL0 + i, now), sc = tw_get(&U.r_sc[i], now) * k;
        float op = tw_get(&U.r_op[i], now), gl = tw_get(&U.r_glow[i], now), ind = tw_get(&U.r_ind[i], now);
        float y = 131 + i * 86, s = RAIL_BTN * sc, x0 = 18 + (RAIL_BTN - s) * 0.5f, y0 = y + (RAIL_BTN - s) * 0.5f;
        col c = hexc(RAIL_COL[i]);
        if (gl > 0.01f) shadow(x0, y0, s, s, RAIL_BTN_R * sc, 26, 0, 10, col_alpha(c, 0.5f * gl));
        rrect(x0, y0, s, s, RAIL_BTN_R * sc, col_alpha(c, op));
        if (k < 1) rrect(x0, y0, s, s, RAIL_BTN_R * sc, hexa(0, press_dim(k) * op));
        ico(RAIL_ICON[i], x0 + s * 0.5f, y0 + s * 0.5f, RAIL_ICON_SZ[i] * sc, hexa(0xFFFFFF, op));
        if (ind > 0.01f) {
            float ih = 30 * (0.2f + 0.8f * ind);
            rrect(0, y + 34 - ih * 0.5f, 4, ih, 2, col_alpha(C_TEXT, fminf(1, ind)));
        }
    }
    for (i = 0; i < 2; i++) {
        int id = i ? B_DASH : B_APPS;
        float x = 16, y = i ? 674 : 598, w = 72, h = 58, k = press_box(id, &x, &y, &w, &h, now);
        float bg = tw_get(&U.r_bg[i], now);
        if (bg > 0.01f) { gfx_squircle(1); rrect(x, y, w, h, 22 * k, col_alpha(C_RAILACT, bg)); gfx_squircle(0); }   /* v35: 22, squircle */
        ico(i ? "dashboard" : "apps", x + w * 0.5f, y + h * 0.5f, 30 * k, col_alpha(C_TEXT, 1 - press_dim(k)));
    }
}

/* ================================================================= bottom strip (moving) */

static void btn_ico(int id, const char *icon, float cx, float cy, float sz, col c, double now)
{
    float k = press_k(id, now);
    ico(icon, cx, cy, sz * k, col_alpha(c, 1 - press_dim(k)));
}

/* segment bars (temperature, fan): flex: 1, gap 4, padding 0 6 */
static float seg_w(float area_w, int n) { return (area_w - 12 - 4 * (n - 1)) / n; }

static void draw_bottom(double now)
{
    int i;
    float cy = bb_y + 30, ca = tw_get(&U.clim_a, now);   /* dimmed without ignition */
    {   /* A/C: glass pill; v32: on - the whole pill is c.accent (background and border) */
        float x = ac_x, y = bb_y, w = ac_w, h = PILL_H, k = press_box(B_AC, &x, &y, &w, &h, now);
        float f = tw_get(&U.ac_fill, now), tw = t_width(800, 22, "A/C"), ix = x + 5 * k;
        col fg = col_alpha(col_mix(C_TEXT, C_ONACCENT, f), ca);
        if (f > 0.01f) rrect(x, y, w, h, PILL_R * k, col_alpha(C_ACCENT, f));
        if (k < 1) rrect(x, y, w, h, PILL_R * k, hexa(0, press_dim(k)));
        {
            font *fo = F(800, 22);
            rrect(ix + 14 * k, cy - 5 * k, 10 * k, 10 * k, 5 * k, col_alpha(fg, 0.3f + 0.7f * f));   /* v20: dot, 1 / .3 */
            text_draw(fo, D(ix + (14 + 10 + 8) * k), D(cy) - text_line_height(fo) * 0.5f, "A/C", fg, 0);
        }
        (void)tw;
    }
    {   /* temperature: 16 segments */
        float ax = tp_x + 53, aw = tp_w - 106, sw = seg_w(aw, 16);
        btn_ico(B_COLD, "png_cold", tp_x + 28, cy, 26, col_alpha(C_COLD, ca), now);
        btn_ico(B_HOT, "flame", tp_x + tp_w - 28, cy, 28, col_alpha(C_HOT, ca), now);
        for (i = 0; i < 16; i++) {
            float h = tw_get(&U.seg_h[i], now), op = tw_get(&U.seg_op[i], now);
            rrect(ax + 6 + i * (sw + 4), cy - h * 0.5f, sw, h, 3, hexa(temp_col(i), op));
        }
    }
    {   /* fan: power, -, 7 bars, + */
        float ax = fan_x + FAN_AX, aw = fan_w - FAN_AX - 57, sw = seg_w(aw, 7), b = tw_get(&U.blower_bg, now);
        float x = fan_x, y = bb_y, w = 60, h = 60, k = press_box(B_BLOWER, &x, &y, &w, &h, now);
        if (b > 0.01f) rrect(x, y, w, h, 30 * k, col_alpha(C_ACCENT, b));
        if (k < 1) rrect(x, y, w, h, 30 * k, hexa(0, press_dim(k)));
        ico("power", x + w * 0.5f, cy, 28 * k, col_alpha(col_mix(C_TEXT, C_ONACCENT, b), ca));
        btn_ico(B_FAN_DN, "minus", fan_x + 89, cy, 24, col_alpha(C_TEXT, ca), now);
        btn_ico(B_FAN_UP, "plus", fan_x + fan_w - 30, cy, 24, col_alpha(C_TEXT, ca), now);
        for (i = 0; i < 7; i++) {
            float h = tw_get(&U.fan_h[i], now), on = tw_get(&U.fan_on[i], now);
            rrect(ax + 6 + i * (sw + 4), cy - h * 0.5f, sw, h, 3, col_alpha(col_mix(C_TEXT, C_ACCENT, on), 0.2f + 0.8f * on));
        }
    }
    {   /* recirculation / fresh air; v32: on - the whole pill is c.accent (background and border) */
        float x = rc_x, y = bb_y, w = 84, h = PILL_H, k = press_box(B_RECIRC, &x, &y, &w, &h, now);
        float b = tw_get(&U.recirc_bg, now);
        if (b > 0.01f) rrect(x, y, w, h, PILL_R * k, col_alpha(C_ACCENT, b));
        if (k < 1) rrect(x, y, w, h, PILL_R * k, hexa(0, press_dim(k)));
        ico(U.recirc ? "png_recirc" : "png_fresh", x + w * 0.5f, cy, 46 * k, col_alpha(col_mix(C_TEXT, C_ONACCENT, b), ca));
    }
    {   /* airflow: v32 thumb 60 over the border, step 58; the selected one has its own mask */
        static const char *ON[5] = { "png_air0_on", "png_air1_on", "png_air2_on", "png_air3_on", "png_air4_on" };
        static const char *OFF[5] = { "png_air0_off", "png_air1_off", "png_air2_off", "png_air3_off", "png_air4_off" };
        float pill = tw_get(&U.air_pill, now);
        if (pill > 0.005f) rrect(air_x0 + tw_get(&U.air_x, now), bb_y, 60, 60, 30, col_alpha(C_ACCENT, pill));
        for (i = 0; i < 5; i++) {
            float m = tw_get(&U.air_ic[i], now), kk = press_k(B_AIR0 + i, now);
            ico(i == U.air && pill > 0.5f ? ON[i] : OFF[i], air_x0 + 30 + i * AIR_STEP, cy, 30 * kk, col_alpha(col_mix(C_TEXT, C_ONACCENT, m), ca));
        }
    }
    {
        float k = press_k(B_THEME, now);
        ico_rot(A.theme_auto ? "auto_theme" : A.light ? "sun_w20" : "moon2", th_x + 30, cy, 30 * k,   /* Auto is a deviation, see DEVIATIONS */
                col_alpha(C_TEXT, 1 - press_dim(k)), tw_get(&U.theme_rot, now));
    }
}

/* ================================================================= HUD */

void hud_show(int kind, const char *text, double now)
{
    U.hud_kind = kind; U.hud_shown = 1; U.hud_t = now;
    snprintf(U.hud_text, sizeof U.hud_text, "%s", text ? text : "");
    tw_set(&U.hud_op, 1, 350, &EASE, now);
    tw_set(&U.hud_k, 1, SPRING_MS, &SPRING, now);
}

static void draw_hud(double now)
{
    float op = tw_get(&U.hud_op, now), kk = tw_get(&U.hud_k, now);
    float s = 0.92f + 0.08f * kk, dy = -16 * (1 - kk);
    float x, y = 30, w, h = PILL_H;
    char val[48];
    const char *lab = "Media volume";               /* design: "Volume"; owner: the label follows what is being changed */
    float frac = 0;
    col fill = C_TEXT;
    int bar = 0;
    if (op < 0.005f) return;
    switch (U.hud_kind) {
    case 0: if (A.muted) snprintf(val, sizeof val, "Muted"); else snprintf(val, sizeof val, "%d", A.vol);
            frac = A.muted ? 0 : A.vol / 31.0f; bar = 1; break;
    case 12: lab = "Phone volume"; snprintf(val, sizeof val, "%d", A.phone_vol); frac = A.phone_vol / 25.0f; bar = 1; break;
    case 1: lab = "Temperature"; snprintf(val, sizeof val, "%d", U.temp + 1); frac = (U.temp + 1) / 16.0f;
            fill = hexc(temp_col(U.temp)); bar = 1; break;
    case 8: lab = "Fan"; if (U.blower) snprintf(val, sizeof val, "%d", U.fan); else snprintf(val, sizeof val, "Off");
            frac = U.blower ? U.fan / 7.0f : 0; fill = C_ACCENT; bar = 1; break;
    case 10: lab = "Favorites"; snprintf(val, sizeof val, "%s", U.hud_text); break;
    case 11: lab = "USB drive"; snprintf(val, sizeof val, "%s", U.hud_text); break;
    case 9: lab = "Air"; snprintf(val, sizeof val, "%s", U.recirc ? "Recirculation" : "Fresh air"); break;
    case 2: lab = "Airflow"; snprintf(val, sizeof val, "%s", AIR_NAMES[U.air]); break;
    case 3: lab = "Sound"; snprintf(val, sizeof val, "Defaults restored"); break;
    case 4: lab = "Display"; snprintf(val, sizeof val, "Defaults restored"); break;
    case 5: lab = "Vehicle"; snprintf(val, sizeof val, "Defaults restored"); break;
    case 6: lab = "Wallpaper"; snprintf(val, sizeof val, "Updated"); break;
    case 14: lab = "Android Auto"; snprintf(val, sizeof val, "Ready"); break;   /* owner 2026-10-26: wireless AA came up in the background */
    default: lab = "Can\xE2\x80\x99t open"; snprintf(val, sizeof val, "%s", U.hud_text); break;
    }
    {   /* v33: width by content (max-content, 240..600), centered in the screen area; bar 160 - only hud.on;
         * value pushed to the right (margin-left: auto), a long one gets an ellipsis */
        float lw = t_width(700, 20, lab), vw = t_width(700, 20, val), vx, av;
        w = fminf(600, fmaxf(240, 22 + lw + 16 + (bar ? 160 + 16 : 0) + vw + 22));
        x = 692 - w * 0.5f;
        vx = x + 22 + lw + 16 + (bar ? 160 + 16 : 0); av = x + w - 22 - vx;
        gfx_xform(D(x + w * 0.5f), D(y + h * 0.5f), s, D(dy), op);
        shadow(x, y, w, h, PILL_R, 40, 0, 16, hexa(0, 0.25f));
        gfx_glass(D(x), D(y), D(w), D(h), D(PILL_R), HUD_TINT, HUD_A, GLASS_EDGE);
        t_left(700, 20, x + 22, y + 30, lab, C_SUB);
        if (bar) {
            rrect(x + 22 + lw + 16, y + 26, 160, 8, 4, C_TRACK);
            if (frac > 0) rrect(x + 22 + lw + 16, y + 26, 160 * frac, 8, 4, fill);
        }
        if (vw <= av + 1) t_right(700, 20, x + w - 22, y + 30, val, C_TEXT);   /* +1: width exactly by text (float error) */
        else t_fit(700, 20, vx, y + 30, av, val, C_TEXT);
    }
    gfx_xform_reset();
}

/* ================================================================= incoming call (design v18)
 * Over any screen - a card (inset 18 in the screen area); on the Camera - a banner at the top 560x104 (don't cover the view). */
static tween in_p[2], in_po[2], in_shake;
static int in_tick = -1;

static int incoming(void) { return A.bt_call == 1; }
static int in_banner(void) { return U.screen == S_CAMERA; }
#define BAN_X 392.0f                       /* left 50% (104 + 588) - 300 */
#define BAN_Y 32.0f

static float in_top(void)                  /* card: centered column */
{
    float h = 40 + 34 + 150 + 22 + t_lh(800, 42) + 4 + t_lh(600, 23) + 44 + 96 + 10 + t_lh(700, 20);
    return ST_Y + (ST_H - h) * 0.5f;
}
static void in_layout(void)
{
    if (!incoming()) { hit[B_IN_DEC].w = hit[B_IN_ACC].w = 0; return; }
    if (in_banner()) {
        set_hit(B_IN_DEC, BAN_X + 560 - 16 - 76 - 12 - 76, BAN_Y + 14, 76, 76);
        set_hit(B_IN_ACC, BAN_X + 560 - 16 - 76, BAN_Y + 14, 76, 76);
    } else {
        float y = in_top() + 40 + 34 + 150 + 22 + t_lh(800, 42) + 4 + t_lh(600, 23) + 44;
        set_hit(B_IN_DEC, 692 - 60 - 96, y, 96, 96);
        set_hit(B_IN_ACC, 692 + 60, y, 96, 96);
    }
}
static void in_update(double now)
{
    int t = (int)(now * 1000 / TICK_MS);
    if (!incoming()) { in_tick = -1; return; }
    if (t != in_tick) {                    /* 250 ms tick: two circles in turn (1.1 s ease-out), Accept wobble */
        int a = t % 4 < 2;
        in_tick = t;
        tw_set(&in_p[0], a ? 1.35f : 0.9f, 1100, &EASE_OUT, now); tw_set(&in_po[0], a ? 0 : 1, 1100, &EASE_OUT, now);
        tw_set(&in_p[1], a ? 0.9f : 1.35f, 1100, &EASE_OUT, now); tw_set(&in_po[1], a ? 1 : 0, 1100, &EASE_OUT, now);
        tw_set(&in_shake, t % 8 == 0 ? -12.0f : t % 8 == 1 ? 12.0f : 0, 250, &EASE, now);
    }
    if (in_banner()) damage_d(BAN_X - 40, 0, 640, 180); else damage_d(ST_X, ST_Y, ST_W, ST_H);
}
static void in_draw(double now)
{
    const char *num = A.bt_call_num, *nm = pb_name_of(num);
    char ini[8], t[96];
    float k;
    if (!incoming()) return;
    if (!nm) nm = num[0] ? num : "Unknown";
    person_initial(ini, sizeof ini, nm);
    if (in_banner()) {   /* banner: avatar 72, name 26/800, "Incoming call · phone", Decline / Accept 76 */
        float x = BAN_X, y = BAN_Y, l1 = t_lh(800, 26), l2 = t_lh(600, 19), ty = y + 52 - (l1 + 2 + l2) * 0.5f;
        shadow(x, y, 560, 104, 52, 50, 0, 20, hexa(0, 0.4f));
        gfx_glass(D(x), D(y), D(560), D(104), D(52), HUD_TINT, HUD_A, GLASS_EDGE);
        rrect(x + 18, y + 16, 72, 72, 36, hexc(person_color(nm)));
        t_center(800, 30, x + 54, y + 52, ini, C_WHITE);
        t_fit(800, 26, x + 18 + 72 + 16, ty + l1 * 0.5f, 560 - 18 - 72 - 16 - 16 - 76 - 16 - 76 - 16, nm, C_TEXT);
        snprintf(t, sizeof t, tr("Incoming call \xC2\xB7 %s"), tr(bt_phone_name()));
        t_fit(600, 19, x + 18 + 72 + 16, ty + l1 + 2 + l2 * 0.5f, 560 - 18 - 72 - 16 - 16 - 76 - 16 - 76 - 16, t, C_SUB);
        k = press_k(B_IN_DEC, now);
        rrect(x + 560 - 16 - 76 - 12 - 76 + 38 - 38 * k, y + 52 - 38 * k, 76 * k, 76 * k, 38 * k, hexc(0xFF3B30));
        ico_rot("phone", x + 560 - 16 - 76 - 12 - 38, y + 52, 32 * k, C_WHITE, 135);
        k = press_k(B_IN_ACC, now);
        rrect(x + 560 - 16 - 38 - 38 * k, y + 52 - 38 * k, 76 * k, 76 * k, 38 * k, C_ACCENT);
        ico("phone", x + 560 - 16 - 38, y + 52, 32 * k, C_ONACCENT);
        return;
    }
    {   /* card: chip, avatar 132 with 150 circles, name 42, "Mobile · number", Decline / Accept 96 */
        float y = in_top(), cw = 18 + 20 + 10 + 18, cx = 692, l1 = t_lh(800, 42), l2 = t_lh(600, 23);
        int i;
        snprintf(t, sizeof t, tr("Incoming call \xC2\xB7 %s"), tr(bt_phone_name()));
        cw += t_width(700, 19, t);
        gfx_glass(D(ST_X), D(ST_Y), D(ST_W), D(ST_H), D(PANEL_R), HUD_TINT, HUD_A, GLASS_EDGE);
        rrect(cx - cw * 0.5f, y, cw, 40, 20, C_ACC_SOFT);
        ico("phone_w22", cx - cw * 0.5f + 18 + 10, y + 20, 20, C_ACCENT);
        t_left(700, 19, cx - cw * 0.5f + 18 + 20 + 10, y + 20, t, C_ACCENT);
        y += 40 + 34;
        for (i = 0; i < 2; i++) {
            float s = tw_get(&in_p[i], now) * 75, o = tw_get(&in_po[i], now);
            if (o > 0.01f) rrect(cx - s, y + 75 - s, s * 2, s * 2, s, col_alpha(C_ACC_SOFT, o));
        }
        rrect(cx - 66, y + 9, 132, 132, 66, hexc(person_color(nm)));
        t_center(800, 50, cx, y + 75, ini, C_WHITE);
        y += 150 + 22;
        t_center(800, 42, cx, y + l1 * 0.5f, nm, C_TEXT);
        y += l1 + 4;
        snprintf(t, sizeof t, "%s \xC2\xB7 %s", pb_kind_of(num), num);
        t_center(600, 23, cx, y + l2 * 0.5f, t, C_SUB);
        y += l2 + 44;
        k = press_k(B_IN_DEC, now);
        shadow(cx - 60 - 96 + 48 - 48 * k, y + 48 - 48 * k, 96 * k, 96 * k, 48 * k, 34, 0, 12, hexa(0xFF3B30, 0.45f));
        rrect(cx - 60 - 96 + 48 - 48 * k, y + 48 - 48 * k, 96 * k, 96 * k, 48 * k, hexc(0xFF3B30));
        ico_rot("phone", cx - 60 - 48, y + 48, 40 * k, C_WHITE, 135);
        k = press_k(B_IN_ACC, now);
        shadow(cx + 60 + 48 - 48 * k, y + 48 - 48 * k, 96 * k, 96 * k, 48 * k, 34, 0, 12, C_ACC_GLOW);
        rrect(cx + 60 + 48 - 48 * k, y + 48 - 48 * k, 96 * k, 96 * k, 48 * k, C_ACCENT);
        ico_rot("phone", cx + 60 + 48, y + 48, 40 * k, C_ONACCENT, tw_get(&in_shake, now));
        t_center(700, 20, cx - 60 - 48, y + 96 + 10 + t_lh(700, 20) * 0.5f, "Decline", C_SUB);
        t_center(700, 20, cx + 60 + 48, y + 96 + 10 + t_lh(700, 20) * 0.5f, "Accept", C_SUB);
    }
}

/* ================================================================= "Running app" window (launching a program from USB) */

void app_run(int i, double now)
{
    const lua_app *a = lua_apps_get(i);
    if (!a || rev_lock) return;                         /* reverse - camera only */
    scpy(A.app_name, sizeof A.app_name, a->name);
    A.app_run = 1; A.app_t = now;
    tw_set(&U.app_op, 1, 400, &EASE, now);
    tw_set(&U.app_k, 1, 550, &ENTER, now);
    tw_init(&U.app_launch, 1); tw_init(&U.app_prog, 0);
    layout_app();
    lua_app_start(i);                                   /* error - the window closes in update (HUD "Can't open") */
}

static void app_close(double now)
{
    A.app_run = 0;
    lua_app_stop();
    tw_set(&U.app_op, 0, 400, &EASE, now);
    tw_set(&U.app_k, 0, 550, &ENTER, now);
    layout_app();
}

/* program icon: icon.png (whole tile) or a color from the manifest + the design's default icon */
static void app_icon(const lua_app *a, float cx, float cy, float s, float r, const char *ic, float isz, float o)
{
    if (a && a->icon.id) gfx_image(&a->icon, D(cx - s * 0.5f), D(cy - s * 0.5f), D(s), D(s), 0, 0, 1, 1, hexa(0xFFFFFF, o));
    else {
        rrect(cx - s * 0.5f, cy - s * 0.5f, s, s, r, hexa(a ? a->color : 0x2DBE6C, o));
        ico(ic, cx, cy, isz, hexa(0xFFFFFF, o));
    }
}

/* program window fully open (opaque, fullscreen) - the screen beneath is neither updated nor drawn */
static int app_covers(double now) { return A.app_run && tw_get(&U.app_op, now) > 0.999f && tw_get(&U.app_k, now) > 0.999f; }

#define APP_HEAD 76                                     /* program window header; below it - the program canvas */

static void draw_app(double now)
{
    float op = tw_get(&U.app_op, now), k = 0.94f + 0.06f * tw_get(&U.app_k, now);
    float lo = tw_get(&U.app_launch, now), ro = 1 - lo;
    float x = ST_X, y = ST_Y, w = ST_W, h = ST_H;
    const lua_app *a = lua_apps_get(lua_app_running());
    if (op < 0.005f) return;
    gfx_xform(D(x + w * 0.5f), D(y + h * 0.5f), k, 0, op);
    shadow(x, y, w, h, PANEL_R, 80, 0, 30, hexa(0, 0.5f));
    rrect_b(x, y, w, h, PANEL_R, hexc(0x0B0C0F), hexa(0xFFFFFF, 0.12f));
    if (lo > 0.005f) {                                  /* launching: pulsing icon and progress */
        float ty = 336 - 108.2f, ps = U.app_pulse ? 1.06f : 0.97f;
        float s = 112 * ps;
        shadow(692 - s * 0.5f, ty + 56 - s * 0.5f, s, s, 30 * ps, 50, 0, 20, hexa(0, 0.4f * lo));
        app_icon(a, 692, ty + 56, s, 30 * ps, "bin_w18", 54 * ps, lo);
        t_center(800, 26, 692, ty + 112 + 16 + 15.6f, A.app_name, hexa(0xF5F5F7, lo));
        t_center(600, 20, 692, ty + 112 + 16 + 31.2f + 16 + t_lh(600, 20) * 0.5f, "Launching\xE2\x80\xA6", hexa(0xC7C7CC, lo));
        rrect(692 - 130, ty + 210.4f, 260, 6, 3, hexa(0xFFFFFF, 0.14f * lo));
        rrect(692 - 130, ty + 210.4f, 260 * tw_get(&U.app_prog, now), 6, 3, col_alpha(C_ACCENT, lo));
    }
    if (ro > 0.005f) {                                  /* running: program header and canvas */
        float bx = ST_X + ST_W - 16 - app_close_w(), bw = app_close_w(), bh = 50, by = ST_Y + 13, kk;
        rrect(x, y + APP_HEAD - 1, w, 1, 0, hexa(0xFFFFFF, 0.08f * ro));
        app_icon(a, x + 40, y + 38, 40, 11, "bin_w20", 22, ro);
        {
            float l1 = t_lh(800, 22), l2 = t_lh(700, 17), hy = y + 38 - (l1 + 2 + l2) * 0.5f;
            t_top(800, 22, x + 74, hy, A.app_name, hexa(0xF5F5F7, ro), 0);
            const lua_app *ra = lua_apps_get(lua_app_running());   /* from NAND - "builtin" (DEVIATIONS.md) */
            t_top(700, 17, x + 74, hy + l1 + 2, ra && !ra->usb ? "Lua script \xC2\xB7 builtin" : "Lua script \xC2\xB7 USB", hexa(0x32D74B, ro), 0);
        }
        kk = press_box(B_APP_CLOSE, &bx, &by, &bw, &bh, now);
        rrect_b(bx, by, bw, bh, 25 * kk, hexa(0xFFFFFF, 0.08f * ro), hexa(0xFFFFFF, 0.14f * ro));
        ico("close", bx + (20 + 9) * kk, by + bh * 0.5f, 18 * kk, hexa(0xF5F5F7, ro));
        t_left(700, 19, bx + (20 + 18 + 8) * kk, by + bh * 0.5f, "Close", hexa(0xF5F5F7, ro));
        gfx_xform_push(0, 0, 1, 0, ro);
        lua_app_draw(x, y + APP_HEAD, w, h - APP_HEAD, now);
        gfx_xform_pop();
    }
    gfx_xform_reset();
}

/* ================================================================= transitions, theme, wallpaper, brightness, volume */

static void rail_state(double now)
{
    int i;
    for (i = 0; i < 5; i++) {
        int on = U.screen == RAIL_SCREEN[i];
        tw_set(&U.r_op[i], on ? 1 : 0.5f, 350, &EASE, now);
        tw_set(&U.r_sc[i], on ? 1 : 0.88f, SPRING_MS, &SPRING, now);
        tw_set(&U.r_glow[i], on ? 1 : 0, SPRING_MS, &EASE, now);
        tw_set(&U.r_ind[i], on ? 1 : 0, SPRING_MS, &SPRING, now);
    }
    tw_set(&U.r_bg[0], U.screen == S_APPS ? 1 : 0, 350, &EASE, now);
    tw_set(&U.r_bg[1], U.screen == S_DASH ? 1 : 0, 350, &EASE, now);
}

void go_screen(int screen, double now)
{
    if (rev_lock && screen != S_CAMERA) return;
    if (screen == S_BT && A.bt_on && U.screen != S_BT) { A.searching = 1; A.n_extra = 0; bt_search(1); }   /* design: search on entry */
    if (screen == U.screen && U.phase != 2) return;
    U.pending = screen;
    U.phase = 2; U.phase_t = now;
    tw_set(&U.st_op, 0, LEAVE_MS, &EASE, now);
    tw_set(&U.st_s, 0.99f, LEAVE_MS, &EASE, now);
    tw_set(&U.st_dy, -10, LEAVE_MS, &EASE, now);
}

void close_screen(double now)
{
    int p = A.prev_screen;
    go_screen(p != U.screen && p >= 0 ? p : S_APPS, now);
}

static void draw_all(double now);
static int layer_dyn;                              /* the screen layer currently has the moving part baked in too (a transition is running) */

static int transitioning(double now)
{
    if (U.screen == S_APPS || press_active(now) || A.vfull) return 0;
    return U.phase != 1 || tw_active(&U.st_op, now) || tw_active(&U.st_s, now) || tw_active(&U.st_dy, now);
}

static void begin_fade(double now, float ms)
{
    static double fade_at = -1;
    if (U.fading && fade_at == now) { tw_set(&U.fade, 0, ms, &EASE, now); return; }   /* already snapshotted in this frame */
    fade_at = now;
    gfx_layer_begin(GFX_L_SNAP);
    draw_all(now);
    gfx_layer_end();
    tw_init(&U.fade, 1);
    tw_set(&U.fade, 0, ms, &EASE, now);
    U.fading = 1;
}

static void load_wallpaper(void)
{
    char path[512];
    gfx_tex t;
    if (A.scene == SCENE_CUSTOM) {
        /* photo from USB (copy in the data partition; if absent, from the stick): the theme dimming (c.overlay) is applied to the pixels on
         * load - for both the wallpaper and the blur */
        col ov = A.light ? hexa(0xFFFFFF, 0.48f) : hexa(0, 0.28f);
        if (media_wallpaper_file(&t, ov) && media_wallpaper(&t, A.custom_img, ov)) return;   /* neither exists - keep the previous */
    } else {
        snprintf(path, sizeof path, "%s/bg_%s%s.png", assets, SCENE_FILE[A.scene], A.light ? "_light" : "");
        if (gfx_tex_load(&t, path, 0)) return;
    }
    gfx_tex_free(&wall);
    wall = t;
    gfx_blur_params(PAL.glass_blur, PAL.glass_sat);   /* theme glass (design c.blur) */
    gfx_set_wallpaper(&wall);
}


void set_theme(int light, double now)
{
    if (light == A.light) return;
    begin_fade(now, 500);
    A.light = light;
    theme_set(light);
    load_wallpaper();
    tw_set(&U.theme_rot, light ? 360 : 0, 700, &SPRING, now);
    base_dirty_f = screen_dirty_f = 1;
}

/* v30 Auto theme: night by the clock (19:00..07:00, as in the design) or headlights on (0x16 byte 17 bit 2; logic only,
 * no headlights in the UI - owner) -> dark, otherwise light */
void theme_auto_apply(double now)
{
    time_t t = time(NULL);
    struct tm tm;
    int dark;
    if (!A.theme_auto) return;
    localtime_r(&t, &tm);
    dark = tm.tm_hour >= 19 || tm.tm_hour < 7 || (mcu_ok() && mcu_st()->ill == 1);
    set_theme(!dark, now);
}

/* accent color (v17): changes everywhere at once - redraw everything */
void set_accent(int k)
{
    if (k == theme_accent) return;
    theme_set_accent(k);
    base_dirty_f = screen_dirty_f = 1;
    full = 1;
}

void set_scene(int scene, double now)
{
    if (scene == A.scene) return;
    begin_fade(now, 900);
    A.scene = scene;
    load_wallpaper();
    base_dirty_f = screen_dirty_f = 1;
}

void set_custom_wallpaper(int img, double now)
{
    begin_fade(now, 900);
    media_wallpaper_save(img);                          /* copy to the data partition: if the stick is removed, the wallpaper stays */
    A.scene = SCENE_CUSTOM; A.custom_img = img;
    load_wallpaper();
    base_dirty_f = screen_dirty_f = 1;
    hud_show(6, NULL, now);
}

/* brightness is the hardware backlight via MCU (0x11 [04, 1..10]); on the PC (no MCU) just the slider */
void set_bright(int b)
{
    b = b < 1 ? 1 : b > 10 ? 10 : b;
    if (b == A.bright) return;
    A.bright = b;
    mcu_set_brightness(b);
}

void ui_mcu_ready(void) { mcu_set_brightness(A.bright); }

void set_volume(int v, double now)
{
    A.vol = v < 0 ? 0 : v > 31 ? 31 : v;
    A.muted = 0;
    hud_show(0, NULL, now);
}

void toggle_muted(double now) { A.muted = !A.muted; hud_show(0, NULL, now); }

/* call volume 0..25 (like the Phone slider in Sound), HUD "Phone volume" - steering wheel buttons during a call */
static void set_phone_volume(int v, double now)
{
    A.phone_vol = v < 0 ? 0 : v > 25 ? 25 : v;
    hud_show(12, NULL, now);
}

#define CLIM_OFF_A 0.35f                       /* like unavailable items in the design (Speed alarm, source in Music) */
/* without ignition (owner) everything is off: A/C, fan Off, recirculation, no temperature or airflow mode selected */
static void climate_state(double now)
{
    int i, on = U.clim_live, bl = on && U.blower;
    tw_set(&U.ac_fill, on && U.ac ? 1 : 0, 300, &EASE, now);
    for (i = 0; i < 16; i++) {
        tw_set(&U.seg_h[i], on && i == U.temp ? 30 : 14, 400, &SPRING, now);
        tw_set(&U.seg_op[i], on && i == U.temp ? 1 : 0.4f, 300, &EASE, now);
    }
    for (i = 0; i < 7; i++) {                           /* bar i+1: lit if the fan is running and i+1 <= fan */
        tw_set(&U.fan_h[i], bl && i + 1 == U.fan ? 30 : 14, 400, &SPRING, now);
        tw_set(&U.fan_on[i], bl && i + 1 <= U.fan ? 1 : 0, 300, &EASE, now);
    }
    tw_set(&U.blower_bg, (float)bl, 300, &EASE, now);
    tw_set(&U.recirc_bg, on && U.recirc ? 1 : 0, 300, &EASE, now);
    tw_set(&U.air_x, U.air * (float)AIR_STEP, SPRING_MS, &SPRING_SOFT, now);
    tw_set(&U.air_pill, (float)on, 300, &EASE, now);
    tw_set(&U.clim_a, on ? 1 : CLIM_OFF_A, 300, &EASE, now);
    for (i = 0; i < 5; i++) tw_set(&U.air_ic[i], on && i == U.air ? 1 : 0, 300, &EASE, now);
}

static struct { int known, on, fan, ac, air, temp, recirc; double sent; } ct;   /* climate: what the MCU reported */
static int ac_btn, ac_req;
/* Screen off (design v27, Standby; standby.c): the knob (power icon) - MCU 0x02 [01] (the MCU handles backlight and sound,
 * we answer 0x02 like the stock one), [00] - back. The "Wake screen" button (v28) wakes: MCU 0x03 [00] (like ShellApp).
 * Appearance: screen 0.8 s, blobs 1.5 s (up to 0.85), clock 1.2 s; fully in Standby - 15 frames/s. Music from USB is paused. */
static void mcu_resend_source(void);
static int scr_off, sb_resume;
static tween sb_op, sb_glow, sb_clk;
static double sb_down, sb_frame, sb_t0;               /* sb_t0 - when Screen off appeared */
static int sb_wake;

static void standby_set(int on, double now)
{
    if (on == scr_off) return;
    scr_off = on;
    tw_set(&sb_op, on ? 1 : 0, 800, &EASE, now);
    tw_set(&sb_glow, on ? 0.85f : 0, 1500, &EASE, now);
    tw_set(&sb_clk, on ? 1 : 0, 1200, &ENTER, now);
    if (on) settings_flush(A.playing && A.src_usb);     /* sleep - power may be cut */
    if (on) { sb_t0 = now; standby_palette(); sb_resume = A.playing; A.playing = 0; press_id = drag_id = scroll_id = -1; }
    else { if (sb_resume) A.playing = 1; sb_resume = 0; mcu_resend_source(); }   /* woke up - re-send the MCU audio source */
    sb_down = 0; full = 1;
    fprintf(stderr, "hu: screen %s\n", on ? "off" : "on");
}

static void click_core(int id, double now)
{
    if (id >= B_RAIL0 && id < B_RAIL0 + 5) go_screen(RAIL_SCREEN[id - B_RAIL0], now);
    else if (id == B_APPS) go_screen(S_APPS, now);
    else if (id == B_DASH) go_screen(S_DASH, now);
    else if (id == B_AC) {                                /* v20: turning A/C on turns the fan on too */
        if (!U.ac) { U.ac = 1; U.blower = 1; if (U.fan < 1) U.fan = 1; } else U.ac = 0;
        ac_btn = U.ac; ac_req = 1;
    }
    else if (id == B_COLD) { if (U.temp > 0) U.temp--; hud_show(1, NULL, now); }
    else if (id == B_HOT) { if (U.temp < 15) U.temp++; hud_show(1, NULL, now); }
    else if (id == B_FAN_DN) {                            /* v20: down to 0 - fan and A/C off */
        if (!U.blower) return;
        if (U.fan - 1 <= 0) { U.fan = 0; U.blower = 0; U.ac = 0; } else U.fan--;
        hud_show(8, NULL, now);
    } else if (id == B_FAN_UP) {
        U.fan = U.blower ? (U.fan < 7 ? U.fan + 1 : 7) : (U.fan < 1 ? 1 : U.fan);
        U.blower = 1; if (U.air >= 3) U.ac = 1;              /* windshield airflow - A/C */
        hud_show(8, NULL, now);
    } else if (id == B_BLOWER) {
        if (U.blower) { U.blower = 0; U.ac = 0; } else { U.blower = 1; if (U.fan < 1) U.fan = 1; if (U.air >= 3) U.ac = 1; }
        hud_show(8, NULL, now);
    }
    else if (id == B_RECIRC) { U.recirc = !U.recirc; hud_show(9, NULL, now); }
    else if (id >= B_AIR0 && id < B_AIR0 + 5) {             /* v20: windshield modes (3, 4) - A/C and fan */
        int was = U.air;
        U.air = id - B_AIR0;
        if (U.air >= 3) { U.ac = 1; U.blower = 1; if (U.fan < 1) U.fan = 1; }
        else if (was >= 3 && !ac_btn) U.ac = 0;            /* from a windshield mode - A/C off unless it was turned on with the button */
        hud_show(2, NULL, now);
    }
    else if (id == B_THEME) {                       /* cycles Auto -> Dark -> Light -> Auto (owner; the design has only Dark/Light) */
        if (A.theme_auto) { A.theme_auto = 0; set_theme(0, now); }
        else if (!A.light) set_theme(1, now);
        else { A.theme_auto = 1; theme_auto_apply(now); }
        base_dirty_f = screen_dirty_f = 1;          /* the icon changed even if the theme is the same (Light -> Auto during the day) */
    }
    else if (id == B_IN_DEC) bt_reject();
    else if (id == B_IN_ACC) { bt_answer(); if (U.screen != S_PHONE) go_screen(S_PHONE, now); }
    else if (id == B_APP_CLOSE) app_close(now);
    climate_state(now);
}

/* dragging across the temperature and fan bars (input range in the design: 0..15 and 0..7 over the full width) */
static void core_drag(int id, float x, double now)
{
    if (id == B_TEMP_DRAG) {
        int v = (int)lroundf(fminf(1, fmaxf(0, (x - (tp_x + 53)) / (tp_w - 106))) * 15);
        if (v != U.temp) { U.temp = v; hud_show(1, NULL, now); }
    } else if (id == B_FAN_DRAG) {
        int v = (int)lroundf(fminf(1, fmaxf(0, (x - (fan_x + FAN_AX)) / (fan_w - FAN_AX - 57))) * 7);
        if (v != (U.blower ? U.fan : 0)) {
            U.fan = v; U.blower = v > 0;
            if (!v) U.ac = 0; else if (U.air >= 3) U.ac = 1;     /* v20 */
            hud_show(8, NULL, now);
        }
    }
    climate_state(now);
}

/* ================================================================= events */

static int inside(int id, float x, float y)
{
    return hit[id].w > 0 && x >= hit[id].x && x < hit[id].x + hit[id].w && y >= hit[id].y && y < hit[id].y + hit[id].h;
}

static void screen_drag(int id, float x, float y, int phase, double now)
{
    if (id >= HIT_FS && id < HIT_FS + 10) { video_fs_drag(id, x, y, phase, now); return; }
    if (id < HIT_SCREEN) { if (phase < 2) core_drag(id, x, now); return; }
    if (SCREENS[U.screen]->drag) SCREENS[U.screen]->drag(id, x, y, phase, now);
}

void ui_event(const pf_event *ev, double now)
{
    float x = ev->x / K, y = ev->y / K;
    int i;
    if (scr_off) {                                 /* Screen off: only the "Wake screen" button */
        int in = x >= sb_btn[0] && x < sb_btn[0] + 72 && y >= sb_btn[1] && y < sb_btn[1] + 72;
        if (ev->type == PF_DOWN) sb_down = in ? now : 0;
        else if (ev->type == PF_UP && sb_down > 0 && in) sb_wake = 1;
        if (ev->type == PF_UP) sb_down = 0;
        return;
    }
    if (aa_view_covers()) { aa_view_touch(ev->type, x, y); return; }   /* Android Auto fullscreen: touches go to the phone */
    if (ev->type == PF_DOWN) {
        press_id = drag_id = scroll_id = -1; scrolling = 0;
        if (U.phase != 1) return;                        /* touches are ignored during a transition */
        down_x = x; down_y = y; down_t = now;
        if (incoming()) {                                /* an incoming call covers the screen (on the Camera - only the banner) */
            if (inside(B_IN_ACC, x, y)) press_id = B_IN_ACC;
            else if (inside(B_IN_DEC, x, y)) press_id = B_IN_DEC;
            if (press_id >= 0) { anim_id = press_id; tw_init(&press_tw, 1); tw_set(&press_tw, PRESS_SCALE, PRESS_MS, &EASE, now); return; }
            if (in_banner() ? (x >= BAN_X && x < BAN_X + 560 && y >= BAN_Y && y < BAN_Y + 104)
                            : (x >= ST_X && x < ST_X + ST_W && y >= ST_Y && y < ST_Y + ST_H)) return;
        }
        if (A.app_run) {                                 /* the program window covers the screen */
            if (inside(B_APP_CLOSE, x, y)) press_id = B_APP_CLOSE;
            else if (x < ST_X) return;                   /* the side panel doesn't switch screens beneath the window */
            else if (x >= ST_X && y < ST_Y + ST_H && y >= ST_Y) {
                if (y >= ST_Y + APP_HEAD && tw_get(&U.app_launch, now) < 0.5f) { app_touch = 1; lua_app_touch(0, x, y, now); }
                return;
            }
        }
        if (A.vfull) {                                   /* fullscreen video covers everything */
            for (i = HIT_FS + 9; i >= HIT_FS; i--) {
                if (!inside(i, x, y)) continue;
                if (hit[i].type == HT_DRAG) drag_id = i; else press_id = i;
                break;
            }
            if (drag_id >= 0) { screen_drag(drag_id, x, y, 0, now); return; }
            if (press_id >= 0) { anim_id = press_id; tw_init(&press_tw, 1); tw_set(&press_tw, PRESS_SCALE, PRESS_MS, &EASE, now); }
            return;
        }
        if (press_id < 0)
            for (i = HIT_MAX - 1; i >= 0; i--) {
                if (!inside(i, x, y)) continue;
                if (hit[i].type == HT_SCROLL) { if (scroll_id < 0) scroll_id = i; continue; }
                if (hit[i].type == HT_DRAG) { drag_id = i; break; }
                if (press_id < 0) press_id = i;
                if (scroll_id >= 0 || i < HIT_SCREEN) break;
            }
        if (press_id >= 0 && scroll_id < 0)              /* scroll area "under" a button - keep searching */
            for (i = press_id - 1; i >= HIT_SCREEN; i--) if (hit[i].type == HT_SCROLL && inside(i, x, y)) { scroll_id = i; break; }
        if (drag_id >= 0) { screen_drag(drag_id, x, y, 0, now); return; }
        if (press_id >= 0) {
            anim_id = press_id; tw_init(&press_tw, 1); tw_set(&press_tw, PRESS_SCALE, PRESS_MS, &EASE, now);
            if (A.key_tone && !A.muted) mcu_beep();          /* key click - the sound is made by the MCU */
        }
    } else if (ev->type == PF_MOVE) {
        if (app_touch) { lua_app_touch(1, x, y, now); return; }
        if (drag_id >= 0) screen_drag(drag_id, x, y, 1, now);
        else if (scrolling) screen_drag(scroll_id, x, y, 1, now);
        else if (scroll_id >= 0 && (fabsf(y - down_y) > 8 || fabsf(x - down_x) > 8)) {
            if (press_id >= 0) cancel_press(now);        /* this is a scroll, not a press */
            scrolling = 1;
            screen_drag(scroll_id, down_x, down_y, 0, now);
            screen_drag(scroll_id, x, y, 1, now);
        }
    } else if (ev->type == PF_UP) {
        if (app_touch) { app_touch = 0; if (A.app_run) lua_app_touch(2, x, y, now); return; }
        if (drag_id >= 0) { screen_drag(drag_id, x, y, 2, now); drag_id = -1; return; }
        if (scrolling) { screen_drag(scroll_id, x, y, 2, now); scrolling = 0; scroll_id = -1; return; }
        if (press_id >= 0) {
            int id = press_id;
            press_id = -1;
            tw_set(&press_tw, 1, PRESS_MS, &EASE, now);
            if (inside(id, x, y)) {
                if (id >= HIT_FS && id < HIT_FS + 10) video_fs_click(id, now);
                else if (id < HIT_SCREEN) click_core(id, now);
                else if (SCREENS[U.screen]->click) SCREENS[U.screen]->click(id, now);
            }
        }
    }
}

/* ================================================================= now playing (USB / Bluetooth) */

pb_contact *PB;
pb_call *REC;

/* name by number: compare by the last 9 digits (+995 5.. / 5..) */
static void tail9(char *o, const char *n)
{
    char d[32];
    int k = 0, l;
    for (; *n && k < 31; n++) if (*n >= '0' && *n <= '9') d[k++] = *n;
    d[k] = 0; l = k > 9 ? k - 9 : 0;
    snprintf(o, 16, "%.15s", d + l);
}
const char *pb_name_of(const char *num)
{
    char a[16], b[16];
    int i, j;
    if (!num || !num[0]) return NULL;
    tail9(a, num);
    if (strlen(a) < 6) return NULL;
    for (i = 0; i < A.n_cont; i++)
        for (j = 0; j < PB[i].nnum; j++) { tail9(b, PB[i].num[j]); if (!strcmp(a, b)) return PB[i].name; }
    return NULL;
}

const char *pb_kind_of(const char *num)
{
    char a[16], b[16];
    int i, j;
    tail9(a, num ? num : "");
    for (i = 0; i < A.n_cont && strlen(a) >= 6; i++)
        for (j = 0; j < PB[i].nnum; j++) {
            tail9(b, PB[i].num[j]);
            if (!strcmp(a, b)) return PB[i].kind[j] == 'h' ? "Home" : PB[i].kind[j] == 'w' ? "Work" : "Mobile";
        }
    return "Mobile";
}
/* avatar colors - the design's people palette; for real contacts we pick by name (DEVIATIONS) */
uint32_t person_color(const char *name)
{
    static const uint32_t PAL_P[9] = { 0xFF4F6A, 0x7B61FF, 0x2DBE6C, 0x2F7BF6, 0xFF8A2A, 0x14A3A3, 0xE0458F, 0xC2410C, 0xFFB020 };
    unsigned h = 5381;
    if (!name || !name[0] || (name[0] >= '0' && name[0] <= '9') || name[0] == '+') return 0x5E5E66;   /* number without a name - gray */
    for (; *name; name++) h = h * 33 + (unsigned char)*name;
    return PAL_P[h % 9];
}
void person_initial(char *out, size_t n, const char *name)
{
    size_t k = 1;
    if (!name || !name[0] || (name[0] >= '0' && name[0] <= '9') || name[0] == '+') { snprintf(out, n, "#"); return; }
    while (name[k] && ((unsigned char)name[k] & 0xC0) == 0x80) k++;          /* whole UTF-8 character */
    if (k >= n) k = n - 1;
    memcpy(out, name, k); out[k] = 0;
    if (k == 1 && out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
}

int music_bt_av(void) { return A.bt_on && A.bt_conn[0]; }
int music_usb_av(void) { return A.usb_conn && NTRACKS > 0; }
int music_any(void) { return music_bt_av() || music_usb_av(); }
const char *bt_phone_name(void)
{
    int i;
    for (i = 0; i < A.n_paired; i++) if (!strcmp(A.paired[i].id, A.bt_conn)) return A.paired[i].name;
    return "phone";
}
static int bt_idle(void) { return !A.src_usb && music_bt_av() && (!A.bt_av || !A.bt_title[0]); }   /* no track from the phone */
int music_dash_play(void) { np_t n = np_now(0); return A.m_loaded && n.av && !n.idle; }

/* what is playing in Android Auto (owner 2026-10-26): while a session is active and the phone has sent a track - instead of the Bluetooth track
 * (AVRCP is often silent during AA), control is by AA keys. The cover is a texture from aa_np_art */
static aa_np_t anp;
static gfx_tex anp_art;
/* the card is the master: it controls whatever is playing. The AA track - when the phone is selected and AA audio is playing, or AA is wireless
 * (the same phone on Bluetooth), or Bluetooth is silent; wired AA + music from another phone over Bluetooth - Bluetooth
 * (owner 2026-10-26) */
static int anp_on(void) { return !A.src_usb && aa_np(&anp) && anp.song[0] && (A.aa_media || aa_wireless() || !(A.bt_av && A.bt_playing)); }
gfx_tex *np_aa_art(void)
{
    int w, h;
    unsigned char *px = aa_np_art(&w, &h);
    if (px) { gfx_tex_free(&anp_art); gfx_tex_rgba(&anp_art, w, h, px); free(px); screen_dirty_f = 1; }
    return anp_on() && anp_art.id ? &anp_art : NULL;
}

np_t np_now(double now)
{
    np_t n;
    n.live = 0;
    n.av = A.src_usb ? music_usb_av() : music_bt_av();
    if (anp_on()) {                                       /* track from Android Auto */
        static char t[160], a[96], al[96];
        snprintf(t, sizeof t, "%s", anp.song); snprintf(a, sizeof a, "%s", anp.artist); snprintf(al, sizeof al, "%s", anp.album);
        n.av = 1; n.idle = 0; n.t = t; n.a = a; n.al = al; n.len = anp.dur > 0 ? anp.dur : 1; n.playing = anp.state == 2;
        n.pos = anp.pos + (n.playing && anp.pos_t > 0 ? pf_now() - anp.pos_t : 0);
        if (n.pos > n.len) n.pos = n.len;
        n.hue = 0xFF4F6A; n.id = 200000 + anp.seq;
        n.live = anp.dur <= 0;                              /* live stream (radio app): no duration - LIVE instead of time */
        return n;
    }
    n.idle = n.av && bt_idle();
    if (n.idle) {   /* v15 btIdle: "Nothing playing", play - start on the phone */
        static char sub[96];
        snprintf(sub, sizeof sub, tr("Start music on %s, or tap play"), tr(bt_phone_name()));
        n.t = "Nothing playing"; n.a = sub; n.al = ""; n.len = 1; n.pos = 0; n.playing = 0; n.hue = 0; n.id = -3;
        return n;
    }
    if (!n.av) {   /* v14: source not connected - placeholder, time "–:––" */
        int none = !music_any();
        int empty = A.src_usb && A.usb_conn;                    /* v15: stick present, no music */
        n.t = none && !A.usb_conn ? "No music source" : !A.src_usb ? "No phone connected" : empty ? "No music on USB drive" : "No USB drive";
        n.a = none && !A.usb_conn ? "Connect a phone or insert a USB drive" : !A.src_usb ? "Connect a phone over Bluetooth"
              : empty ? "This drive has no audio files" : "Insert a USB drive with music";
        n.al = ""; n.len = 1; n.pos = 0; n.playing = 0; n.hue = 0; n.id = -1 - A.src_usb;
        return n;
    }
    if (A.src_usb) {
        const track_t *t = trk_at(A.trk);
        n.t = t->t; n.a = t->a; n.al = t->al; n.len = t->len; n.pos = A.pos; n.playing = A.playing; n.hue = t->hue; n.id = A.trk;
    } else {   /* phone: the position arrives once a second - between events it advances by itself */
        n.t = A.bt_title; n.a = A.bt_artist; n.al = A.bt_album; n.len = A.bt_len; n.playing = A.bt_av && A.bt_playing;
        n.pos = A.bt_pos + (n.playing && A.bt_pos_t > 0 ? now - A.bt_pos_t : 0);
        if (n.len > 0 && n.pos > n.len) n.pos = n.len;
        n.hue = 0xFF4F6A; n.id = 100000 + A.bt_seq;
        n.live = A.bt_title[0] && A.bt_len <= 0;           /* live stream: the phone gave no duration */
    }
    if (n.len < 1) n.len = 1;
    return n;
}
void np_play(double now)
{
    if (anp_on()) { aa_key(0x55, 1); aa_key(0x55, 0); A.m_loaded = 1; return; }   /* play/pause in AA */
    if (A.src_usb) A.playing = !A.playing;
    else { if (!A.bt_playing) { A.radio_on = 0; A.vplaying = 0; } bt_media(A.bt_playing ? 2 : 1); }   /* BT turned itself on */
    A.m_loaded = 1;
    (void)now;
}
void np_next(double now)
{
    (void)now;
    if (anp_on()) { aa_view_key(1); return; }
    if (A.src_usb) { A.trk = next_track(); A.pos = 0; } else bt_media(3);
    screen_dirty_f = 1;
}
void np_prev(double now)
{
    (void)now;
    if (anp_on()) { aa_view_key(-1); return; }
    if (!A.src_usb) bt_media(4);
    else if (A.pos > 3) np_seek(0);
    else { A.trk = prev_track(); A.pos = 0; }
    screen_dirty_f = 1;
}
void np_mode(void) { if (!A.src_usb) bt_media_mode(A.shuffle, A.repeat); }
/* source change: pause the other player */
void np_source(int usb, double now)
{
    (void)now;
    if (usb == A.src_usb) return;
    if (usb && A.bt_playing) bt_media(2);
    if (!usb) A.playing = 0;
    if (!usb && A.bt_av && A.bt_playing) { A.radio_on = 0; A.vplaying = 0; }   /* BT selected and the phone is already playing - listen to it */
    A.src_usb = usb;
    base_dirty_f = screen_dirty_f = 1;
}

/* ================================================================= a single audio source (owner's request)
 * Radio on -> USB and BT pause; USB starts -> radio off, BT pause; call -> radio off (back on after the call),
 * USB pause (resume after the call). The phone itself pauses BT music for a call.
 * The phone is audible only when Bluetooth is selected (like the stock HU, owner 2026-10-23): when the phone starts playing by itself (music,
 * notification, navigation - all of them send AVRCP "playing") we switch nothing; the phone itself ducks for prompts.
 * BT is enabled explicitly: play in the BT player / choosing Bluetooth in Music (np_play, np_source). */
#define DUCK_VOL 0.20                           /* USB stick under the phone sound (owner: 20%) */
#define DUCK_S 0.3

static void audio_policy(double now)
{
    static int p_bt = -1, p_usb = -1, p_radio = -1, p_vid = -1, in_call, usb_was, radio_was, vid_was;
    int usb = A.playing && A.src_usb && A.usb_conn, call = A.bt_call != 0;
    int vid = A.vplaying && A.usb_conn, bt = A.bt_av && A.bt_playing;
    (void)now;
    if (p_bt < 0) { p_bt = bt; p_usb = usb; p_radio = A.radio_on; p_vid = vid; }
    if (call != in_call) {
        in_call = call;
        if (call) { usb_was = usb; radio_was = A.radio_on; vid_was = vid; A.playing = 0; A.radio_on = 0; A.vplaying = 0; }   /* visible in the UI too */
        else {
            if (usb_was && A.usb_conn) A.playing = 1;
            if (radio_was) A.radio_on = 1;
            if (vid_was && A.usb_conn) A.vplaying = 1;
            usb_was = radio_was = vid_was = 0;
        }
        p_radio = A.radio_on; p_usb = A.playing && A.src_usb && A.usb_conn; p_vid = A.vplaying && A.usb_conn;   /* this is not "turned on by itself" */
        base_dirty_f = screen_dirty_f = 1;
    }
    {   /* Android Auto: when AA music starts - pause our own; when ours is started - AA loses sound (pause on the phone).
         * We don't touch Bluetooth music with wireless AA: it is the same phone and the same player - it moved the sound from
         * A2DP to AA itself, and an AVRCP pause stopped its song (in the car 2026-10-26) */
        static int p_aa;
        int aa = A.aa_media;
        if (aa && !p_aa) { A.radio_on = 0; A.playing = 0; A.vplaying = 0; if (A.bt_playing && !aa_wireless()) bt_media(2); base_dirty_f = screen_dirty_f = 1; }
        else if (aa && ((A.radio_on && !p_radio) || (A.playing && A.src_usb && A.usb_conn && !p_usb) || (vid && !p_vid))) aa_audio_loss();
        p_aa = aa;
    }
    if (!call) {
        if (bt && !p_bt) { if (!A.src_usb) A.m_loaded = 1; }   /* the phone itself - leave the source alone */
        else if (A.playing && A.src_usb && A.usb_conn && !p_usb) { A.radio_on = 0; A.vplaying = 0; if (A.bt_playing) bt_media(2); }
        else if (A.radio_on && !p_radio) { A.playing = 0; A.vplaying = 0; if (A.bt_playing) bt_media(2); }
        else if (vid && !p_vid) { A.radio_on = 0; A.playing = 0; if (A.bt_playing) bt_media(2); }   /* video with sound - only one at a time */
    }
    p_bt = bt; p_usb = A.playing && A.src_usb && A.usb_conn; p_radio = A.radio_on; p_vid = A.vplaying && A.usb_conn;
    {   /* we don't mute phone sound (A2DP): GStreamer (USB stick) and nFore play into the same i.MX output - navigation and notifications are
         * audible over the USB stick music (owner 2026-10-23; not with radio - the audio processor has a different input). mute
         * may be left in the module from last time - on phone connection, "unmute" once */
        static int muted = -1, av;
        int m = 0;
        if (A.bt_av != av) { av = A.bt_av; muted = -1; }       /* reconnected - redo */
        if (A.bt_av && m != muted) { muted = m; bt_a2dp_mute(m); }
    }
    {   /* ducking (owner): the phone is playing something (A2DP stream open - navigation, notification) - USB stick and video get quieter,
         * smoothly over DUCK_S; when it closes (Android holds the stream for another 3-5 s) - back */
        static double v = 1, t;
        double target = A.bt_stream && A.bt_av ? DUCK_VOL : 1, step = t > 0 ? (now - t) / DUCK_S * (1 - DUCK_VOL) : 1;
        static int was = -1;
        t = now;
        if (v < target) v = fmin(target, v + step); else if (v > target) v = fmax(target, v - step);
        player_volume(v);
        if ((target < 1) != was) { was = target < 1; fprintf(stderr, "hu: duck %s\n", was ? "on (phone audio)" : "off"); }
    }
}

/* ================================================================= MCU turned off the screen and sound (0x04 [01]: ignition off /
 * door opened, owner): write the track and position once, pause everything (nothing to play with - the amplifier is de-energized);
 * on again (0x04 [02]) - resume what was playing. We leave the radio alone: the MCU turned the tuner off. */
static void power_sync(double now)
{
    static int off, usb, vid, bt, left;
    int o = mcu_ok() && mcu_st()->off;
    (void)now;
    {   /* left the car: the MCU turned off the screen (0x04 01) with power off (0x50 = 0) - AA says goodbye (ByeBye), the access point
         * goes down (aa_wifi_done), no new wireless sessions until the car is switched on (owner 2026-10-25) */
        int l = o && mcu_st()->power == 0;
        if (l != left) {
            static int bt_was;
            left = l;
            aa_wifi_pause(l);
            if (l && aa_state() != AA_IDLE) { fprintf(stderr, "hu: aa: car off - bye-bye\n"); aa_view_shutdown(); }
            /* and Bluetooth: disconnect the phone (gently - we don't shut the stack down, bt_power), and reconnect when the car is switched on,
             * it will connect the last phone. Only if it was on */
            if (l) { bt_was = A.bt_on; if (bt_was) { fprintf(stderr, "hu: bt: car off - disconnect\n"); bt_power(0); } }
            else if (bt_was) { bt_was = 0; fprintf(stderr, "hu: bt: car on - reconnect\n"); bt_power(1); }
        }
    }
    if (o == off) return;
    off = o;
    if (o) {
        settings_shutdown((A.playing || sb_resume) && A.src_usb);
        usb = A.playing; vid = A.vplaying; bt = A.bt_av && A.bt_playing;
        A.playing = 0; A.vplaying = 0;
        if (bt) bt_media(2);
    } else {
        settings_shutdown_cancel();
        if (usb && A.usb_conn) A.playing = 1;
        if (vid && A.usb_conn) A.vplaying = 1;
        if (bt && A.bt_av) bt_media(1);
        usb = vid = bt = 0;
    }
    fprintf(stderr, "hu: power %s\n", o ? "off (0x04 01) - paused, track saved" : "on (0x04 02)");
    base_dirty_f = screen_dirty_f = 1;
}

/* ================================================================= USB player (media.c): A.trk / A.playing -> GStreamer */
static int pl_open = -1, pl_run, pl_gen = -1;
static double pl_seek = -1;                       /* seek while the track is not open - apply when the player is ready */
static int pl_seek_trk = -1;
int audio_loaded(void) { return !A.src_usb || pl_open >= 0; }   /* USB: track is open in the player (otherwise time shows -:--) */

static void usb_player(double now)
{
    int want = A.src_usb && A.usb_conn && A.playing && NTRACKS > 0;
    (void)now;
    if (media_gen() != pl_gen) { pl_gen = media_gen(); pl_open = -1; pl_run = 0; }   /* lists changed */
    if (!NTRACKS || !A.usb_conn) { if (pl_open >= 0) player_stop(); pl_open = -1; pl_run = 0; return; }
    if (want && pl_open != A.trk) {                              /* another track - open it */
        if (player_open(TRACKS[A.trk].path) == 0) {
            pl_open = A.trk; pl_run = 1;
            if (pl_seek_trk != A.trk) pl_seek = -1;         /* the deferred seek was for another track */
            A.pos = pl_seek >= 0 ? pl_seek : 0;
        }
        else { A.playing = 0; return; }
    } else if (pl_open >= 0 && want != pl_run) { player_pause(!want); pl_run = want; }
    if (pl_open == A.trk && pl_run) {
        double p = player_pos(), d;
        if (pl_seek >= 0) { if (p >= 0) { player_seek(pl_seek); pl_seek = -1; } }   /* player ready - seek */
        else if (p >= 0) A.pos = p;
        if (TRACKS[A.trk].len <= 0 && (d = player_dur()) > 0) TRACKS[A.trk].len = (int)d;   /* duration not from tags */
        if (player_eos()) {                                       /* finished - next (repeat - the same from the start) */
            if (A.repeat) { player_seek(0); A.pos = 0; }
            else { A.trk = next_track(); A.pos = 0; screen_dirty_f = 1; }
        }
    }
}
void np_seek(double sec)
{
    double len = trk_at(A.trk)->len;
    if (!A.src_usb) return;
    if (len > 1 && sec > len - 1) sec = len - 1;      /* length unknown (0) - no limit */
    if (sec < 0) sec = 0;
    A.pos = sec;
    if (pl_open == A.trk) player_seek(sec);
    else { pl_seek = sec; pl_seek_trk = A.trk; fprintf(stderr, "hu: audio: seek %.1f pending (open %d, trk %d)\n", sec, pl_open, A.trk); }
}

/* ================================================================= video from the USB stick (player.c): A.vsel / A.vplaying / A.vpos -> GStreamer.
 * Screens change A.vpos themselves (seek, slider) - here it shows up as a jump and goes to the player. */
static int vp_open = -1, vp_run, vp_gen = -1;
static double vp_last, vp_away, vp_seek_t, vp_target;   /* seek: when and where to (the GStreamer position is still old) */                                   /* vp_away - when we left the Video screen (0 - still on it) */

int video_loaded(void) { return vp_open >= 0; }
static void video_end(double now);

static void usb_video(double now)
{
    {   /* PC test (no VPU there and the video does not open): HU_TEST_VEND=sec - "finished watching" after that many seconds */
        static double t0;
        if (!t0) t0 = now;
        if (getenv("HU_TEST_VEND") && t0 > 0 && now - t0 > atof(getenv("HU_TEST_VEND"))) { t0 = -1; vp_open = A.vsel; video_end(now); }
    }
    int want = A.vplaying && A.usb_conn && NVIDEOS > 0;
    (void)now;
    if (media_gen() != vp_gen) { vp_gen = media_gen(); if (vp_open >= 0) vplayer_stop(); vp_open = -1; vp_run = 0; }
    if (!NVIDEOS || !A.usb_conn) { if (vp_open >= 0) vplayer_stop(); vp_open = -1; vp_run = 0; return; }
    if (vp_open >= 0 && vp_open != A.vsel && !want) { vplayer_stop(); vp_open = -1; vp_run = 0; }   /* picked another one while paused */
    if (want && vp_open != A.vsel) {                             /* another video - open it (from position A.vpos) */
        if (VIDEOS[A.vsel].w > 1920 || VIDEOS[A.vsel].h > 1088) {   /* VPU i.MX6DL: up to 1920x1088 (4K crashes the decoder) */
            A.vplaying = 0; hud_show(7, VIDEOS[A.vsel].name, now); return;
        }
        if (vplayer_open(VIDEOS[A.vsel].path) == 0) { vp_open = A.vsel; vp_run = 1; vp_last = 0; }
        else { A.vplaying = 0; hud_show(7, VIDEOS[A.vsel].name, now); return; }
    } else if (vp_open >= 0 && want != vp_run) { vplayer_pause(!want); vp_run = want; }
    if (vp_open != A.vsel) return;
    vplayer_poll(now);
    if (U.screen != S_VIDEO && !A.vfull) {                       /* left the screen: pause, after 5 min - shut down */
        if (!vp_away) { vp_away = now; if (A.vplaying) { A.vplaying = 0; vplayer_pause(1); vp_run = 0; } }
        else if (now - vp_away > 300) { vplayer_stop(); vp_open = -1; vp_away = 0; return; }
    } else vp_away = 0;
    if (A.vdrag) {}                                               /* slider is being dragged - wait for release */
    else if (fabs(A.vpos - vp_last) > 1.5) {                      /* seeked in the UI */
        vplayer_seek(A.vpos); vp_last = vp_target = A.vpos; vp_seek_t = now;
        fprintf(stderr, "hu: video: seek %.1f\n", A.vpos);
    } else if (vp_run) {
        double p = vplayer_pos(), d;
        if (p >= 0 && (now - vp_seek_t > 1.5 || fabs(p - vp_target) < 2)) {   /* after a seek - not the old position */
            if (vp_seek_t > 0 && now - vp_seek_t < 3) { fprintf(stderr, "hu: video: after seek %.1f -> %.1f\n", vp_target, p); vp_seek_t = -10; }
            A.vpos = vp_last = p;
        }
        if (VIDEOS[A.vsel].len <= 0 && (d = vplayer_dur()) > 0) { VIDEOS[A.vsel].len = (int)d; screen_dirty_f = 1; }
    }
    if (vplayer_unsupported()) {                                  /* no hardware decoder for this format */
        A.vplaying = 0; vp_run = 0; A.vpos = 0;
        vplayer_stop(); vp_open = -1;
        hud_show(7, VIDEOS[A.vsel].name, now);
        screen_dirty_f = 1;
        return;
    }
    {   /* fallback end indicator: 1.5 s without frames, playing, position near the end (EOS from GStreamer may not arrive) */
        const video_t *v = &VIDEOS[A.vsel];
        int end_eos = vplayer_eos(), end_idle = !end_eos && vp_run && v->len > 0 && A.vpos >= v->len - 1 && vplayer_idle(now) > 1.5;
        if (end_idle) fprintf(stderr, "hu: video: end without EOS (pos %.1f / %d)\n", A.vpos, v->len);
        if (end_eos || end_idle) video_end(now);
    }
}

/* finished (or error) - close: empty player, position 0, leave fullscreen */
static void video_end(double now)
{
    A.vplaying = 0; vp_run = 0; A.vpos = vp_last = 0;
    vplayer_close(); vp_open = -1;
    if (A.vfull) { A.vfull = 0; video_fs_layout(now); damage_full(); }
    screen_dirty_f = 1;
}

/* ================================================================= frame */

/* design tick 250 ms: equalizer, Bluetooth (connection, pairing, visibility, search), program window */
static void tick(double now)
{
    int i;
    U.tick++;
    int pl = np_now(now).playing;
    for (i = 0; i < 4; i++) tw_set(&A.eq[i], pl ? 4 + (float)(rand() % 15) : 3, 250, &EASE, now);
    if (A.disc && now - A.disc_t >= 180) { A.disc = 0; bt_discoverable(0); base_dirty_f = screen_dirty_f = 1; }   /* design: visibility 3 min */
    if (A.app_run) {
        double ael = now - A.app_t;
        U.app_pulse = (int)(ael / 0.75) % 2;
        if (ael >= 1.5) tw_set(&U.app_launch, 0, 350, &EASE, now);
        tw_set(&U.app_prog, (float)fmin(1, ael / 1.5), 250, &LINEAR, now);
        damage_d(ST_X - 40, ST_Y - 40, ST_W + 80, ST_H + 80);
    }
}

/* ================================================================= sound and radio <-> MCU (NOTES_mcu.md)
 * MCU state -> UI (at startup and after every change); UI -> MCU only what changed.
 * Until the MCU has sent its state we send nothing - so as not to overwrite the real values with our demo ones. */
static struct { int init, vol, phone, def, bass, treble, middle, mute, speed, src, radio, am, fm, amf; double t0, sent, region_sent; } tx;
static int tuner_seen, tuner_warned;                  /* MCU sent the current station (0x30) - the radio can be turned on */
static void mcu_resend_source(void) { tx.src = 0; }

static int fm_units(float f) { return (int)lroundf(f * radio_fm_div()); }     /* MHz -> MCU units (region) */
void radio_region_sent(void) { tx.fm = fm_units(A.fm_f); tx.amf = (int)lroundf(A.am_f); tx.region_sent = U.last_now; }

/* climate <-> MCU: frame 0x51 -> UI (panel knobs - with the design's HUD); screen buttons change U.* by the
 * design's rules, and the MCU receives only the difference via 0x5b commands. Until the MCU has sent climate we send nothing (PC: demo).
 * In windshield modes the car turns A/C on and off by itself (unless A/C was turned on with the button) - we send it only on a button press. */

static void climate_sync(mcu_state *m, double now)
{
    int on = U.blower && U.fan > 0, sent = 0;
    if (m->clim_new) {
        m->clim_new = 0;
        if (!ct.known || now - ct.sent >= 0.6) {             /* we just changed it ourselves - wait, the MCU reply lags */
            int was = ct.known;
            if (was && m->temp != U.temp + 1) hud_show(1, NULL, now);
            else if (was && (m->clim_on != U.blower || (m->clim_on && m->fan != U.fan))) hud_show(8, NULL, now);
            else if (was && m->clim_on && m->air != U.air) hud_show(2, NULL, now);
            else if (was && m->recirc != U.recirc) hud_show(9, NULL, now);
            U.blower = m->clim_on && m->fan > 0; if (m->fan > 0) U.fan = m->fan;
            if (m->ac != ct.ac && m->air == ct.air) ac_btn = m->ac;   /* A/C changed without a mode change - a panel button */
            U.ac = m->ac && U.blower; if (m->air < 5) U.air = m->air; U.recirc = m->recirc;   /* design: airflow off - A/C off */
            if (m->temp >= 1 && m->temp <= 16) U.temp = m->temp - 1;
            climate_state(now); base_dirty_f = screen_dirty_f = 1;
        }
        ct.on = m->clim_on; ct.fan = m->fan; ct.ac = m->ac; ct.air = m->air; ct.temp = m->temp; ct.recirc = m->recirc;
        ct.known = 1;
        on = U.blower && U.fan > 0;
    }
    if (!ct.known) return;
    if (!on) { if (ct.on) { mcu_climate(MCU_CLIM_OFF); ct.on = 0; sent = 1; } }
    else {
        if (!ct.on) { mcu_climate(MCU_CLIM_ON); ct.on = 1; sent = 1; }
        if (U.fan != ct.fan) { mcu_climate(MCU_CLIM_FAN | U.fan); ct.fan = U.fan; sent = 1; }
        if (U.air != ct.air) { mcu_climate_mode(U.air); ct.air = U.air; sent = 1; }
        if (ac_req && U.ac != ct.ac) { mcu_climate(U.ac ? MCU_CLIM_AC_ON : MCU_CLIM_AC_OFF); sent = 1; }
        ct.ac = U.ac; ac_req = 0;                           /* the car handles the rest of A/C itself */
    }
    if (U.recirc != ct.recirc) { mcu_climate(U.recirc ? MCU_CLIM_RECIRC : MCU_CLIM_FRESH); ct.recirc = U.recirc; sent = 1; }
    if (U.temp + 1 != ct.temp) { mcu_climate_temp(U.temp + 1); ct.temp = U.temp + 1; sent = 1; }
    if (sent) ct.sent = now;
}

/* car settings (Vehicle screen): we show what the MCU reports (0x50 bits, 0x52), except for ~2 s after our edit */
static void vehicle_sync(mcu_state *m, double now)
{
    if (!m->veh_new) return;
    m->veh_new = 0;
    if (now - A.veh_touch < 2) return;
    if (m->lock_fb >= 0) A.lock_lh = m->lock_fb == 1 ? 0 : 1;   /* 1=headlights only -> "Lights only", otherwise "Lights + Horn" */
    if (m->auto_light_c >= 0) A.auto_light = m->auto_light_c;
    if (m->drl_c >= 0) A.drl = m->drl_c;
    if (m->bl >= 0) A.backlight = m->bl;                        /* instrument backlight from 0x53 */
    if (m->spd_alarm_c >= 0) {                                  /* speed threshold (0x52): the car reflects its own value here */
        A.speed_alarm = m->spd_alarm_c;
        if (m->spd_alarm_c && m->spd_idx_c > 0) {
            int km = m->spd_idx_c * 5 + 25;
            A.speed_limit = km < 30 ? 30 : km > 130 ? 130 : km;
        }
    }
}

static void mcu_sync(double now)
{
    mcu_state *m = mcu_st();
    if (!mcu_ok()) return;
    climate_sync(m, now);
    vehicle_sync(m, now);
    /* while we are changing the sound ourselves (and 0.6 s after) we ignore MCU replies: they lag and made the sliders jump */
    if (m->audio_new && now - tx.sent < 0.6) m->audio_new = 0;
    if (m->audio_new) {
        m->audio_new = 0;
        if (!(A.muted && m->media == 0)) A.vol = m->media;     /* with Mute the MCU volume is 0 - ours stays */
        A.phone_vol = m->phone; A.def_vol = m->def; A.speed_vol = m->speed;
        A.eqv[0] = m->bass; A.eqv[1] = m->middle; A.eqv[2] = m->treble;
        tx.vol = A.muted ? 0 : A.vol; tx.phone = A.phone_vol; tx.def = A.def_vol; tx.mute = A.muted; tx.speed = A.speed_vol;
        tx.bass = m->bass; tx.middle = m->middle; tx.treble = m->treble;
        tx.init = 1;
    }
    if (m->region_new) {                           /* the region is stored by the MCU; while we wait for a reply to our 0x36 - don't touch */
        m->region_new = 0;
        if (now - tx.region_sent > 1.5 && m->region != A.region) radio_set_region(m->region);
    }
    if (m->tuner_new) {
        m->tuner_new = 0;
        tuner_seen = 1;
        float f = m->am ? (float)m->freq : m->freq / (float)radio_fm_div();
        if (A.scanning && !m->scanning) A.found_known[m->am] = 1;      /* scan finished */
        A.scanning = m->scanning; A.seeking = m->seeking;
        A.scan_n = m->scanning && m->idx > 0 ? m->idx - 1 : 0;
        A.scan_f = f;
        A.am = m->am;
        if (!m->scanning) {                        /* during a scan the "current" one stays as it was (design), scan_f runs */
            if (m->am) A.am_f = f; else A.fm_f = f;
            tx.fm = fm_units(A.fm_f); tx.amf = (int)lroundf(A.am_f);
        }
        tx.am = A.am;
    }
    if (m->lists_new) {                            /* favorites and found: MCU units -> MHz / kHz */
        int b, i;
        m->lists_new = 0;
        for (b = 0; b < 2; b++) {
            A.nfav[b] = m->nfav[b]; A.nfound[b] = m->nfound[b];
            for (i = 0; i < m->nfav[b]; i++) A.fav[b][i] = b ? (float)m->fav[b][i] : m->fav[b][i] / (float)radio_fm_div();
            for (i = 0; i < m->nfound[b]; i++) A.found[b][i] = b ? (float)m->found[b][i] : m->found[b][i] / (float)radio_fm_div();
            if (m->nfound[b]) A.found_known[b] = 1;
        }
    }
    if (!tx.init) {                                /* MCU silent for more than 2 s - take our values as is */
        if (tx.t0 == 0) tx.t0 = now;
        if (now - tx.t0 < 2) return;
        tx.init = 1; tx.vol = -1; tx.def = -1; tx.bass = 99; tx.speed = -1;
    }
    {   /* Mute: the mute flag in 0x21 does not silence the sound (checked in the car) - the MCU gets volume 0 */
        int v = A.muted ? 0 : A.vol;
        if (v != tx.vol || A.phone_vol != tx.phone) { tx.vol = v; tx.phone = A.phone_vol; mcu_set_volumes(v, A.phone_vol); tx.sent = now; }
    }
    if (A.def_vol != tx.def) { tx.def = A.def_vol; mcu_set_default_volume(A.def_vol); tx.sent = now; }
    if (A.eqv[0] != tx.bass || A.eqv[1] != tx.middle || A.eqv[2] != tx.treble || A.muted != tx.mute) {
        tx.bass = A.eqv[0]; tx.middle = A.eqv[1]; tx.treble = A.eqv[2]; tx.mute = A.muted;
        mcu_set_eq(A.eqv[0], A.eqv[2], A.eqv[1], A.muted);
        tx.sent = now;
    }
    if (A.speed_vol != tx.speed) { tx.speed = A.speed_vol; mcu_set_speed_volume(A.speed_vol); tx.sent = now; }
    /* MCU audio source (TEF6635 audio processor input): follows what is playing; codes are as ShellApp sends them (mcu.h).
     * USB - by the stick port (/media/usb1-.. / usb2-..). If nothing is playing - keep the previous one (0x0F "off" mutes
     * the HU music too), except with the radio off - tuner stop. Voice processor - as ShellApp */
    {
        /* radio - only once the MCU has sent the current station (0x30, in the startup batch): otherwise our tuner (default
         * 101.5) overwrote the MCU station. If it hasn't arrived within 5 s (frame missed) - as before */
        int usb2 = A.usb_mnt[10] != '1', radio_ok = tuner_seen || now - tx.t0 > 5;
        if (!tuner_seen && radio_ok && A.radio_on && !tuner_warned) { tuner_warned = 1; fprintf(stderr, "hu: radio: no 0x30 from MCU in 5 s\n"); }
        static int boot_src;
        if (!boot_src) {                                   /* after startup we don't stay silent (owner 2026-10-26, like the stock one): if there is nothing to
                                                            * resume - radio, on whatever station it was (the MCU remembers it, 0x30) */
            boot_src = 1;
            if (!A.radio_on && !A.playing && !A.vplaying && !settings_resume_usb() && !(A.bt_av && A.bt_playing) && !A.bt_call
                && !A.aa_conn) { A.radio_on = 1; fprintf(stderr, "hu: start: nothing to resume - radio\n"); }
        }
        int want = A.bt_call ? MCU_SRC_PHONE
                 : A.aa_cm ? MCU_SRC_CP_PHONE                       /* AA voice session - like a CarPlay call: hands-free */
                 : A.aa_mic ? MCU_SRC_SIRI                          /* AA assistant is listening - like Siri on the stock one */
                 : A.aa_voice && !A.aa_navi ? MCU_SRC_USB1_AUDIO    /* AA prompt / reply (with radio - mix-in, leave the source alone) */
                 : A.aa_media && !A.radio_on ? MCU_SRC_USB1_AUDIO   /* AA - i.MX output */
                 : A.radio_on && !radio_ok ? 0                       /* waiting for the station from the MCU - leave the source alone */
                 : A.radio_on ? MCU_SRC_RADIO
                 : A.vplaying && A.usb_conn ? (usb2 ? MCU_SRC_USB2_VIDEO : MCU_SRC_USB1_VIDEO)
                 : A.playing && A.src_usb && A.usb_conn ? (usb2 ? MCU_SRC_USB2_AUDIO : MCU_SRC_USB1_AUDIO)
                 : !A.src_usb && A.bt_av ? MCU_SRC_BT_AUDIO   /* Bluetooth selected - everything from the phone is audible, even when paused */
                 : tx.src == MCU_SRC_RADIO || !tx.src ? MCU_SRC_OFF : tx.src;
        /* we always send the source - both at startup and after waking (0x04 02), even if nothing is playing (then Off), like
         * the stock one: without 0x15 the MCU didn't send climate, instrument panel or clock (0x51-0x53) - neither the batch nor changes from the buttons; once
         * 0x15 arrived - after 0.45 s the batch and then live changes (car log 2026-10-26, logs/car/climate_ok_1613) */
        {
            static int was_off;
            int off = mcu_st()->off;
            if (was_off && !off) tx.src = 0;                  /* woke up - source again */
            was_off = off;
        }
        /* DSP mode - as ShellApp SourceControler::update1388Mode: call 6, Siri 7, otherwise 8; CarPlay call - 16 kHz
         * hands-free (mode 1, set by AppleCarPlay): Gemini Live talks and listens at once - the DSP suppresses echo */
        {   /* AA prompt over radio - MCU navigation mix-in, like CarPlay: on for the whole AA connection (the path is not
             * toggled - otherwise the radio dipped at the end of every prompt), on a prompt only the source is ducked.
             * Mix-in volume is the same as media (0..31 -> 1..10) */
            static int p_on, p_duck, p_vol = -1;
            int nv = A.muted ? 0 : (A.vol * 10 + 15) / 31, on = A.aa_conn, duck = on && A.aa_navi;
            if (nv < 1 && !A.muted) nv = 1;
            if (on != p_on || duck != p_duck || (on && nv != p_vol)) {
                if (on || p_on) mcu_navi_mix(on, nv, duck);
                fprintf(stderr, "hu: navi mix %d duck %d vol %d\n", on, duck, nv);
                p_on = on; p_duck = duck; p_vol = nv;
            }
        }
        if (want != tx.src && want) {
            tx.src = want; mcu_source(want);
            fm1388_set_mode(want == MCU_SRC_PHONE ? FM1388_BT_CALL : want == MCU_SRC_CP_PHONE ? FM1388_CM16K
                          : want == MCU_SRC_SIRI ? FM1388_SIRI : FM1388_DEFAULT);
            tx.radio = want == MCU_SRC_RADIO;
            if (tx.radio) { tx.am = -1; tx.fm = tx.amf = -1; }
        }
    }
    if (tx.radio) {
        if (A.am != tx.am) { tx.am = A.am; mcu_band(A.am); tx.fm = tx.amf = -1; }
        if (!A.am && fm_units(A.fm_f) != tx.fm) { tx.fm = fm_units(A.fm_f); mcu_tune(0, tx.fm); }
        if (A.am && (int)lroundf(A.am_f) != tx.amf) { tx.amf = (int)lroundf(A.am_f); mcu_tune(1, tx.amf); }
    }
}

/* ================================================================= car: steering wheel buttons (NOTES_mcu.md)
 * Volume is as in the UI (Volume HUD; during a call - phone volume), when held - a step every 0.2 s. Mute - volume 0 in the MCU (mcu_sync).
 * Previous/next: short press (released within 0.5 s) - station (as on the Radio screen) or track; held - seek in radio,
 * seek in the USB player. Mode - cycles the source: radio -> USB -> phone (unavailable ones are skipped), with the source screen.
 * Home - Dashboard, Phone - Phone screen, knob - volume (panel). Voice assistant - not done (owner). Reverse - camera_sync. */
static void car_mode(double now)
{
    int cur = A.radio_on ? 0 : A.src_usb ? 1 : 2, k;
    for (k = 1; k <= 3; k++) {
        int s = (cur + k) % 3;
        if (s == 1 && !music_usb_av()) continue;
        if (s == 2 && !music_bt_av()) continue;
        if (s == 0) { A.radio_on = 1; go_screen(S_RADIO, now); }
        else {
            A.radio_on = 0; np_source(s == 1, now);
            if (s == 1) A.playing = 1; else bt_media(1);
            A.m_loaded = 1;
            go_screen(S_MUSIC, now);
        }
        break;
    }
    base_dirty_f = screen_dirty_f = 1;
}

static void car_volume(int d, double now)             /* call - phone volume, otherwise media */
{
    if (A.bt_call) set_phone_volume(A.phone_vol + d, now);
    else set_volume(A.vol + d, now);
}

static int car_btn(int d, int held)                    /* button of the current screen for the steering wheel, -1 - none */
{
    const screen_def *sd = SCREENS[U.screen];
    if (U.phase == 2 || A.app_run || !sd->hw_btn) return -1;
    if (A.vfull) A.vctl_t = U.last_now;                 /* fullscreen video: show the controls */
    return sd->hw_btn(d, held);
}

static void car_input(double now)
{
    static int vol_held, skip_key, skip_long, skip_btn = -1;   /* held volume (+1/-1); previous/next */
    static double vol_t, skip_t0, skip_t, skip_pos;             /* skip_pos - where we have already seeked to (the player position lags) */
    mcu_state *m = mcu_st();
    int i;
    for (i = 0; i < m->nkeys; i++) {
        int c = m->keys[i] & 0xFF, up = (m->keys[i] & MCU_KEY_UP) != 0;
        switch (c) {
        case MCU_KEY_VOL_UP: case MCU_KEY_VOL_DOWN:
            if (!up) car_volume(c == MCU_KEY_VOL_UP ? 1 : -1, now);
            break;
        case MCU_KEY_VOL_UP_LONG: case MCU_KEY_VOL_DOWN_LONG:   /* the step was already made by the press */
            vol_held = up ? 0 : c == MCU_KEY_VOL_UP_LONG ? 1 : -1; vol_t = now;
            break;
        case MCU_KEY_MUTE: if (!up) toggle_muted(now); break;
        case MCU_KEY_NEXT_LONG: case MCU_KEY_PREV_LONG:    /* held 2 s: the release will then arrive with this code */
            c = c == MCU_KEY_NEXT_LONG ? MCU_KEY_NEXT : MCU_KEY_PREV;
            if (!up) break;
            /* fall through */
        case MCU_KEY_NEXT: case MCU_KEY_PREV: {
            int d = c == MCU_KEY_NEXT ? 1 : -1;
            if (aa_view_covers()) {                      /* Android Auto: radio is playing - stations (owner), otherwise - to the phone */
                if (up) { if (A.radio_on && !A.aa_media) radio_jump(d); else aa_view_key(d); }
                break;
            }
            if (!up) {
                skip_key = c; skip_long = 0; skip_t0 = now;
                skip_btn = car_btn(d, 0);
                if (skip_btn >= 0) hw_press(skip_btn, 1, now);
            } else if (skip_key == c) {
                if (skip_btn >= 0) {
                    hw_press(skip_btn, 0, now);
                    if (!skip_long) SCREENS[U.screen]->click(skip_btn, now);   /* like a tap on the button */
                } else if (!skip_long) {
                    if (A.radio_on) radio_jump(d);
                    else if (music_any()) { if (d > 0) np_next(now); else np_prev(now); }
                }
                skip_key = 0; skip_btn = -1;
            }
            break;
        }
        case MCU_KEY_MODE: if (!up) car_mode(now); break;
        case MCU_KEY_VOICE: if (!up && !aa_view_voice()) fprintf(stderr, "hu: mcu: voice key (no Android Auto)\n"); break;
        case MCU_KEY_HOME: if (!up) { if (aa_view_covers()) aa_view_exit(); go_screen(S_DASH, now); } break;   /* from AA to the shell */
        case MCU_KEY_PHONE: if (!up) { if (aa_view_covers()) aa_view_phone(); else go_screen(S_PHONE, now); } break;
        default: if (!up) fprintf(stderr, "hu: mcu: key %02x\n", c); break;
        }
    }
    m->nkeys = 0;
    if (m->knob) { car_volume(m->knob, now); m->knob = 0; }   /* panel volume knob */
    if (m->clk_new) {                                   /* HU clock follows the car clock (MCU 0x53), the date is left alone */
        time_t t = time(NULL);
        struct tm tm;
        long diff;
        m->clk_new = 0;
        localtime_r(&t, &tm);
        diff = (long)(m->clk_h * 3600 + m->clk_m * 60 + m->clk_s) - (long)(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
        if (diff > 43200) diff -= 86400; else if (diff < -43200) diff += 86400;
        if (diff > 60 || diff < -60) {
            struct timeval tv = { t + diff, 0 };
            if (settimeofday(&tv, NULL) == 0) { fprintf(stderr, "hu: clock %02d:%02d:%02d from car\n", m->clk_h, m->clk_m, m->clk_s); base_dirty_f = 1; }
        }
    }
    if (m->user_off_new) { m->user_off_new = 0; standby_set(m->user_off, now); }
    if (sb_wake) { sb_wake = 0; if (scr_off) { mcu_user_off(0); standby_set(0, now); } }
    if (vol_held && now - vol_t >= 0.2) { vol_t = now; car_volume(vol_held, now); }
    if (skip_key && now - skip_t0 >= 0.5 && now - skip_t >= 0.3) {   /* previous/next held: seek / search */
        int d = skip_key == MCU_KEY_NEXT ? 1 : -1, b = car_btn(d, 1);
        skip_t = now;
        if (b >= 0) {                                   /* the screen has a seek button (Video: -10/+10 s) */
            if (b != skip_btn) { if (skip_btn >= 0) hw_press(skip_btn, 0, now); skip_btn = b; hw_press(b, 1, now); }
            SCREENS[U.screen]->click(b, now);
        } else if (A.radio_on) { if (!skip_long && !A.scanning) { A.seek_dir = d; mcu_seek(d > 0); } }
        else if (A.src_usb && music_usb_av()) {
            if (!skip_long) skip_pos = A.pos;
            skip_pos += d * 10;
            if (skip_pos < 0) skip_pos = 0;
            np_seek(skip_pos);
        }
        skip_long = 1;
    }
}

/* ================================================================= rear-view camera (cam.c)
 * Reverse (GPIO 84, held 0.8 s - passing through R into D doesn't count) - Camera screen, after R - back. Camera screen without R (button) - as ShellApp: MCU 0x18 [01],
 * when leaving - [00]. Capture stays open while the Camera screen is visible (+3 s). No good frames for 4 s - restart with a reset of
 * the decoder (up to 2 times); if it didn't open or there is still no picture on R - we close it and the stock cameradetect returns
 * (the camera on R won't be lost), retry no sooner than 10 s later. */
static void camera_sync(double now)
{
    static int rev, rev_back = -1, manual, restarts, bl_max, aa_back;
    static double away_t, fail_t = -100, open_t, raw_t, r_t;
    int raw = cam_reverse(now), r, want, mx;
    /* PRND lever: going from P to D passes through R - show the camera only if R is held 0.8 s; leaving R - immediately */
    if (!raw) raw_t = 0; else if (raw_t == 0) raw_t = now;
    r = raw && now - raw_t >= 0.8;
    if (r != rev) {
        rev = r;
        rev_lock = 0;                                   /* R released - the screen may be restored; R engaged - below */
        r_t = now;
        fprintf(stderr, "hu: reverse %d\n", r);
        if (r && A.app_run) app_close(now);           /* the program window would cover the camera */
        if (r && aa_view_covers()) { aa_view_exit(); aa_back = 1; }   /* fullscreen AA would cover the camera: take the screen for ourselves */
        if (r && U.screen != S_CAMERA) { rev_back = U.screen; go_screen(S_CAMERA, now); }
        else if (!r && rev_back >= 0) { if (U.screen == S_CAMERA) go_screen(rev_back, now); rev_back = -1; }
        if (!r && aa_back) { aa_back = 0; aa_view_resume(); }    /* R released - screen back to AA */
        rev_lock = r;
    }
    want = r || (U.screen == S_CAMERA && U.phase != 2) || (U.phase == 2 && U.pending == S_CAMERA);
    if ((want && !r) != manual) { manual = want && !r; mcu_camera(manual); }
    /* owner: on R the screen goes to maximum once the camera is on screen (transition finished and a frame arrived; if no frame - after
     * 2 s), after R is released - back to our own brightness (A.bright is untouched) */
    mx = r && U.screen == S_CAMERA && U.phase != 2 && ((cam_active() && cam_last_frame() >= r_t) || now - r_t > 2);
    if (mx != bl_max) { bl_max = mx; mcu_set_brightness(mx ? 10 : A.bright); }
    if (want) {
        away_t = 0;
        if (!cam_active() && now - fail_t > 10) {
            if (cam_open() < 0) fail_t = now; else open_t = now;
        }
        if (cam_active() && now - open_t > 4 && now - (cam_last_frame() > 0 ? cam_last_frame() : open_t) > 4) {
            if (restarts < 2) {                              /* no good frames for 4 s - restart with a decoder reset */
                fprintf(stderr, "hu: cam: no picture - restart %d\n", ++restarts);
                cam_stop(0);
                if (cam_open() < 0) fail_t = now; else open_t = now;
            } else if (r) {                                  /* still nothing on R - hand the camera to the stock one */
                fprintf(stderr, "hu: cam: no picture on reverse - back to stock camera\n");
                cam_close(); fail_t = now; restarts = 0;
            }
        }
        if (cam_last_frame() > open_t) restarts = 0;
    } else if (cam_active()) {
        if (away_t == 0) away_t = now;
        else if (now - away_t > 3) cam_close();
    }
}

static void update(double now)
{
    time_t tt = time(NULL);
    struct tm tm;
    int i, bat;
    U.last_now = now;
    mcu_sync(now);
    car_input(now);
    camera_sync(now);
    {   /* Auto theme: headlights - immediately on the MCU signal; clock (day/night) - once a second */
        static double t_auto;
        static int last_ill = -2;
        int ill = mcu_ok() ? mcu_st()->ill : -1;
        if (ill != last_ill || now - t_auto >= 1) { last_ill = ill; t_auto = now; theme_auto_apply(now); }
    }
    if (A.lang != i18n_lang()) {                   /* Vehicle › Language: whole UI again (string lengths differ) */
        i18n_set_lang(A.lang);
        full = 1; base_dirty_f = screen_dirty_f = 1; layout_again();
    }

    if (U.phase == 2 && (now - U.phase_t) * 1000 >= LEAVE_MS) {
        A.prev_screen = U.screen;
        U.screen = U.pending; U.phase = 0; U.phase_t = now; screen_dirty_f = 1;
        tw_set(&U.st_op, 0, 0, &LINEAR, now); tw_set(&U.st_s, 0.98f, 0, &LINEAR, now); tw_set(&U.st_dy, 24, 0, &LINEAR, now);
        rail_state(now);
        if (SCREENS[U.screen]->enter) SCREENS[U.screen]->enter(now);
        layout_again();
    } else if (U.phase == 0 && (now - U.phase_t) * 1000 >= 40) {
        U.phase = 1; U.phase_t = now;
        tw_set(&U.st_op, 1, ENTER_FADE_MS, &EASE, now);
        tw_set(&U.st_s, 1, ENTER_MOVE_MS, &ENTER, now);
        tw_set(&U.st_dy, 0, ENTER_MOVE_MS, &ENTER, now);
    }
    if (U.phase != 1 || tw_active(&U.st_op, now) || tw_active(&U.st_s, now) || tw_active(&U.st_dy, now))
        damage_d(ST_X, 0, ST_W, 672);

    {   /* cover: 1 playing / 0.9 paused - for any source (a pause may come from the phone) */
        static int last_pl = -1;
        int pl = np_now(now).playing;
        if (pl != last_pl) { if (last_pl >= 0) tw_set(&A.art_sc, pl ? 1 : 0.9f, 700, &SPRING, now); last_pl = pl; }
    }
    usb_player(now);
    usb_video(now);
    if ((now - U.tick_t) * 1000 >= TICK_MS) { U.tick_t = now; tick(now); }

    localtime_r(&tt, &tm);
    if (tm.tm_min != U.last_min) { U.last_min = tm.tm_min; base_dirty_f = 1; }
    net_poll(now);
    media_poll(now);
    usb_poll(now);
    bt_poll(now);
    audio_policy(now);
    settings_tick(now, (A.playing || sb_resume) && A.src_usb);
    power_sync(now);
    in_layout();
    in_update(now);
    bat = conn_dev() != NULL;
    if (bat != U.last_bat) { U.last_bat = bat; base_dirty_f = 1; }

    for (i = 0; i < 5; i++)
        if (tw_active(&U.r_op[i], now) || tw_active(&U.r_sc[i], now) || tw_active(&U.r_glow[i], now) || tw_active(&U.r_ind[i], now))
            damage_d(0, 131 + i * 86 - 40, RAIL_W, RAIL_BTN + 90);
    if (any_active(U.r_bg, 2, now)) damage_d(0, 590, RAIL_W, 150);
    if (tw_active(&U.ac_fill, now)) damage_d(ac_x, bb_y, ac_w, PILL_H);
    if (any_active(U.seg_h, 16, now) || any_active(U.seg_op, 16, now)) damage_d(tp_x, bb_y, tp_w, PILL_H);
    if (any_active(U.fan_h, 7, now) || any_active(U.fan_on, 7, now) || tw_active(&U.blower_bg, now)) damage_d(fan_x, bb_y, fan_w, PILL_H);
    if (tw_active(&U.recirc_bg, now)) damage_d(rc_x, bb_y, 84, PILL_H);
    if (tw_active(&U.air_x, now) || any_active(U.air_ic, 5, now) || tw_active(&U.air_pill, now)) damage_d(air_x0, bb_y, AIR_W, PILL_H);
    if (tw_active(&U.clim_a, now)) damage_d(ac_x, bb_y, air_x0 + AIR_W - ac_x, PILL_H);
    {   /* climate works only with ignition: 0x50 bits 0-1 = 0 off, 1 ACC (fans stopped), 2 ignition, 3 starter
         * (Start/Stop button: ACC -> ignition -> off, owner); without MCU - as in the design */
        int p = mcu_ok() ? mcu_st()->power : -1, live = p < 0 || p >= 2;   /* -1 - not known yet: don't flicker at startup */
        A.car_live = live;                                 /* and the Vehicle screen (scr_vehicle.c) */
        if (live != U.clim_live) {
            U.clim_live = live;
            fprintf(stderr, "hu: climate %s (power %d)\n", live ? "live" : "off", p);
            layout_core(); climate_state(now);
        }
    }
    if (tw_active(&U.theme_rot, now)) damage_d(th_x, bb_y, 60, PILL_H);
    if (press_active(now)) { hitbox r = hit[anim_id]; damage_d(r.x - 40, r.y - 40, r.w + 80, r.h + 80); }

    if (U.hud_shown && (now - U.hud_t) * 1000 >= TICK_MS * HUD_TICKS) {
        U.hud_shown = 0;
        tw_set(&U.hud_op, 0, 350, &EASE, now);
        tw_set(&U.hud_k, 0, SPRING_MS, &SPRING, now);
    }
    if (tw_active(&U.hud_op, now) || tw_active(&U.hud_k, now) || U.hud_shown) damage_d(692 - 300 - 50, 0, 700, 140);   /* v33: HUD up to 600 + shadow */
    if (tw_active(&U.app_op, now) || tw_active(&U.app_k, now) || tw_active(&U.app_launch, now)) damage_d(ST_X - 40, 0, ST_W + 80, 700);
    if (A.app_run) {                                     /* Lua program: crashed / closed by itself, redraw */
        int f = lua_app_failed();
        if (f) { if (f == 1) hud_show(7, A.app_name, now); app_close(now); }
        else if (lua_app_busy(now)) damage_d(ST_X, ST_Y + APP_HEAD, ST_W, ST_H - APP_HEAD);
    }

    video_fs_update(now);
    aa_view_update(now);
    if (U.fading) { damage_full(); if (!tw_active(&U.fade, now)) U.fading = 0; }
    {   /* HU_BENCH: flips through the screens by itself every 1.5 s (measurements on the HU without touches) */
        static double bt;
        static int bench = -1;
        if (bench < 0) bench = getenv("HU_BENCH") != NULL;
        if (bench && now - bt > 1.5) { bt = now; go_screen((U.screen + 1) % S_COUNT, now); }
    }
    if (U.phase != 2 && SCREENS[U.screen]->update && !app_covers(now) && !video_fs_covers(now))
        SCREENS[U.screen]->update(now);                  /* beneath the program window / fullscreen video the screen is static */
    if (base_dirty_f) full = 1;
    if (screen_dirty_f) damage_d(ST_X, 0, ST_W, 672);
}

static void draw_all(double now)
{
    float op = tw_get(&U.st_op, now), sc = tw_get(&U.st_s, now), dy = tw_get(&U.st_dy, now);
    if (aa_view_covers()) {                              /* Android Auto: only its frame, with the call and HUD on top */
        aa_view_draw(now);
        in_draw(now);
        draw_hud(now);
        return;
    }
    if (video_fs_covers(now)) {                          /* fullscreen video covers everything: nothing is drawn beneath it */
        video_fs_draw(now);
        in_draw(now);                                    /* call and HUD - on top */
        draw_hud(now);
        return;
    }
    gfx_layer_blit(GFX_L_BASE, 0, 0, SCREEN_W, SCREEN_H, 1, 1, 0);
    draw_rail(now);
    draw_bottom(now);
    gfx_layer_blit(GFX_L_SCREEN, D(ST_X), D(ST_Y), D(ST_W), D(ST_H), op, sc, D(dy));
    if (op > 0.002f && SCREENS[U.screen]->draw_dyn && !layer_dyn && !app_covers(now)) {
        gfx_xform(D(ST_X + ST_W * 0.5f), D(ST_Y + ST_H * 0.5f), sc, D(dy), op);
        SCREENS[U.screen]->draw_dyn(now);
        gfx_xform_reset();
    }
    draw_app(now);
    in_draw(now);
    draw_hud(now);
    video_fs_draw(now);
}

/* black rounded screen corners (owner 2026-10-29, radius = card radius PANEL_R): drawn last, over everything (AA, camera, Standby).
 * A rect larger than the screen with an inside border as wide as the radius - its inner edge is the screen with radius PANEL_R;
 * scissored to the four corner squares, so only ~22x22 px each is shaded */
static void screen_corners(void)
{
    float r = D(PANEL_R);
    int s = (int)ceilf(r), c;
    for (c = 0; c < 4; c++) {
        cur_sc[0] = c & 1 ? SCREEN_W - s : 0; cur_sc[1] = c & 2 ? SCREEN_H - s : 0; cur_sc[2] = s; cur_sc[3] = s; cur_sc_on = 1;
        apply_scissor();
        gfx_rect_border(-r, -r, SCREEN_W + 2 * r, SCREEN_H + 2 * r, 2 * r, hexa(0, 0), r, hexa(0, 1));
    }
    cur_sc_on = 0;
    apply_scissor();
}

int ui_frame(double now, int age)
{
    int i, j, k, quiet;
    update(now);                                   /* nd/full already contain marks from the touch handlers */
    {   /* Screen off: fully - only it, 15 frames/s; appearance/exit - over the UI (below) */
        float op = tw_get(&sb_op, now);
        if (scr_off && op > 0.999f && !tw_active(&sb_glow, now) && !tw_active(&sb_clk, now)) {
            int k2;
            nd = 0; full = 0;
            if (now - sb_frame < 1.0 / 15) return 0;
            sb_frame = now;
            gfx_frame_begin();
            cur_sc_on = 0; apply_scissor();
            standby_draw(now - sb_t0, 1, tw_get(&sb_glow, now), tw_get(&sb_clk, now));
            screen_corners();
            gfx_frame_end();
            for (k2 = 0; k2 < HIST; k2++) hist_full[k2] = 1;    /* afterwards - the whole UI */
            base_dirty_f = screen_dirty_f = 1;
            return 1;
        }
        if (op > 0.001f || tw_active(&sb_op, now)) full = 1;
    }
    quiet = !full && nd == 0;
    for (k = 0; k < HIST && quiet; k++) quiet = !hist_full[k] && nhist[k] == 0;
    if (quiet) return 0;

    cur_sc_on = 0; clip_n = 0;
    if (base_dirty_f) { gfx_layer_begin(GFX_L_BASE); draw_base(); gfx_layer_end(); base_dirty_f = 0; }
    {
        /* screen transition: the screen together with its moving part is "photographed" into a layer once, the transition animation is
         * just a texture overlay (on the GC880 redrawing rounded shapes every frame is the main load).
         * We leave Apps alone: it has its own icon wave on entry. */
        int tr = transitioning(now);
        if (tr != layer_dyn) screen_dirty_f = 1;
        if (screen_dirty_f) {
            gfx_layer_begin(GFX_L_SCREEN);
            if (SCREENS[U.screen]->draw_static) SCREENS[U.screen]->draw_static();
            if (tr && SCREENS[U.screen]->draw_dyn) SCREENS[U.screen]->draw_dyn(now);
            gfx_layer_end();
            screen_dirty_f = 0;
            layer_dyn = tr;
            if (!tr) damage_d(ST_X, 0, ST_W, 672);
        }
    }

    if (age < 1 || age > HIST + 1) full = 1;
    for (k = 0; k < age - 1 && !full; k++) if (hist_full[k]) full = 1;

    gfx_frame_begin();
    if (full) {
        cur_sc_on = 0; cur_sc[0] = 0; cur_sc[1] = 0; cur_sc[2] = SCREEN_W; cur_sc[3] = SCREEN_H;
        apply_scissor();
        draw_all(now);
        if (U.fading) gfx_layer_blit(GFX_L_SNAP, 0, 0, SCREEN_W, SCREEN_H, tw_get(&U.fade, now), 1, 0);
        if (tw_get(&sb_op, now) > 0.001f) standby_draw(now - sb_t0, tw_get(&sb_op, now), tw_get(&sb_glow, now), tw_get(&sb_clk, now));
    } else {
        rect all[MAXR * (HIST + 1)];
        int n = 0;
        for (i = 0; i < nd; i++) all[n++] = dmg[i];
        for (k = 0; k < age - 1; k++) for (i = 0; i < nhist[k]; i++) all[n++] = hist[k][i];
        for (i = 0; i < n; i++)
            for (j = i + 1; j < n; j++) {
                rect *a = &all[i], *b = &all[j];
                if (a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h) {
                    float x0 = fminf(a->x, b->x), y0 = fminf(a->y, b->y);
                    float x1 = fmaxf(a->x + a->w, b->x + b->w), y1 = fmaxf(a->y + a->h, b->y + b->h);
                    a->x = x0; a->y = y0; a->w = x1 - x0; a->h = y1 - y0;
                    all[j] = all[--n]; j = i;
                }
            }
        for (i = 0; i < n; i++) {
            int x0 = (int)fmaxf(0, all[i].x), y0 = (int)fmaxf(0, all[i].y);
            int x1 = (int)fminf(SCREEN_W, all[i].x + all[i].w), y1 = (int)fminf(SCREEN_H, all[i].y + all[i].h);
            if (x1 <= x0 || y1 <= y0) continue;
            cur_sc[0] = x0; cur_sc[1] = y0; cur_sc[2] = x1 - x0; cur_sc[3] = y1 - y0; cur_sc_on = 1;
            apply_scissor();
            draw_all(now);
        }
        cur_sc_on = 0;
        apply_scissor();
    }
    screen_corners();
    gfx_frame_end();

    memmove(&hist[1], &hist[0], sizeof(hist[0]) * (HIST - 1));
    memmove(&nhist[1], &nhist[0], sizeof(nhist[0]) * (HIST - 1));
    memmove(&hist_full[1], &hist_full[0], sizeof(hist_full[0]) * (HIST - 1));
    memcpy(hist[0], dmg, sizeof(rect) * (size_t)nd);
    nhist[0] = nd; hist_full[0] = full;
    nd = 0; full = 0;                              /* reset after the frame, not before: otherwise marks from events were lost */
    return 1;
}

/* ================================================================= init */

int ui_init(const char *dir)
{
    char path[512];
    int i;
    double now = pf_now();
    const char *start = getenv("HU_SCREEN");      /* for testing: screen at startup (0..12) */
    memset(&U, 0, sizeof U);
    memset(&A, 0, sizeof A);
    snprintf(assets, sizeof assets, "%s", dir);
    snprintf(path, sizeof path, "%s/fonts", dir);
    if (text_init(path)) return -1;
    if (icons_init(dir)) { text_shutdown(); return -1; }

    /* default state - as in the design (this.state) */
    A.light = getenv("HU_LIGHT") != NULL;
    theme_set(A.light);
    A.scene = 0; A.custom_img = -1; A.bright = 8;
    load_wallpaper();
    if (!wall.id) { icons_shutdown(); text_shutdown(); return -1; }
    A.playing = 0; A.trk = 0; A.pos = 0; A.src_usb = 1;     /* USB is paused at startup (the demo used to play by itself) */
    A.vsel = 0; A.vplaying = 0; A.vpos = 0;                  /* video doesn't play at startup (in the design - demo 1450 s) */
    A.am = 0; A.fm_f = 101.5f; A.am_f = 1008; A.radio_on = 1; A.region = 1;
    A.guides = 1; A.wide = 0;
    A.speaker = 1;
    A.vol = 18; A.phone_vol = 15; A.def_vol = 12; A.speed_vol = 2; A.key_tone = 1;
    A.eqv[0] = 2; A.eqv[1] = 0; A.eqv[2] = 1;
    A.bt_on = 0; A.n_paired = 0; A.ph_batt = A.ph_sig = -1;    /* Bluetooth is real (bt.h): it turns on when the stack reports */
    A.auto_light = 1; A.drl = 1; A.lock_lh = 1; A.speed_alarm = 0; A.speed_limit = 120; A.lang = 0; A.backlight = 6;
    if (getenv("HU_LANG")) A.lang = atoi(getenv("HU_LANG"));   /* for testing: 0 en, 1 uk, 2 ru */
    settings_load();                                     /* saved settings (settings.c) over the defaults */
    if (A.scene == SCENE_CUSTOM && !media_wallpaper_thumb()) A.scene = 0;   /* no wallpaper copy */
    theme_set(A.light);
    if (A.light || A.scene) load_wallpaper();
    {
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
        A.vt_h = tm.tm_hour; A.vt_m = tm.tm_min;
    }
    A.usb_img = -1; A.prev_screen = -1;
    for (i = 0; i < 4; i++) tw_init(&A.eq[i], 3);
    tw_init(&A.art_sc, 1);

    U.ac = 1; U.temp = 5; U.fan = 3; U.blower = 1; U.recirc = 0; U.air = 0; U.last_bat = -1;
    U.clim_live = 1; tw_init(&U.air_pill, 1); tw_init(&U.clim_a, 1);
    U.screen = start ? atoi(start) % S_COUNT : S_DASH;
    U.phase = 0; U.phase_t = now; U.last_min = -1;
    tw_init(&U.st_op, 0); tw_init(&U.st_s, 0.98f); tw_init(&U.st_dy, 24);
    tw_init(&press_tw, 1);
    for (i = 0; i < 5; i++) { tw_init(&U.r_op[i], 0.5f); tw_init(&U.r_sc[i], 0.88f); tw_init(&U.r_glow[i], 0); tw_init(&U.r_ind[i], 0); }
    tw_init(&U.r_bg[0], 0); tw_init(&U.r_bg[1], 0);
    tw_init(&U.ac_fill, 1);
    for (i = 0; i < 16; i++) { tw_init(&U.seg_h[i], i == U.temp ? 30 : 14); tw_init(&U.seg_op[i], i == U.temp ? 1 : 0.4f); }
    for (i = 0; i < 7; i++) { tw_init(&U.fan_h[i], i + 1 == U.fan ? 30 : 14); tw_init(&U.fan_on[i], i + 1 <= U.fan ? 1 : 0); }
    tw_init(&U.blower_bg, 1); tw_init(&U.recirc_bg, 0);
    tw_init(&U.air_x, 0);
    for (i = 0; i < 5; i++) tw_init(&U.air_ic[i], i == 0 ? 1 : 0);
    tw_init(&U.theme_rot, A.light ? 360 : 0);
    tw_init(&U.hud_op, 0); tw_init(&U.hud_k, 0);
    tw_init(&U.fade, 0);
    tw_init(&U.app_op, 0); tw_init(&U.app_k, 0); tw_init(&U.app_launch, 1); tw_init(&U.app_prog, 0);
    layout_core();
    rail_state(now);
    if (SCREENS[U.screen]->enter) SCREENS[U.screen]->enter(now);
    layout_again();
    lua_apps_scan(NULL);                                 /* built-in programs - immediately, the USB stick is added in usb_poll */
    if (getenv("HU_APP")) { usb_poll(now); app_run(atoi(getenv("HU_APP")), now - 4); }   /* program window test */
    if (getenv("HU_RING")) { A.bt_call = 1; snprintf(A.bt_call_num, sizeof A.bt_call_num, "%s", getenv("HU_RING")); }   /* incoming call test */
    if (getenv("HU_STANDBY")) standby_set(1, now - 2);   /* for testing: Screen off right away */
    if (getenv("HU_VFULL")) { A.vfull = 1; A.vctl_t = now; video_fs_layout(now); }   /* fullscreen video test */
    if (getenv("HU_TRACK") && NTRACKS) A.trk = atoi(getenv("HU_TRACK")) % NTRACKS;
    if (bt_init() < 0) fprintf(stderr, "hu: bt: none\n");
    aa_wifi_init();                                       /* wireless AA: RFCOMM (after BT is turned on) + TCP 5288 */
    return 0;
}

int ui_video_active(void) { return A.vplaying && A.usb_conn; }

void ui_shutdown(void)
{
    display_shutdown();
    usb_shutdown();
    aa_view_shutdown();
    player_shutdown();
    gfx_tex_free(&wall);
    icons_shutdown();
    text_shutdown();
}
