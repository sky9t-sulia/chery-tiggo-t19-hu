/* Lua Apps screen: programs from <USB stick>/apps (lua_app.c), 3-per-row grid, a tile launches the window "Running app".
 * Design v21: is.lua (luaApps, luaSub, usbNone). */
#include "ui_int.h"
#include "i18n.h"
#include "lua_app.h"

#include <math.h>
#include <stdio.h>

enum { H_CLOSE = HIT_SCREEN, H_LIST, H_A0 };
#define SLOTS 24

#define X0 PX0                       /* v26: padding 24 */
#define Y0 PY0
#define CW PCW
#define HH fmaxf(52, t_lh(800, 28) + 2 + t_lh(600, 18))   /* header: title 28 + caption */
#define HC (Y0 + HH * 0.5f)
#define GY (Y0 + HH + 22)         /* grid: after the header, gap 22 */
#define GH (PBOT - GY)
#define TW ((CW - 36) / 3)        /* 3 columns, gap 18 */
#define TH 132.0f
#define PITCH (TH + 18)

static scroller sc;
static float last_off = -1;
static int slot_app[SLOTS], last_n = -1;

static float content_h(void) { int r = (lua_apps_count() + 2) / 3; return r ? r * PITCH - 18 : 0; }

static void layout(void)
{
    int i, first = (int)(sc.off / PITCH) * 3;
    set_hit(H_CLOSE, X0, HC - 26, 52, 52);
    for (i = 0; i < SLOTS; i++) slot_app[i] = -1;
    if (!lua_apps_count()) return;
    set_scroll(H_LIST, X0, GY, CW, GH);
    for (i = first; i < lua_apps_count() && i < first + SLOTS; i++) {
        float y0 = GY + (i / 3) * PITCH - sc.off, y1 = y0 + TH;
        if (y0 < GY) y0 = GY;
        if (y1 > GY + GH) y1 = GY + GH;
        if (y1 <= y0) continue;
        slot_app[i % SLOTS] = i;
        set_hit(H_A0 + i % SLOTS, X0 + (i % 3) * (TW + 18), y0, TW, y1 - y0);
    }
}

static void draw_static(void)
{
    char sub[64];
    float l1 = t_lh(800, 28), l2 = t_lh(600, 18), y = HC - (l1 + 2 + l2) * 0.5f;
    panel(ST_X, ST_Y, ST_W, ST_H, PANEL_R);
    rrect(X0 + 66, HC - 21, 42, 42, 12, hexc(0x3B5BDB));
    ico("app_lua_w20", X0 + 87, HC, 24, C_WHITE);
    int i, nand = 0;                                   /* programs from NAND (/mnt/data1/hu-apps) - not in the design:
                                                        * " + builtin" / "N apps · builtin" (owner, DEVIATIONS.md) */
    for (i = 0; i < lua_apps_count(); i++) if (!lua_apps_get(i)->usb) nand++;
    if (A.usb_conn) snprintf(sub, sizeof sub, tr_n(nand ? "%d apps \xC2\xB7 USB drive \xE2\x80\xBA apps + builtin"
                                                        : "%d apps \xC2\xB7 USB drive \xE2\x80\xBA apps", (long)(lua_apps_count())), lua_apps_count());
    else if (nand) snprintf(sub, sizeof sub, tr_n("%d apps \xC2\xB7 builtin", (long)nand), nand);
    else snprintf(sub, sizeof sub, "USB drive not connected");
    t_left(800, 28, X0 + 122, y + l1 * 0.5f, "Lua Apps", C_TEXT);
    t_left(600, 18, X0 + 122, y + l1 + 2 + l2 * 0.5f, sub, C_SUB);
    if (!A.usb_conn && !lua_apps_count()) {            /* usbNone: circle 96, "No USB drive", caption */
        float h1 = t_lh(800, 34), h2 = t_lh(600, 21), h = 96 + 12 + 6 + h1 + 12 + h2, cx = X0 + CW * 0.5f;
        y = GY + (GH - h) * 0.5f;
        rrect(cx - 48, y, 96, 96, 48, C_ACC_SOFT);
        ico("usb_full_w18", cx, y + 48, 46, C_ACCENT);
        y += 96 + 12 + 6;
        t_center(800, 34, cx, y + h1 * 0.5f, "No USB drive", C_TEXT);
        t_center(600, 21, cx, y + h1 + 12 + h2 * 0.5f, "Insert a USB drive with .lua apps to run them.", C_SUB);
    }
}

/* tile: radius 22, border, icon 76 (icon.png or color + icon), name 23/800, description 17/600, "apps/<folder> · v<version>" 16/600 */
static void tile(int i, double now)
{
    const lua_app *a = lua_apps_get(i);
    char meta[128];
    float x = X0 + (i % 3) * (TW + 18), y = GY + (i / 3) * PITCH - sc.off, w = TW, h = TH;
    float k = press_box(H_A0 + i % SLOTS, &x, &y, &w, &h, now);
    float h1 = t_lh(800, 23), h2 = t_lh(600, 17), h3 = t_lh(600, 16), ty = y + h * 0.5f - (h1 + 4 + h2 + 4 + h3) * 0.5f;
    float tx = x + (20 + 76 + 18) * k, tw = w - (20 + 76 + 18 + 20) * k;
    col tc = col_alpha(C_TEXT, 1 - press_dim(k));
    rrect_b(x, y, w, h, 22 * k, C_INNER, C_BORDER);
    if (a->icon.id) gfx_image(&a->icon, D(x + 20 * k), D(y + h * 0.5f - 38 * k), D(76 * k), D(76 * k), 0, 0, 1, 1, C_WHITE);
    else {
        rrect(x + 20 * k, y + h * 0.5f - 38 * k, 76 * k, 76 * k, 20 * k, hexc(a->color));
        ico("bin", x + 58 * k, y + h * 0.5f, 36 * k, C_WHITE);
    }
    snprintf(meta, sizeof meta, "%s/%s%s%s", a->usb ? "apps" : "builtin", a->id,   /* NAND is not a USB stick ("apps/" as in the design) */
             a->version[0] ? " \xC2\xB7 v" : "", a->version);
    t_fit(800, 23, tx, ty + h1 * 0.5f, tw, a->name, tc);
    t_fit(600, 17, tx, ty + h1 + 4 + h2 * 0.5f, tw, a->desc, C_SUB);
    text_tnum(1);
    t_fit(600, 16, tx, ty + h1 + 4 + h2 + 4 + h3 * 0.5f, tw, meta, C_MUTED);
    text_tnum(0);
}

static void draw_dyn(double now)
{
    float k = press_k(H_CLOSE, now);
    int i, first = (int)(sc.off / PITCH) * 3;
    rrect(X0 + 26 - 26 * k, HC - 26 * k, 52 * k, 52 * k, 26 * k, C_INNER);
    ico("close", X0 + 26, HC, 24 * k, col_alpha(C_TEXT, 1 - press_dim(k)));
    if (!lua_apps_count()) return;
    clip_push(X0 - 10, GY, CW + 20, GH);
    for (i = first; i < lua_apps_count() && i < first + SLOTS; i++) {
        if (GY + (i / 3) * PITCH - sc.off > GY + GH) break;
        tile(i, now);
    }
    clip_pop();
}

static void list_changed(void)
{
    sc.max = fmaxf(0, content_h() - GH);
    if (sc.off > sc.max) sc.off = sc.max;
}

static void update(double now)
{
    static int last_usb = -1;
    if (A.usb_conn != last_usb || lua_apps_count() != last_n) {    /* stick inserted / removed - rebuild the list */
        last_usb = A.usb_conn; last_n = lua_apps_count();
        list_changed();
        screen_dirty(); layout_again(); damage_d(ST_X, ST_Y, ST_W, ST_H);
    }
    if (sc_update(&sc, now) || sc.off != last_off) { last_off = sc.off; damage_d(ST_X, GY - 10, ST_W, GH + 20); layout_again(); }
    if (press_active(now)) damage_d(ST_X, ST_Y, ST_W, ST_H);
}

static void click(int id, double now)
{
    if (id == H_CLOSE) close_screen(now);
    else if (id >= H_A0 && id < H_A0 + SLOTS && slot_app[id - H_A0] >= 0) app_run(slot_app[id - H_A0], now);
}

static void drag(int id, float x, float y, int phase, double now)
{
    (void)x;
    if (id == H_LIST) sc_drag(&sc, y, phase, now);
}

static void enter(double now)
{
    (void)now;
    sc.off = 0; sc.v = 0; sc.drag = 0; last_off = -1;
    list_changed();
}

const screen_def SCR_LUA = { layout, draw_static, draw_dyn, update, click, enter, drag };
