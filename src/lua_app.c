/* Lua programs from the USB stick (see lua_app.h, apps/README.md). Lua 5.4 - third_party/lua.
 * All in the main thread: the program is called from drawing (draw) and touch handling (tap / touch). */
#include "lua_app.h"
#include "ui_int.h"
#include "mcu.h"

#include "../third_party/lua/lua.h"
#include "../third_party/lua/lauxlib.h"
#include "../third_party/lua/lualib.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define MEM_LIMIT   (16u << 20)         /* program memory */
#define STEP_LIMIT  20000000L           /* instructions per call (draw / tap / touch / tick): otherwise "hung" */
#define MAN_LIMIT   200000L             /* manifest.lua - table only */
#define BTN_FADE    0.2                 /* button color change: background .2s ease, color .2s ease */

/* ================================================================= program list */

static lua_app *apps;
static int n_apps, apps_cap;

static void free_apps(void)
{
    int i;
    lua_app_stop();
    for (i = 0; i < n_apps; i++) { free(apps[i].dir); gfx_tex_free(&apps[i].icon); }
    free(apps); apps = NULL; n_apps = apps_cap = 0;
}

static size_t mem_used, mem_limit;
static long steps, step_limit;

static void *l_alloc(void *ud, void *p, size_t osize, size_t nsize)
{
    size_t old = p ? osize : 0;
    void *q;
    (void)ud;
    if (!nsize) { mem_used -= old; free(p); return NULL; }
    if (mem_used - old + nsize > mem_limit) return NULL;    /* Lua itself reports "not enough memory" */
    if ((q = realloc(p, nsize))) mem_used = mem_used - old + nsize;
    return q;
}

static void l_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if ((steps += 1000) > step_limit) luaL_error(L, "script is not responding (too many steps)");
}

static int read_file(const char *path, char **buf, long *len, long max)
{
    FILE *f = fopen(path, "rb");
    long n;
    *buf = NULL;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || n > max || fseek(f, 0, SEEK_SET)) { fclose(f); return -1; }
    if (!(*buf = malloc((size_t)n + 1)) || fread(*buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(*buf); *buf = NULL; return -1; }
    (*buf)[n] = 0; *len = n;
    fclose(f);
    return 0;
}

static uint32_t parse_hex(const char *s, float *a)
{
    unsigned v = 0;
    size_t i, n;
    *a = 1;
    if (!s || *s != '#') return 0;
    n = strlen(++s);
    for (i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i])) return 0;
    v = (unsigned)strtoul(s, NULL, 16);
    if (n == 3) return (v >> 8 & 15) * 0x110000 | (v >> 4 & 15) * 0x1100 | (v & 15) * 0x11;
    if (n == 8) { *a = (v & 255) / 255.0f; return v >> 8; }
    return n == 6 ? v : 0;
}

/* manifest.lua: return { name = "...", desc = "...", version = "1.0", author = "...", color = "#RRGGBB" } */
static void manifest_str(lua_State *L, const char *k, char *d, size_t n)
{
    lua_getfield(L, -1, k);
    if (lua_type(L, -1) == LUA_TSTRING) scpy(d, n, lua_tostring(L, -1));
    lua_pop(L, 1);
}
static void read_manifest(lua_app *a)
{
    char p[4200], *src;
    long len;
    lua_State *L;
    snprintf(p, sizeof p, "%s/manifest.lua", a->dir);
    if (read_file(p, &src, &len, 64 << 10)) return;
    mem_used = 0; mem_limit = 1u << 20; steps = 0; step_limit = MAN_LIMIT;
    if ((L = lua_newstate(l_alloc, NULL))) {
        lua_sethook(L, l_hook, LUA_MASKCOUNT, 1000);
        if (luaL_loadbufferx(L, src, (size_t)len, "=manifest.lua", "t") == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK
            && lua_istable(L, -1)) {
            char c[16] = "";
            float al;
            manifest_str(L, "name", a->name, sizeof a->name);
            manifest_str(L, "desc", a->desc, sizeof a->desc);
            manifest_str(L, "version", a->version, sizeof a->version);
            manifest_str(L, "author", a->author, sizeof a->author);
            manifest_str(L, "color", c, sizeof c);
            if (c[0]) a->color = parse_hex(c, &al);
        } else fprintf(stderr, "hu: lua %s: manifest: %s\n", a->id, lua_tostring(L, -1) ? lua_tostring(L, -1) : "not a table");
        lua_close(L);
    }
    free(src);
}

static int cmp_app(const void *x, const void *y) { return strcasecmp(((const lua_app *)x)->name, ((const lua_app *)y)->name); }

static void scan_root(const char *root, int usb)
{
    char p[4400];
    DIR *d;
    struct dirent *e;
    int i;
    if (!(d = opendir(root))) return;
    while ((e = readdir(d))) {
        struct stat st;
        lua_app *a, *g;
        if (e->d_name[0] == '.') continue;
        if (snprintf(p, sizeof p, "%s/%s/main.lua", root, e->d_name) >= (int)sizeof p || stat(p, &st) || !S_ISREG(st.st_mode)) continue;
        for (i = 0; i < n_apps && strcmp(apps[i].id, e->d_name); i++) {}
        if (i < n_apps) continue;                       /* a built-in one (NAND) already exists - it stays (owner) */
        if (!(g = grow(apps, &apps_cap, n_apps + 1, sizeof *apps))) break;
        apps = g; a = &apps[n_apps];
        memset(a, 0, sizeof *a);
        a->main_size = (long)st.st_size; a->usb = usb;
        p[strlen(p) - strlen("/main.lua")] = 0;
        if (!(a->dir = strdup(p))) break;
        scpy(a->id, sizeof a->id, e->d_name);
        scpy(a->name, sizeof a->name, e->d_name);       /* no manifest - folder name */
        a->color = 0x2DBE6C;                            /* design: default appColor */
        read_manifest(a);
        snprintf(p, sizeof p, "%s/icon.png", a->dir);
        if (stat(p, &st) == 0 && st.st_size < (2 << 20)) gfx_tex_load(&a->icon, p, 1);   /* icon up to 2 MB */
        n_apps++;
    }
    closedir(d);
}

void lua_apps_scan(const char *mnt)
{
    char root[4096];
    free_apps();
    scan_root(LUA_NAND_DIR, 0);
    if (mnt) { snprintf(root, sizeof root, "%s/apps", mnt); scan_root(root, 1); }
    if (n_apps) qsort(apps, (size_t)n_apps, sizeof *apps, cmp_app);
    fprintf(stderr, "hu: lua: %d apps (%s%s)\n", n_apps, LUA_NAND_DIR, mnt ? " + flash" : "");
}

int lua_apps_count(void) { return n_apps; }
const lua_app *lua_apps_get(int i) { return i >= 0 && i < n_apps ? &apps[i] : NULL; }
int lua_apps_find(const char *dir)
{
    int i;
    for (i = 0; i < n_apps; i++) if (!strcmp(apps[i].dir, dir)) return i;
    return -1;
}

/* ================================================================= running program */

static lua_State *L;
static int run_i = -1, failed, in_draw, want_redraw, has_tick;
static float ox, oy, cw, ch;                            /* canvas (design px) */
static double t_now;

typedef struct { float x, y, w, h; char key[48]; } lbtn;   /* key: "n<number>" or "s<string>" */
static lbtn *btn;
static int n_btn, btn_cap;
typedef struct { char key[48]; col bf, bt, ff, ft; double t0; } fade;
#define NFADE 96
static fade fades[NFADE];
static int n_fade;
static char pressed[48];
static tween press_tw;
static float down_x, down_y;

static void fail(const char *what)
{
    const lua_app *a = lua_apps_get(run_i);
    fprintf(stderr, "hu: lua %s: %s: %s\n", a ? a->id : "?", what, L ? lua_tostring(L, -1) : "");
    failed = 1;
}

/* call the program's global function (if any) with n arguments on the stack */
static void call(const char *fn, int nargs)
{
    if (!L || failed) { if (L) lua_pop(L, nargs); return; }
    lua_getglobal(L, fn);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1 + nargs); return; }
    lua_insert(L, -1 - nargs);
    steps = 0; step_limit = STEP_LIMIT;
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) { fail(fn); lua_pop(L, 1); }
}

static col arg_col(lua_State *S, int i, col def)
{
    float a;
    const char *s;
    if (lua_isnoneornil(S, i)) return def;
    if (lua_type(S, i) == LUA_TNUMBER) return hexc((uint32_t)lua_tointeger(S, i));
    s = luaL_checkstring(S, i);
    if (!strcmp(s, "accent")) return C_ACCENT;          /* system colors: follow the theme */
    if (!strcmp(s, "on_accent")) return C_ONACCENT;
    if (!strcmp(s, "accent_soft")) return C_ACC_SOFT;
    if (!strncmp(s, "rgba(", 5)) {                      /* as in CSS: rgba(255, 255, 255, 0.10) */
        int r, g, bl;
        float al;
        if (sscanf(s + 5, "%d , %d , %d , %f", &r, &g, &bl, &al) == 4)
            return hexa((uint32_t)((r & 255) << 16 | (g & 255) << 8 | (bl & 255)), al);
    }
    if (*s != '#') luaL_argerror(S, i, "color: \"#RRGGBB\", \"#RRGGBBAA\", \"rgba(r, g, b, a)\", 0xRRGGBB or \"accent\"");
    {
        uint32_t rgb = parse_hex(s, &a);                /* argument evaluation order is unspecified - separately */
        return hexa(rgb, a);
    }
}
static col opt_col(lua_State *S, int t, const char *k, col def)
{
    col c;
    lua_getfield(S, t, k);
    c = arg_col(S, -1, def);
    lua_pop(S, 1);
    return c;
}
static float opt_num(lua_State *S, int t, const char *k, float def)
{
    float v;
    lua_getfield(S, t, k);
    v = lua_isnumber(S, -1) ? (float)lua_tonumber(S, -1) : def;
    lua_pop(S, 1);
    return v;
}
static const char *opt_str(lua_State *S, int t, const char *k, const char *def)
{
    const char *v;
    lua_getfield(S, t, k);
    v = lua_type(S, -1) == LUA_TSTRING ? lua_tostring(S, -1) : def;   /* the string stays in the table - alive */
    lua_pop(S, 1);
    return v;
}
static void need_draw(lua_State *S) { if (!in_draw) luaL_error(S, "ui drawing works only inside draw()"); }

/* ui.rect(x, y, w, h, color [, radius [, border_color]]) */
static int u_rect(lua_State *S)
{
    float x = (float)luaL_checknumber(S, 1), y = (float)luaL_checknumber(S, 2);
    float w = (float)luaL_checknumber(S, 3), h = (float)luaL_checknumber(S, 4), r = (float)luaL_optnumber(S, 6, 0);
    col c = arg_col(S, 5, C_WHITE);
    need_draw(S);
    if (lua_isnoneornil(S, 7)) rrect(ox + x, oy + y, w, h, r, c);
    else rrect_b(ox + x, oy + y, w, h, r, c, arg_col(S, 7, c));
    return 0;
}

/* text: y - middle of the line; align left / center / right from x; maxw - clip (overflow: hidden) */
static void text_at(const char *s, float x, float cy, int wt, float size, col c, const char *align, float maxw, float ls, int tnum)
{
    font *f = F(wt, size);
    float tw, bx;
    int clip;
    text_tnum(tnum);
    tw = text_width(f, s, D(ls)) / K;
    clip = maxw > 0 && tw > maxw;
    if (clip) tw = maxw;
    bx = !strcmp(align, "center") ? x - tw * 0.5f : !strcmp(align, "right") ? x - tw : x;
    if (clip) clip_push(bx, cy - size, maxw, size * 2);
    text_draw(f, D(bx), D(cy) - text_line_height(f) * 0.5f, s, c, D(ls));
    if (clip) clip_pop();
    text_tnum(0);
}

/* ui.text(str, x, y, {size, weight, color, align, maxw, spacing, tnum}) */
static int u_text(lua_State *S)
{
    const char *s = luaL_checkstring(S, 1);
    float x = (float)luaL_checknumber(S, 2), y = (float)luaL_checknumber(S, 3);
    int t = lua_istable(S, 4) ? 4 : 0, tn = 0;
    need_draw(S);
    if (t) { lua_getfield(S, t, "tnum"); tn = lua_toboolean(S, -1); lua_pop(S, 1); }
    text_at(s, ox + x, oy + y, t ? (int)opt_num(S, t, "weight", 600) : 600, t ? opt_num(S, t, "size", 20) : 20,
            t ? opt_col(S, t, "color", hexc(0xF5F5F7)) : hexc(0xF5F5F7), t ? opt_str(S, t, "align", "left") : "left",
            t ? opt_num(S, t, "maxw", 0) : 0, t ? opt_num(S, t, "spacing", 0) : 0, tn);
    return 0;
}

/* ui.text_width(str, size, weight [, tnum]) -> px; ui.line_height(size, weight) -> px (line-height: normal) */
static int u_text_width(lua_State *S)
{
    const char *s = luaL_checkstring(S, 1);
    font *f = F((int)luaL_optinteger(S, 3, 600), (float)luaL_optnumber(S, 2, 20));
    text_tnum(lua_toboolean(S, 4));
    lua_pushnumber(S, text_width(f, s, 0) / K);
    text_tnum(0);
    return 1;
}
static int u_line_height(lua_State *S)
{
    lua_pushnumber(S, t_lh((int)luaL_optinteger(S, 2, 600), (float)luaL_optnumber(S, 1, 20)));
    return 1;
}

/* ui.icon(name, cx, cy, size, color) - system icons */
static int u_icon(lua_State *S)
{
    const char *n = luaL_checkstring(S, 1);
    need_draw(S);
    ico(n, ox + (float)luaL_checknumber(S, 2), oy + (float)luaL_checknumber(S, 3), (float)luaL_optnumber(S, 4, 24),
        arg_col(S, 5, hexc(0xF5F5F7)));
    return 0;
}

static void btn_key(lua_State *S, int i, char *k, size_t n)
{
    if (lua_type(S, i) == LUA_TNUMBER) snprintf(k, n, "n%.17g", lua_tonumber(S, i));
    else if (lua_type(S, i) == LUA_TSTRING) snprintf(k, n, "s%s", lua_tostring(S, i));
    else luaL_argerror(S, i, "button id: string or number");
}

static int col_eq(col a, col b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

/* smooth button color change (transition: background .2s ease, color .2s ease) */
static void btn_colors(const char *key, col *bg, col *fg)
{
    fade *f = NULL;
    int i;
    float u;
    for (i = 0; i < n_fade; i++) if (!strcmp(fades[i].key, key)) { f = &fades[i]; break; }
    if (!f) {
        if (n_fade == NFADE) return;
        f = &fades[n_fade++];
        scpy(f->key, sizeof f->key, key);
        f->bf = f->bt = *bg; f->ff = f->ft = *fg; f->t0 = -1;
        return;
    }
    u = f->t0 < 0 ? 1 : (float)((t_now - f->t0) / BTN_FADE);
    u = u >= 1 ? 1 : bezier_eval(&EASE, u);
    if (!col_eq(*bg, f->bt) || !col_eq(*fg, f->ft)) {
        f->bf = col_mix(f->bf, f->bt, u); f->ff = col_mix(f->ff, f->ft, u);
        f->bt = *bg; f->ft = *fg; f->t0 = t_now; u = 0;
    }
    *bg = col_mix(f->bf, f->bt, u); *fg = col_mix(f->ff, f->ft, u);
}

/* ui.button(id, x, y, w, h, {label, size, weight, bg, fg, radius, border, icon, icon_size}) -
 * system button: press (scale .93, darkening), color change .2s; released on it - tap(id) */
static int u_button(lua_State *S)
{
    char key[48];
    float x = (float)luaL_checknumber(S, 2), y = (float)luaL_checknumber(S, 3);
    float w = (float)luaL_checknumber(S, 4), h = (float)luaL_checknumber(S, 5), k = 1, r;
    int t = lua_istable(S, 6) ? 6 : 0;
    col bg, fg, bd;
    const char *lb, *icn;
    lbtn *b;
    need_draw(S);
    btn_key(S, 1, key, sizeof key);
    if (!(b = grow(btn, &btn_cap, n_btn + 1, sizeof *btn))) return luaL_error(S, "not enough memory");
    btn = b; b = &btn[n_btn++];
    b->x = x; b->y = y; b->w = w; b->h = h; scpy(b->key, sizeof b->key, key);
    bg = t ? opt_col(S, t, "bg", hexa(0xFFFFFF, 0.1f)) : hexa(0xFFFFFF, 0.1f);
    fg = t ? opt_col(S, t, "fg", hexc(0xF5F5F7)) : hexc(0xF5F5F7);
    r = t ? opt_num(S, t, "radius", 22) : 22;
    lb = t ? opt_str(S, t, "label", NULL) : NULL;
    icn = t ? opt_str(S, t, "icon", NULL) : NULL;
    btn_colors(key, &bg, &fg);
    if (!strcmp(pressed, key)) k = tw_get(&press_tw, t_now);
    x = ox + x + w * (1 - k) * 0.5f; y = oy + y + h * (1 - k) * 0.5f; w *= k; h *= k; r *= k;
    bg = col_mix(bg, hexa(0, bg.a), press_dim(k)); fg = col_mix(fg, hexa(0, fg.a), press_dim(k));
    if (t) { lua_getfield(S, t, "border"); }
    if (t && !lua_isnil(S, -1)) { bd = arg_col(S, -1, bg); lua_pop(S, 1); rrect_b(x, y, w, h, r, bg, bd); }
    else { if (t) lua_pop(S, 1); rrect(x, y, w, h, r, bg); }
    if (icn) ico(icn, x + w * 0.5f, y + h * 0.5f, (t ? opt_num(S, t, "icon_size", 24) : 24) * k, fg);
    if (lb) {
        float sz = opt_num(S, t, "size", 28);
        font *f = F((int)opt_num(S, t, "weight", 600), sz);    /* font isn't scaled (atlas) - text centered */
        text_draw(f, D(x + w * 0.5f) - text_width(f, lb, 0) * 0.5f, D(y + h * 0.5f) - text_line_height(f) * 0.5f, lb, fg, 0);
    }
    return 0;
}

static int u_redraw(lua_State *S) { (void)S; want_redraw = 1; return 0; }
static int u_size(lua_State *S) { lua_pushnumber(S, cw); lua_pushnumber(S, ch); return 2; }
static int u_time(lua_State *S) { lua_pushnumber(S, t_now); return 1; }
static int u_close(lua_State *S) { (void)S; failed = 2; return 0; }    /* the core will close the window */

/* print -> HU log */
static int u_print(lua_State *S)
{
    int i, n = lua_gettop(S);
    const lua_app *a = lua_apps_get(run_i);
    fprintf(stderr, "hu: lua %s:", a ? a->id : "?");
    for (i = 1; i <= n; i++) fprintf(stderr, " %s", luaL_tolstring(S, i, NULL)), lua_pop(S, 1);
    fputc('\n', stderr);
    return 0;
}

/* require("name") - module name.lua from the program folder (text only, no paths) */
static int u_require(lua_State *S)
{
    const char *n = luaL_checkstring(S, 1), *q;
    const lua_app *a = lua_apps_get(run_i);
    char p[4200], *src;
    long len;
    for (q = n; *q; q++) if (!isalnum((unsigned char)*q) && *q != '_') return luaL_error(S, "require: bad module name '%s'", n);
    lua_getfield(S, LUA_REGISTRYINDEX, "hu_loaded");
    lua_getfield(S, -1, n);
    if (!lua_isnil(S, -1)) return 1;
    lua_pop(S, 1);
    snprintf(p, sizeof p, "%s/%s.lua", a ? a->dir : ".", n);
    if (read_file(p, &src, &len, 1 << 20)) return luaL_error(S, "require: no module '%s'", n);
    snprintf(p, sizeof p, "=%s.lua", n);
    if (luaL_loadbufferx(S, src, (size_t)len, p, "t") != LUA_OK) { free(src); return lua_error(S); }
    free(src);
    lua_call(S, 0, 1);
    if (lua_isnil(S, -1)) { lua_pop(S, 1); lua_pushboolean(S, 1); }
    lua_pushvalue(S, -1);
    lua_setfield(S, -3, n);
    return 1;
}

/* sys.log("hu" | "mcu", n) -> table of the last n lines of the HU log (read-only; the "Logs" program) */
static int u_log(lua_State *S)
{
    const char *w = luaL_checkstring(S, 1), *dir = getenv("HU_DIR") ? getenv("HU_DIR") : "/dev/hu";
    int n = (int)luaL_optinteger(S, 2, 50), k = 0;
    char p[512], *b;
    long sz, from, got;
    FILE *f;
    if (n < 1) n = 1;
    if (n > 400) n = 400;
    snprintf(p, sizeof p, "%s/%s", dir, !strcmp(w, "mcu") ? "mcu.log" : "log.txt");
    lua_newtable(S);
    if (!(f = fopen(p, "rb"))) return 1;
    fseek(f, 0, SEEK_END); sz = ftell(f);
    from = sz - (long)n * 200 > 0 ? sz - (long)n * 200 : 0;   /* tail: log lines are shorter than 200 bytes */
    if (!(b = malloc((size_t)(sz - from) + 1))) { fclose(f); return 1; }
    fseek(f, from, SEEK_SET);
    got = (long)fread(b, 1, (size_t)(sz - from), f);
    fclose(f);
    b[got] = 0;
    {
        char *e = b + got, *s;
        int cnt = 0;
        for (s = e; s > b && cnt <= n; s--) if (s[-1] == '\n' && s != e) cnt++;   /* start of the n-th line from the end */
        if (cnt > n) while (*s && *s != '\n') s++;
        if (*s == '\n') s++;
        if (from > 0 && s == b) { while (*s && *s != '\n') s++; if (*s) s++; }   /* fragment of the first line */
        while (*s) {
            char *nl = strchr(s, '\n');
            size_t len = nl ? (size_t)(nl - s) : strlen(s);
            if (len) { lua_pushlstring(S, s, len); lua_rawseti(S, -2, ++k); }
            if (!nl) break;
            s = nl + 1;
        }
    }
    free(b);
    return 1;
}

/* sys.console() - terminal on screen (console.sh next to hu): only for programs from NAND (LUA_NAND_DIR), not from a stick -
 * otherwise any stick with apps/ would get root. true - started */
static int u_console(lua_State *S)
{
    const lua_app *a = lua_apps_get(run_i);
    const char *dir = getenv("HU_DIR") ? getenv("HU_DIR") : "/dev/hu";
    char p[512];
    pid_t pid;
    if (!a || strncmp(a->dir, LUA_NAND_DIR "/", sizeof LUA_NAND_DIR)) {
        fprintf(stderr, "hu: lua %s: sys.console - only for apps in %s\n", a ? a->id : "?", LUA_NAND_DIR);
        lua_pushboolean(S, 0); return 1;
    }
    snprintf(p, sizeof p, "%s/console.sh", dir);
    if (access(p, R_OK)) { lua_pushboolean(S, 0); return 1; }
    if ((pid = fork()) == 0) {                          /* double fork: hu doesn't wait, no zombie is left */
        if (fork() == 0) { setsid(); execl("/bin/sh", "sh", p, (char *)NULL); _exit(127); }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
    fprintf(stderr, "hu: lua %s: console\n", a->id);
    lua_pushboolean(S, pid > 0); return 1;
}

static int u_os_time(lua_State *S) { lua_pushinteger(S, (lua_Integer)time(NULL)); return 1; }
static int u_os_clock(lua_State *S) { lua_pushnumber(S, t_now); return 1; }

static void open_sandbox(lua_State *S)
{
    static const char *const DROP[] = { "dofile", "loadfile", "load", NULL };
    static const luaL_Reg UI[] = {
        { "rect", u_rect }, { "text", u_text }, { "text_width", u_text_width }, { "line_height", u_line_height },
        { "icon", u_icon }, { "button", u_button }, { "redraw", u_redraw }, { "size", u_size }, { "time", u_time },
        { "close", u_close }, { NULL, NULL } };
    int i;
    luaL_requiref(S, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(S, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(S, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(S, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(S, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(S, 5);
    for (i = 0; DROP[i]; i++) { lua_pushnil(S); lua_setglobal(S, DROP[i]); }
    lua_pushcfunction(S, u_print); lua_setglobal(S, "print");
    lua_pushcfunction(S, u_require); lua_setglobal(S, "require");
    lua_newtable(S); lua_setfield(S, LUA_REGISTRYINDEX, "hu_loaded");
    lua_newtable(S);                                    /* os: time only */
    lua_pushcfunction(S, u_os_time); lua_setfield(S, -2, "time");
    lua_pushcfunction(S, u_os_clock); lua_setfield(S, -2, "clock");
    lua_setglobal(S, "os");
    luaL_newlib(S, UI);
    lua_setglobal(S, "ui");
    lua_newtable(S);                                    /* sys: HU logs (read-only) */
    lua_pushcfunction(S, u_log); lua_setfield(S, -2, "log");
    lua_pushcfunction(S, u_console); lua_setfield(S, -2, "console");
    lua_setglobal(S, "sys");
}

int lua_app_start(int i)
{
    const lua_app *a = lua_apps_get(i);
    char p[4200], *src;
    long len;
    lua_app_stop();
    if (!a) return -1;
    run_i = i; failed = 0; want_redraw = 1;
    snprintf(p, sizeof p, "%s/main.lua", a->dir);
    if (read_file(p, &src, &len, 4 << 20)) { fprintf(stderr, "hu: lua %s: cannot read main.lua\n", a->id); failed = 1; return -1; }
    mem_used = 0; mem_limit = MEM_LIMIT;
    if (!(L = lua_newstate(l_alloc, NULL))) { free(src); failed = 1; return -1; }
    lua_sethook(L, l_hook, LUA_MASKCOUNT, 1000);
    open_sandbox(L);
    steps = 0; step_limit = STEP_LIMIT;
    if (luaL_loadbufferx(L, src, (size_t)len, "=main.lua", "t") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        fail("main.lua"); free(src); return -1;
    }
    free(src);
    lua_getglobal(L, "tick");
    has_tick = lua_isfunction(L, -1);
    lua_pop(L, 1);
    fprintf(stderr, "hu: lua %s: started (%u KB)\n", a->id, (unsigned)(mem_used >> 10));
    return 0;
}

void lua_app_stop(void)
{
    if (L) lua_close(L);
    L = NULL; run_i = -1; failed = 0; has_tick = 0;
    free(btn); btn = NULL; n_btn = btn_cap = 0;
    n_fade = 0; pressed[0] = 0;
}

int lua_app_running(void) { return run_i; }
int lua_app_failed(void) { return L ? failed : 0; }

void lua_app_draw(float x, float y, float w, float h, double now)
{
    if (!L || failed) return;
    ox = x; oy = y; cw = w; ch = h; t_now = now;
    if (has_tick) { lua_pushnumber(L, now); call("tick", 1); }
    n_btn = 0; want_redraw = 0; in_draw = 1;
    clip_push(x, y, w, h);
    lua_pushnumber(L, w); lua_pushnumber(L, h);
    call("draw", 2);
    clip_pop();
    in_draw = 0;
}

static int btn_at(float x, float y)
{
    int i;
    for (i = n_btn - 1; i >= 0; i--)
        if (x >= btn[i].x && x < btn[i].x + btn[i].w && y >= btn[i].y && y < btn[i].y + btn[i].h) return i;
    return -1;
}

static void push_key(const char *k)
{
    if (k[0] == 'n') {
        double v = strtod(k + 1, NULL);
        if (v == floor(v) && fabs(v) < 1e15) lua_pushinteger(L, (lua_Integer)v); else lua_pushnumber(L, v);
    } else lua_pushstring(L, k + 1);
}

void lua_app_touch(int phase, float x, float y, double now)
{
    static const char *PH[3] = { "down", "move", "up" };
    int b;
    if (!L || failed) return;
    t_now = now;
    x -= ox; y -= oy;
    if (phase == 0) {
        down_x = x; down_y = y;
        if ((b = btn_at(x, y)) >= 0) {
            scpy(pressed, sizeof pressed, btn[b].key);
            tw_init(&press_tw, 1); tw_set(&press_tw, PRESS_SCALE, PRESS_MS, &EASE, now);
            if (A.key_tone && !A.muted) mcu_beep();     /* click, like system buttons */
        }
    }
    lua_pushstring(L, PH[phase]); lua_pushnumber(L, x); lua_pushnumber(L, y);
    call("touch", 3);
    if (phase == 2 && pressed[0]) {
        char k[48];
        scpy(k, sizeof k, pressed);
        tw_set(&press_tw, 1, PRESS_MS, &EASE, now);
        if ((b = btn_at(x, y)) >= 0 && !strcmp(btn[b].key, k)) { push_key(k); call("tap", 1); }
        scpy(pressed, sizeof pressed, k);               /* the release animation plays out on this button */
    }
    want_redraw = 1;
}

int lua_app_busy(double now)
{
    int i;
    if (!L || failed) return 0;
    if (want_redraw || has_tick || tw_active(&press_tw, now)) return 1;
    for (i = 0; i < n_fade; i++) if (fades[i].t0 >= 0 && now - fades[i].t0 < BTN_FADE) return 1;
    return 0;
}
