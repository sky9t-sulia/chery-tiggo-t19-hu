/* USB Files screen: folders (Music / Videos / Images / Other), file lists, photo grid, photo viewer (wallpaper from a photo),
 * Other - stick files as is (directories and files). Design: Main.dc.html, is.usb + extra().
 * Data - the USB stick (media.c: music/video/photos; Other - the directory is read on entry). */
#include "ui_int.h"
#include "i18n.h"
#include "media.h"
#include "lua_app.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

enum { H_LEFT = HIT_SCREEN, H_EJECT, H_LIST, H_F0, H_R0 = H_F0 + 4, H_I0 = H_R0 + 16, H_VCLOSE = H_I0 + 16, H_VSET, H_VPREV, H_VNEXT };

#define PX PX0                       /* v26: padding 24 */
#define PY PY0
#define CW PCW
#define CY (PY + fmaxf(52, t_lh(800, 28) + 2 + t_lh(600, 18)) + 18)   /* v26: header (title 28 + path), gap 18 */
#define CH (PBOT - CY)
#define HC (PY + (A.usb_conn ? fmaxf(52, t_lh(800, 28) + 2 + t_lh(600, 18)) : 52) * 0.5f)   /* middle of the header */
#define COL4 ((CW - 54) / 4)
#define IMG_PITCH (158 + t_lh(600, 18) + 18)   /* photo grid row */

/* ---- Other: current stick directory */
/* v21: in apps/<folder>/ - main.lua and other *.lua (Lua script: runs the program), manifest.lua, icon.png (program icon) */
enum { T_DIR, T_AUDIO, T_VIDEO, T_IMAGE, T_LUA, T_DOC, T_MANIFEST, T_ASSET };
static const char *TYPE_KIND[8] = { "Folder", "Audio", "Video", "Image", "Lua script", "Document", "Manifest", "Image" };
static const uint32_t TYPE_COL[8] = { 0x2F7BF6, 0xFF4F6A, 0x7B61FF, 0xC2410C, 0x2DBE6C, 0x5E5E66, 0x5E5E66, 0xC2410C };
static const char *TYPE_ICON[8] = { "app_usb_w19", "app_music_w19", "app_video_w19", "app_display_w19", "bin", "doc", "doc", "app_display_w19" };
/* button on the right (actBg / isDir / isFile): 0 - none (doc, manifest), 1 - ">", 2 - play */
static const int TYPE_ACT[8] = { 1, 2, 2, 1, 2, 0, 0, 1 };
typedef struct { char name[256]; int type, items; long long size; } fs_item;   /* name: whole NAME_MAX - the path is built from it */
static fs_item *fs;                                     /* grows with folder size */
static int fs_n, fs_cap, fs_gen = -1;
static char cwd[4096];                                  /* relative to the stick, "" - root */

/* program whose folder is open in Other (apps/<folder>), -1 - none */
static int lua_apps_find_cwd(void)
{
    char p[4400];
    snprintf(p, sizeof p, "%s/%s", A.usb_mnt, cwd);
    return lua_apps_find(p);
}

static int in_app_dir(void) { return !strncmp(cwd, "apps/", 5) && !strchr(cwd + 5, '/') && lua_apps_find_cwd() >= 0; }
static int type_of(const char *n)
{
    const char *e = strrchr(n, '.');
    int k = media_kind(n);
    if (in_app_dir()) {
        if (!strcmp(n, "manifest.lua")) return T_MANIFEST;
        if (e && !strcmp(e, ".lua")) return T_LUA;
        if (!strcmp(n, "icon.png")) return T_ASSET;
    }
    if (k) return k == 1 ? T_AUDIO : k == 2 ? T_VIDEO : T_IMAGE;
    return T_DOC;
}
static int cmp_fs(const void *a, const void *b)
{
    const fs_item *x = a, *y = b;
    if ((x->type == T_DIR) != (y->type == T_DIR)) return x->type == T_DIR ? -1 : 1;   /* folders first */
    return strcasecmp(x->name, y->name);
}
static void fs_load(void)
{
    char dir[4400], p[4700];
    DIR *d;
    struct dirent *e;
    fs_n = 0;
    snprintf(dir, sizeof dir, "%s%s%s", A.usb_mnt, cwd[0] ? "/" : "", cwd);
    if (!A.usb_conn || !(d = opendir(dir))) return;
    while ((e = readdir(d))) {
        struct stat st;
        fs_item *f, *g = grow(fs, &fs_cap, fs_n + 1, sizeof *fs);
        if (!g) break;
        fs = g; f = &fs[fs_n];
        if (e->d_name[0] == '.') continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        if (stat(p, &st)) continue;
        scpy(f->name, sizeof f->name, e->d_name);
        f->size = (long long)st.st_size; f->items = 0;
        if (S_ISDIR(st.st_mode)) {
            DIR *s = opendir(p);
            struct dirent *x;
            f->type = T_DIR;
            if (s) { while ((x = readdir(s))) if (x->d_name[0] != '.') f->items++; closedir(s); }
        } else f->type = type_of(e->d_name);
        fs_n++;
    }
    closedir(d);
    if (fs_n) qsort(fs, (size_t)fs_n, sizeof fs[0], cmp_fs);
}
static void size_str(char *b, size_t n, long long s)
{
    if (s >= 1000000000LL) snprintf(b, n, "%.1f GB", s / 1e9);
    else if (s >= 1000000) snprintf(b, n, "%.1f MB", s / 1e6);
    else if (s >= 1000) snprintf(b, n, "%d KB", (int)(s / 1000));
    else snprintf(b, n, "%d B", (int)s);
}
/* index in the music/video/photo list by full path (to open a file from Other) */
static int find_path(int type, const char *name)
{
    char p[4500];
    int i;
    snprintf(p, sizeof p, "%s%s%s/%s", A.usb_mnt, cwd[0] ? "/" : "", cwd, name);
    if (type == T_AUDIO) { for (i = 0; i < NTRACKS; i++) if (!strcmp(TRACKS[i].path, p)) return i; }
    else if (type == T_VIDEO) { for (i = 0; i < NVIDEOS; i++) if (!strcmp(VIDEOS[i].path, p)) return i; }
    else if (type == T_IMAGE) { for (i = 0; i < NIMAGES; i++) if (!strcmp(IMAGES[i].path, p)) return i; }
    return -1;
}

static tween c_op, c_dx, v_op, v_k, v_cur;             /* viewer: the new photo fades in over the old (.4s) */
static int icon_app = -1, v_icon;                       /* v21: viewer of the program's icon.png (v_icon - what the viewer shows) */
static gfx_tex checker;                                 /* icon viewer background: 16px checker #141418 on #0B0C0F */
static int viewing(void) { return A.usb_img >= 0 || icon_app >= 0; }
static scroller sc;
static float last_off = -1;
static int last_vimg = -2, slot_row[16], v_prev = -1;
#define ROWW (A.usb_path == 4 ? CW : CW - 14)   /* Music/Videos lists: padding-right 6 + bar 8 */

static int list_n(void) { return A.usb_path == 1 ? NTRACKS : A.usb_path == 2 ? NVIDEOS : A.usb_path == 4 ? fs_n : 0; }
static float content_h(void) { return A.usb_path == 3 ? ((NIMAGES + 3) / 4) * IMG_PITCH - 18 : list_n() * 74 - 4; }

static void title(char *t, size_t tn, char *c, size_t cn)
{
    static const char *T[4] = { "USB Files", "Music", "Videos", "Images" };
    if (A.usb_path < 4) {
        snprintf(t, tn, "%s", T[A.usb_path]);
        if (A.usb_path) snprintf(c, cn, tr("USB drive \xE2\x80\xBA %s"), tr(T[A.usb_path])); else snprintf(c, cn, "USB drive");
    } else {
        const char *last = strrchr(cwd, '/');
        scpy(t, tn, cwd[0] ? (last ? last + 1 : cwd) : "Other");
        if (cwd[0]) {                                   /* path: "USB drive › Other › a › b" */
            char p[600];
            size_t i, o = 0, len = strlen(cwd), from = 0;
            if (len > 120) {                            /* deep - show the end of the path: "… › x › y" */
                const char *q = strchr(cwd + len - 120, '/');
                from = q ? (size_t)(q - cwd) + 1 : len - 120;
                memcpy(p, "\xE2\x80\xA6 \xE2\x80\xBA ", 7); o = 7;
            }
            for (i = from; cwd[i] && o + 6 < sizeof p; i++) {
                if (cwd[i] == '/') { memcpy(p + o, " \xE2\x80\xBA ", 5); o += 5; } else p[o++] = cwd[i];
            }
            p[o] = 0;
            snprintf(c, cn, from ? "%s" : tr("USB drive \xE2\x80\xBA Other \xE2\x80\xBA %s"), p);
        } else snprintf(c, cn, "USB drive \xE2\x80\xBA Other");
    }
}

static void layout(void)
{
    int i, n = list_n();
    set_hit(H_LEFT, PX, HC - 26, 52, 52);
    if (!A.usb_conn) return;                         /* v14: "No USB drive" - close only */
    if (viewing()) {                                 /* the photo viewer covers the screen */
        float sw = 22 + 20 + 10 + t_width(700, 20, "Set as wallpaper") + 22;
        set_hit(H_VCLOSE, ST_X + 18, ST_Y + 18, 52, 52);
        if (icon_app >= 0) return;                   /* icon: no wallpaper and no paging (viewGallery) */
        set_hit(H_VSET, ST_X + ST_W - 18 - sw, ST_Y + 18, sw, 52);
        set_hit(H_VPREV, ST_X + 18, ST_Y + ST_H * 0.5f - 32, 64, 64);
        set_hit(H_VNEXT, ST_X + ST_W - 18 - 64, ST_Y + ST_H * 0.5f - 32, 64, 64);
        return;
    }
    set_hit(H_EJECT, PX + CW - 52, HC - 26, 52, 52);
    for (i = 0; i < 16; i++) slot_row[i] = -1;
    if (A.usb_path == 0) for (i = 0; i < 4; i++) set_hit(H_F0 + i, PX + i * (COL4 + 18), CY, COL4, 190);
    else if (A.usb_path == 3) {                      /* photo grid, 4 per row, scrolling */
        int first = (int)(sc.off / IMG_PITCH) * 4;
        set_scroll(H_LIST, PX, CY, CW, CH);
        for (i = first; i < NIMAGES && i < first + 16; i++) {
            float y0 = CY + (i / 4) * IMG_PITCH - sc.off, y1 = y0 + 158 + t_lh(600, 18);
            if (y0 < CY) y0 = CY;
            if (y1 > CY + CH) y1 = CY + CH;
            if (y1 <= y0) continue;
            slot_row[i % 16] = i;
            set_hit(H_I0 + i % 16, PX + (i % 4) * (COL4 + 18), y0, COL4, y1 - y0);
        }
    } else {
        int first = (int)(sc.off / 74), r;
        set_scroll(H_LIST, PX, CY, CW, CH);
        for (r = first; r < n && r < first + 16; r++) {
            float y0 = CY + r * 74 - sc.off, y1 = y0 + 70;
            if (y0 < CY) y0 = CY;
            if (y1 > CY + CH) y1 = CY + CH;
            if (y1 <= y0) continue;
            slot_row[r % 16] = r;
            set_hit(H_R0 + r % 16, PX, y0, ROWW, y1 - y0);
        }
    }
}

static void draw_static(void)
{
    char t[128], c[600];
    float lt = t_lh(800, 28), lc = t_lh(600, 18), y, ls = t_lh(600, 18);
    panel(ST_X, ST_Y, ST_W, ST_H, PANEL_R);
    if (!A.usb_conn) {   /* v14 usbNone: "USB Files" and centered circle 96, title, caption */
        float l1 = t_lh(800, 34), l2 = t_lh(600, 21), h = 96 + 12 + 6 + l1 + 12 + l2, top = PY + 52;
        float cx = PX + CW * 0.5f;
        y = top + (PBOT - top - h) * 0.5f;
        screen_head(PX + 66, PY + 26, 0x2F7BF6, "app_usb_w22", "USB Files", C_TEXT);   /* v26: tile + 28 */
        rrect(cx - 48, y, 96, 96, 48, hexa(0x2F7BF6, 0.16f));
        ico("usb_full_w18", cx, y + 48, 46, hexc(0x2F7BF6));
        y += 96 + 12 + 6;
        t_center(800, 34, cx, y + l1 * 0.5f, "No USB drive", C_TEXT);
        t_center(600, 21, cx, y + l1 + 12 + l2 * 0.5f, "Insert a USB drive to browse music, videos, photos and files.", C_SUB);
        return;
    }
    rrect(PX + 66, HC - 21, 42, 42, 12, hexc(0x2F7BF6));
    ico("usb_full", PX + 87, HC, 22, C_WHITE);
    title(t, sizeof t, c, sizeof c);
    y = HC - (lt + 2 + lc) * 0.5f;
    t_fit(800, 28, PX + 122, y + lt * 0.5f, CW - 122 - 66 - 300 - 14, t, C_TEXT);
    t_fit(600, 18, PX + 122, y + lt + 2 + lc * 0.5f, CW - 122 - 66 - 300 - 14, c, C_SUB);
    y = HC - (ls + 8 + 6) * 0.5f;                  /* used on the stick (real), on the right - Eject 52 + gap 14 */
    {
        char fr[48];
        float sx = PX + CW - 66 - 300, used = A.usb_total_gb > 0 ? 1 - A.usb_free_gb / A.usb_total_gb : 0;
        snprintf(fr, sizeof fr, tr("%.1f GB free of %.0f GB"), A.usb_free_gb, A.usb_total_gb);
        t_top(600, 18, sx, y, "USB drive", C_SUB, 0);
        t_right(600, 18, sx + 300, y + ls * 0.5f, fr, C_SUB);
        rrect(sx, y + ls + 8, 300, 6, 3, C_TRACK);
        if (used > 0) rrect(sx, y + ls + 8, 300 * used, 6, 3, C_ACCENT);   /* v19 */
    }
}

/* list row: icon 50, name/caption, text and a 44 button (play or >) on the right */
static void file_row(int id, float y, uint32_t color, const char *icon, const char *name, const char *meta,
                     const char *right, int rsize, int act, int cur, double now)
{
    float x = PX, w = ROWW, h = 70, k = press_box(id, &x, &y, &w, &h, now);
    float rw = t_width(600, (float)rsize, right), h1 = t_lh(700, 22), h2 = t_lh(500, 18);
    float ty = y + h * 0.5f - (h1 + 2 + h2) * 0.5f, bx = x + w - (14 + 44) * k;
    col tc = col_alpha(C_TEXT, 1 - press_dim(k));
    if (cur) rrect(x, y, w, h, 18 * k, C_INNER);
    rrect(x + 14 * k, y + 10 * k, 50 * k, 50 * k, 14 * k, hexc(color));
    ico(icon, x + 39 * k, y + h * 0.5f, 24 * k, C_WHITE);
    t_fit(700, 22, x + (14 + 50 + 16) * k, ty + h1 * 0.5f, bx - 16 - rw - 16 - (x + 80 * k), name, tc);
    t_fit(500, 18, x + (14 + 50 + 16) * k, ty + h1 + 2 + h2 * 0.5f, bx - 16 - rw - 16 - (x + 80 * k), meta, C_SUB);
    t_right(600, (float)rsize, bx - 16, y + h * 0.5f, right, C_SUB);
    if (act) rrect(bx, y + h * 0.5f - 22 * k, 44 * k, 44 * k, 22 * k, C_INNER);
    if (act == 1) ico("chev_r_w24", bx + 22 * k, y + h * 0.5f, 20 * k, tc);
    else if (act == 2) ico("play", bx + 22 * k, y + h * 0.5f, 18 * k, tc);
}

static void scrollbar(void)
{
    if (sc.max > 0) {
        float th = fmaxf(30, CH * CH / (CH + sc.max));
        rrect(PX + CW - 8, CY + (CH - th) * sc.off / sc.max, 8, th, 4, C_TRACK);
    }
}

static void draw_content(double now)
{
    static const char *FN[4] = { "Music", "Videos", "Images", "Other" };
    static const uint32_t FC[4] = { 0xFF4F6A, 0x7B61FF, 0xC2410C, 0x5E5E66 };
    char a[200], b[200], c[24];
    int i, n = list_n();
    if (A.usb_path == 0) {
        for (i = 0; i < 4; i++) {
            float x = PX + i * (COL4 + 18), y = CY, w = COL4, h = 190, k = press_box(H_F0 + i, &x, &y, &w, &h, now);
            float l1 = t_lh(800, 24), l2 = t_lh(600, 19);
            col tc = col_alpha(C_TEXT, 1 - press_dim(k));
            if (i == 0) snprintf(a, sizeof a, tr_n("%d tracks", (long)(NTRACKS)), NTRACKS); else if (i == 1) snprintf(a, sizeof a, tr_n("%d videos", (long)(NVIDEOS)), NVIDEOS);
            else if (i == 2) snprintf(a, sizeof a, tr_n("%d photos", (long)(NIMAGES)), NIMAGES); else snprintf(a, sizeof a, "All files on drive");
            rrect_b(x, y, w, h, 24 * k, C_INNER, C_BORDER);
            rrect(x + 24 * k, y + 24 * k, 64 * k, 64 * k, 18 * k, hexc(FC[i]));
            ico("app_usb_w19", x + 56 * k, y + 56 * k, 32 * k, C_WHITE);
            t_top(800, 24, x + 24 * k, y + h - 24 * k - l1 - 4 - l2, FN[i], tc, 0);
            t_top(600, 19, x + 24 * k, y + h - 24 * k - l2, media_scanning() && i < 3 ? "Scanning\xE2\x80\xA6" : a, C_SUB, 0);
            ico("chev_r_w24", x + w - 24 * k - 13 * k, y + h - 24 * k - 13 * k, 26 * k, tc);
        }
    } else if (A.usb_path == 3) {
        int first = (int)(sc.off / IMG_PITCH) * 4;
        clip_push(PX - 10, CY, CW + 20, CH);
        scrollbar();
        for (i = first; i < NIMAGES && i < first + 16; i++) {
            float x = PX + (i % 4) * (COL4 + 18), y = CY + (i / 4) * IMG_PITCH - sc.off, w = COL4, h = 150;
            float k = press_box(H_I0 + i % 16, &x, &y, &w, &h, now);
            gfx_tex *t;
            if (y > CY + CH) break;
            t = media_thumb(i);
            if (A.scene == SCENE_CUSTOM && A.custom_img == i) rrect_b(x - 6, y - 6, w + 12, h + 12, 24, hexa(0, 0), C_TEXT), rrect_b(x - 5, y - 5, w + 10, h + 10, 23, hexa(0, 0), C_TEXT), rrect_b(x - 4, y - 4, w + 8, h + 8, 22, hexa(0, 0), C_TEXT);
            /* no shadow under photos (design: 0 8px 20px) - expensive for the GPU when scrolling, DEVIATIONS */
            if (t) {
                float ar = w / h, ia = (float)t->w / t->h, uw = ar < ia ? ar / ia : 1, vh = ar < ia ? 1 : ia / ar;
                gfx_image_round(t, D(x), D(y), D(w), D(h), 0.5f - uw * 0.5f, 0.5f - vh * 0.5f, 0.5f + uw * 0.5f, 0.5f + vh * 0.5f,
                                col_mix(C_WHITE, hexc(0), press_dim(k)), D(18 * k));
            } else rrect(x, y, w, h, 18 * k, C_INNER);              /* still loading */
            t_fit(600, 18, x + 4, y + h + 8 + t_lh(600, 18) * 0.5f, w - 8, IMAGES[i].name, C_SUB);
        }
        clip_pop();
    } else {
        int first = (int)(sc.off / 74);
        clip_push(PX - 10, CY, CW + 20, CH);
        scrollbar();
        for (i = first; i < n && i < first + 16; i++) {
            float y = CY + i * 74 - sc.off;
            if (y > CY + CH) break;
            if (A.usb_path == 1) {
                const char *fn = strrchr(TRACKS[i].path, '/');
                snprintf(b, sizeof b, "%s%s%s", TRACKS[i].a, TRACKS[i].al[0] ? " \xC2\xB7 " : "", TRACKS[i].al);
                if (TRACKS[i].len > 0) fmt_time(c, sizeof c, TRACKS[i].len); else c[0] = 0;
                file_row(H_R0 + i % 16, y, 0xFF4F6A, "app_music_w19", fn ? fn + 1 : TRACKS[i].t, b, c, 19, 2, A.src_usb && A.trk == i, now);
            } else if (A.usb_path == 2) {
                size_str(b, sizeof b, VIDEOS[i].size);
                snprintf(a, sizeof a, tr("Video file \xC2\xB7 %.40s"), b);
                if (VIDEOS[i].len > 0) fmt_time(c, sizeof c, VIDEOS[i].len); else c[0] = 0;
                file_row(H_R0 + i % 16, y, 0x7B61FF, "app_video_w19", VIDEOS[i].name, a, c, 19, 2, A.vsel == i, now);
            } else {
                const fs_item *f = &fs[i];
                if (f->type == T_DIR) snprintf(b, sizeof b, tr_n("%d items", (long)(f->items)), f->items); else size_str(b, sizeof b, f->size);
                file_row(H_R0 + i % 16, y, TYPE_COL[f->type], TYPE_ICON[f->type], f->name, b, TYPE_KIND[f->type], 18, TYPE_ACT[f->type], 0, now);
            }
        }
        clip_pop();
    }
}

static void glass_btn(float x, float y, float w, float h, float a, double now, int id)
{
    float k = press_box(id, &x, &y, &w, &h, now);
    rrect_b(x, y, w, h, h * 0.5f, hexa(0x101014, a), hexa(0xFFFFFF, 0.14f));
    (void)k;
}

/* full-screen photo: whole (up to 1280), while loading - thumbnail */
static void viewer_img(int i, float o)
{
    gfx_tex *t;
    if (i < 0 || i >= NIMAGES || o < 0.005f) return;
    if (!(t = media_full(i)) && !(t = media_thumb(i))) return;
    {
        float ar = (float)ST_W / ST_H, ia = (float)t->w / t->h, uw = ar < ia ? ar / ia : 1, vh = ar < ia ? 1 : ia / ar;
        gfx_image_round(t, D(ST_X), D(ST_Y), D(ST_W), D(ST_H), 0.5f - uw * 0.5f, 0.5f - vh * 0.5f, 0.5f + uw * 0.5f,
                        0.5f + vh * 0.5f, hexa(0xFFFFFF, o), D(PANEL_R));
    }
}

/* program icon: checker, tile 256 (shadow 0 30 80 .5) */
static void viewer_icon(const lua_app *a)
{
    if (!checker.id) {                                   /* checker in screen pixels, once */
        int w = (int)D(ST_W), h = (int)D(ST_H), x, y;
        unsigned char *px = malloc((size_t)w * h * 4);
        if (px) {
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    unsigned char *o = px + ((size_t)y * w + x) * 4;
                    int dark = ((int)(x / D(16)) + (int)(y / D(16))) % 2 == 0;
                    o[0] = dark ? 0x14 : 0x0B; o[1] = dark ? 0x14 : 0x0C; o[2] = dark ? 0x18 : 0x0F; o[3] = 255;
                }
            gfx_tex_rgba(&checker, w, h, px);
            free(px);
        }
    }
    if (checker.id) gfx_image_round(&checker, D(ST_X), D(ST_Y), D(ST_W), D(ST_H), 0, 0, 1, 1, C_WHITE, D(PANEL_R));
    else rrect(ST_X, ST_Y, ST_W, ST_H, PANEL_R, hexc(0x0B0C0F));
    if (!a) return;
    {
        float cx = ST_X + ST_W * 0.5f - 128, cy = ST_Y + ST_H * 0.5f - 128;
        shadow(cx, cy, 256, 256, 64, 80, 0, 30, hexa(0, 0.5f));
        if (a->icon.id) gfx_image(&a->icon, D(cx), D(cy), D(256), D(256), 0, 0, 1, 1, C_WHITE);
    }
}

static void draw_viewer(double now)
{
    float op = tw_get(&v_op, now), s = 0.96f + 0.04f * tw_get(&v_k, now);
    int i, cur = A.usb_img >= 0 ? A.usb_img : A.usb_last, wall = A.usb_img >= 0 && A.scene == SCENE_CUSTOM && A.custom_img == A.usb_img;
    const lua_app *ia = lua_apps_get(icon_app);
    char cnt[96];
    const char *name;
    if (op < 0.005f || (!v_icon && (cur < 0 || cur >= NIMAGES))) return;
    gfx_xform_push(D(ST_X + ST_W * 0.5f), D(ST_Y + ST_H * 0.5f), s, 0, op);
    rrect(ST_X, ST_Y, ST_W, ST_H, PANEL_R, hexc(0));
    if (v_icon) viewer_icon(ia);
    else {
        viewer_img(v_prev, 1);                         /* photo change: background .4s */
        viewer_img(cur, tw_get(&v_cur, now));
    }
    {
        float x = ST_X + 18, y = ST_Y + 18, w = 52, h = 52, k;
        glass_btn(x, y, w, h, 0.5f, now, H_VCLOSE);
        k = press_box(H_VCLOSE, &x, &y, &w, &h, now);
        ico("close_w22", x + w * 0.5f, y + h * 0.5f, 24 * k, hexc(0xF5F5F7));
    }
    {
        float nw, x = ST_X + 18 + 52 + 12, y = ST_Y + 18 + 4;
        if (v_icon) {                                /* "icon.png", "<program> · 256×256" */
            name = "icon.png";
            if (ia) snprintf(cnt, sizeof cnt, "%s \xC2\xB7 %d\xC3\x97%d", ia->name, ia->icon.w, ia->icon.h); else cnt[0] = 0;
        } else { name = NULL; snprintf(cnt, sizeof cnt, "%d / %d", cur + 1, NIMAGES); }   /* photo: no name (user) */
        nw = name ? fminf(t_width(700, 20, name), 520) : 0;
        rrect_b(x, y, 18 + (name ? nw + 10 : 0) + t_width(600, 20, cnt) + 18, 44, 22, hexa(0x101014, 0.5f), hexa(0xFFFFFF, 0.14f));
        if (name) t_fit(700, 20, x + 18, y + 22, nw, name, hexc(0xF5F5F7));
        t_left(600, 20, x + 18 + (name ? nw + 10 : 0), y + 22, cnt, hexc(0xC7C7CC));
    }
    if (v_icon) { gfx_xform_pop(); return; }
    {
        const char *lb = wall ? "Current wallpaper" : "Set as wallpaper";
        float w = 22 + 20 + 10 + t_width(700, 20, lb) + 22, x = ST_X + ST_W - 18 - w, y = ST_Y + 18, h = 52;
        float k = press_box(H_VSET, &x, &y, &w, &h, now);
        col fg = wall ? hexc(0xF5F5F7) : hexc(0);
        rrect_b(x, y, w, h, 26 * k, wall ? hexa(0x101014, 0.5f) : col_mix(hexc(0xF5F5F7), hexc(0), press_dim(k)), hexa(0xFFFFFF, 0.14f));
        ico(wall ? "check_w26" : "photo", x + (22 + 10) * k, y + h * 0.5f, 20 * k, fg);
        t_left(700, 20, x + (22 + 20 + 10) * k, y + h * 0.5f, lb, fg);
    }
    for (i = 0; i < 2; i++) {
        float x = i ? ST_X + ST_W - 18 - 64 : ST_X + 18, y = ST_Y + ST_H * 0.5f - 32, w = 64, h = 64, k;
        rrect_b(x, y, w, h, 32, hexa(0x101014, 0.45f), hexa(0xFFFFFF, 0.14f));
        k = press_box(H_VPREV + i, &x, &y, &w, &h, now);
        ico(i ? "chev_r" : "chev_l", x + w * 0.5f, y + h * 0.5f, 28 * k, hexc(0xF5F5F7));
    }
    gfx_xform_pop();
}

static void draw_dyn(double now)
{
    float o = tw_get(&c_op, now), dx = tw_get(&c_dx, now), k = press_k(H_LEFT, now);
    rrect(PX + 26 - 26 * k, HC - 26 * k, 52 * k, 52 * k, 26 * k, C_INNER);
    if (!A.usb_conn) { ico("close", PX + 26, HC, 24 * k, col_alpha(C_TEXT, 1 - press_dim(k))); return; }
    {
        float ke = press_k(H_EJECT, now), ex = PX + CW - 26;
        rrect(ex - 26 * ke, HC - 26 * ke, 52 * ke, 52 * ke, 26 * ke, C_INNER);
        ico("eject", ex, HC, 24 * ke, col_alpha(C_TEXT, 1 - press_dim(ke)));
    }
    if (A.usb_path == 0) ico("close", PX + 26, HC, 24 * k, col_alpha(C_TEXT, 1 - press_dim(k)));
    else ico("chev_l", PX + 26, HC, 26 * k, col_alpha(C_TEXT, 1 - press_dim(k)));
    if (o > 0.005f) {
        gfx_xform_push(0, 0, 1, 0, o);
        gfx_xform_push(D(dx), 0, 1, 0, 1);
        draw_content(now);
        gfx_xform_pop();
        gfx_xform_pop();
    }
    draw_viewer(now);
}

static void list_changed(void)
{
    sc.max = fmaxf(0, content_h() - CH);
    if (sc.off > sc.max) sc.off = sc.max;
}

/* directory change: content leaves instantly and slides in from the right after 30 ms (opacity .35s, x 28 -> 0 .5s) */
static void anim_in(double now)
{
    if (A.usb_path == 4) fs_load();
    tw_init(&c_op, 0); tw_init(&c_dx, 28);
    tw_set_delay(&c_op, 1, 350, 30, &EASE, now);
    tw_set_delay(&c_dx, 0, 500, 30, &ENTER, now);
    sc.off = 0; sc.v = 0; sc.drag = 0;
    list_changed();
    screen_dirty();
    layout_again();
}

static void viewer_state(double now)
{
    tw_set(&v_op, viewing() ? 1 : 0, 350, &EASE, now);
    tw_set(&v_k, viewing() ? 1 : 0, 500, &ENTER, now);
}

static void show_img(int i, double now)
{
    if (i < 0 || i >= NIMAGES) return;
    v_prev = A.usb_img >= 0 ? A.usb_img : -1;
    A.usb_img = A.usb_last = i; icon_app = -1; v_icon = 0;
    tw_init(&v_cur, v_prev >= 0 ? 0 : 1);
    tw_set(&v_cur, 1, 400, &EASE, now);
    viewer_state(now);
    layout_again();
}

static void update(double now)
{
    static int last_usb = -1;
    if (A.usb_conn != last_usb) {                    /* stick inserted/removed - back to root */
        last_usb = A.usb_conn;
        if (!A.usb_conn) { A.usb_path = 0; A.usb_img = -1; icon_app = -1; cwd[0] = 0; A.other_depth = 0; }
        screen_dirty(); layout_again(); damage_d(ST_X, ST_Y, ST_W, ST_H);
    }
    if (media_gen() != fs_gen) {                     /* stick lists updated */
        fs_gen = media_gen();
        if (A.usb_path == 4) fs_load();
        list_changed();
        screen_dirty(); layout_again(); damage_d(ST_X, ST_Y, ST_W, ST_H);
    }
    if (A.usb_img != last_vimg) { last_vimg = A.usb_img; viewer_state(now); layout_again(); }
    if (sc_update(&sc, now) || sc.off != last_off) { last_off = sc.off; damage_d(ST_X, CY - 10, ST_W, CH + 20); layout_again(); }
    if (tw_active(&c_op, now) || tw_active(&c_dx, now) || tw_active(&v_op, now) || tw_active(&v_k, now)
        || tw_active(&v_cur, now) || press_active(now))
        damage_d(ST_X, ST_Y, ST_W, ST_H);
}

static void open_file(const fs_item *f, double now)
{
    int i = find_path(f->type, f->name);
    if (f->type == T_AUDIO && i >= 0) { np_source(1, now); A.trk = i; A.pos = 0; A.playing = 1; A.m_loaded = 1; go_screen(S_MUSIC, now); }
    else if (f->type == T_VIDEO && i >= 0) { A.vsel = i; A.vpos = 0; A.vplaying = 1; go_screen(S_VIDEO, now); }
    else if (f->type == T_IMAGE && i >= 0) show_img(i, now);
    else if (f->type == T_LUA) app_run(lua_apps_find_cwd(), now);
    else if (f->type == T_ASSET) {                    /* program's icon.png - own viewer */
        icon_app = lua_apps_find_cwd(); A.usb_img = -1; v_icon = 1;
        viewer_state(now); layout_again();
    }
    else if (f->type == T_MANIFEST) hud_show(7, f->name, now);
    else hud_show(7, f->name, now);
}

static void click(int id, double now)
{
    if (id == H_EJECT) { usb_eject(now); return; }
    if (id == H_LEFT && !A.usb_conn) { close_screen(now); return; }
    if (id == H_LEFT) {
        if (A.usb_path == 0) {
            int p = A.prev_screen;
            A.usb_img = -1; icon_app = -1;
            go_screen(p >= 0 && p != S_USB ? p : S_APPS, now);
            return;
        }
        if (A.usb_path == 4 && cwd[0]) {             /* Other: up one level */
            char *s = strrchr(cwd, '/');
            if (s) *s = 0; else cwd[0] = 0;
            A.other_depth--;
        } else { A.usb_img = -1; A.usb_path = 0; }
        icon_app = -1;
        anim_in(now);
    } else if (id >= H_F0 && id < H_F0 + 4) {
        A.usb_path = 1 + id - H_F0;
        if (A.usb_path == 4) { cwd[0] = 0; A.other_depth = 0; }
        anim_in(now);
    } else if (id >= H_I0 && id < H_I0 + 16 && slot_row[id - H_I0] >= 0) show_img(slot_row[id - H_I0], now);
    else if (id >= H_R0 && id < H_R0 + 16 && slot_row[id - H_R0] >= 0) {
        int i = slot_row[id - H_R0];
        if (A.usb_path == 1) { np_source(1, now); A.trk = i; A.pos = 0; A.playing = 1; A.m_loaded = 1; go_screen(S_MUSIC, now); }
        else if (A.usb_path == 2) { A.vsel = i; A.vpos = 0; A.vplaying = 1; go_screen(S_VIDEO, now); }
        else if (fs[i].type == T_DIR) {              /* Other: into the folder */
            size_t l = strlen(cwd);
            if (l + 1 + strlen(fs[i].name) + strlen(A.usb_mnt) + 2 >= sizeof cwd) { hud_show(7, fs[i].name, now); return; }   /* path longer than PATH_MAX */
            snprintf(cwd + l, sizeof cwd - l, "%s%s", l ? "/" : "", fs[i].name);
            A.other_depth++;
            anim_in(now);
        } else open_file(&fs[i], now);
    } else if (id == H_VCLOSE) { A.usb_img = -1; icon_app = -1; viewer_state(now); layout_again(); }
    else if (id == H_VPREV) show_img((A.usb_img + NIMAGES - 1) % NIMAGES, now);
    else if (id == H_VNEXT) show_img((A.usb_img + 1) % NIMAGES, now);
    else if (id == H_VSET) { if (A.usb_img >= 0) set_custom_wallpaper(A.usb_img, now); }
}

static void drag(int id, float x, float y, int phase, double now)
{
    (void)x;
    if (id == H_LIST) sc_drag(&sc, y, phase, now);
}

static void enter(double now)
{
    tw_init(&c_op, 1); tw_init(&c_dx, 0);
    if (A.usb_path == 4) fs_load();
    sc.off = 0; sc.v = 0; sc.drag = 0;
    list_changed();
    last_off = -1; last_vimg = A.usb_img;
    v_prev = -1; tw_init(&v_cur, 1);
    viewer_state(now);
    tw_init(&v_op, v_op.to); tw_init(&v_k, v_k.to);
}

void usb_shutdown(void) { gfx_tex_free(&checker); }

const screen_def SCR_USB = { layout, draw_static, draw_dyn, update, click, enter, drag };
