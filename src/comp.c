/* Drawing helpers and common design components. All coordinates are design px (1280x750). */
#include "ui_int.h"
#include "i18n.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

font *F(int weight, float size) { return text_font(weight, size); }   /* sizes exactly as in the design (px) */

/* t_*: UI strings are translated here (i18n.h); Lua program text bypasses this (text_draw) */
float t_width(int w, float size, const char *s) { return text_width(F(w, size), tr(s), 0) / K; }

void t_top(int w, float size, float x, float y, const char *s, col c, float ls)
{
    s = tr(s);
    text_draw(F(w, size), D(x), D(y), s, c, D(ls));
}

void t_left(int w, float size, float x, float cy, const char *s, col c)
{
    s = tr(s);
    font *f = F(w, size);
    text_draw(f, D(x), D(cy) - text_line_height(f) * 0.5f, s, c, 0);
}

void t_right(int w, float size, float xr, float cy, const char *s, col c)
{
    s = tr(s);
    font *f = F(w, size);
    text_draw(f, D(xr) - text_width(f, s, 0), D(cy) - text_line_height(f) * 0.5f, s, c, 0);
}

void t_center(int w, float size, float cx, float cy, const char *s, col c)
{
    s = tr(s);
    font *f = F(w, size);
    text_draw(f, D(cx) - text_width(f, s, 0) * 0.5f, D(cy) - text_line_height(f) * 0.5f, s, c, 0);
}

void fmt_time(char *b, size_t n, double sec)
{
    int x = sec < 0 ? 0 : (int)sec, h = x / 3600, m = (x % 3600) / 60, s = x % 60;
    if (h) snprintf(b, n, "%d:%02d:%02d", h, m, s); else snprintf(b, n, "%d:%02d", m, s);
}

/* design v35: every rounded square tile 26..112 (not a circle) - corner-shape: squircle, radius ~0.345 of the side
 * (26/9, 34/12, 38/13, 42/14, 44/15, 52/18, 56/19, 64/22, 68/24, 96/33, 112/40); exceptions are only the large ones
 * (cover 230, 256) - the rule leaves them alone. A pressed size (x k) uses the same fraction */
static const float SQ_S[] = { 26, 34, 38, 40, 42, 44, 46, 50, 52, 56, 64, 68, 76, 84, 88, 96, 112 };
static const float SQ_R[] = { 9, 12, 13, 14, 14, 15, 16, 17, 18, 19, 22, 24, 26, 29, 30, 33, 40 };
int tile_sq(float w, float h, float r, float *rr)
{
    int i, b = 0;
    if (fabsf(w - h) > 0.5f || w < 22 || w > 120 || r >= w * 0.5f - 0.25f || r <= 0) return 0;
    for (i = 1; i < (int)(sizeof SQ_S / sizeof *SQ_S); i++) if (fabsf(SQ_S[i] - w) < fabsf(SQ_S[b] - w)) b = i;
    *rr = w * SQ_R[b] / SQ_S[b];
    return 1;
}
void rrect(float x, float y, float w, float h, float r, col c)
{
    int sq = tile_sq(w, h, r, &r);
    if (sq) gfx_squircle(1);
    gfx_rect(D(x), D(y), D(w), D(h), D(r), c);
    if (sq) gfx_squircle(0);
}
void rrect_b(float x, float y, float w, float h, float r, col fill, col border)
{
    int sq = tile_sq(w, h, r, &r);
    if (sq) gfx_squircle(1);
    gfx_rect_border(D(x), D(y), D(w), D(h), D(r), fill, 1, border);
    if (sq) gfx_squircle(0);
}
void shadow(float x, float y, float w, float h, float r, float blur, float ox, float oy, col c)
{
    int sq = tile_sq(w, h, r, &r);
    if (sq) gfx_squircle(1);
    gfx_shadow(D(x), D(y), D(w), D(h), D(r), D(blur), D(ox), D(oy), c);
    if (sq) gfx_squircle(0);
}
void ico(const char *name, float cx, float cy, float size, col c) { icon_draw_c(name, D(cx), D(cy), D(size), c); }
void ico_rot(const char *name, float cx, float cy, float size, col c, float deg)
{
    icon_draw_rot(name, D(cx - size * 0.5f), D(cy - size * 0.5f), D(size), c, deg);
}

/* track cover into square s (radius r): fills the whole square, the excess is cropped about the center (as on Android;
 * owner 2026-10-26 - a non-square one looked "squeezed") */
void art_draw(const gfx_tex *t, float x, float y, float s, float r)
{
    float ia = (float)t->w / (float)t->h, uw = ia > 1 ? 1 / ia : 1, vh = ia > 1 ? 1 : ia;
    gfx_image_round(t, D(x), D(y), D(s), D(s), 0.5f - uw * 0.5f, 0.5f - vh * 0.5f, 0.5f + uw * 0.5f, 0.5f + vh * 0.5f, C_WHITE, D(r));
}

/* blurred cover (owner 2026-10-26 "like YouTube Music", by eye; DEVIATIONS): cover -> 64x64 (GPU), rows
 * flipped (glReadPixels is bottom-up), blurred (see below), stretched with linear filtering over the whole panel (with a
 * 1px inset the white glass edge showed through in the light theme); on top, darkening 45% / lightening 55% */
int art_bg_update(art_bg *b, const gfx_tex *art, long key)
{
    /* 64x64 (at 24x24 squares are visible after stretching - owner 2026-10-26); blur strength matches what 24x24 had with two
     * 3x3 passes (variance 4/3 px at 24 = ~9.5 px at 64): 5 passes of a 5-tap window, over rows and columns */
    enum { B = 64, R = 2, PASSES = 5 };
    static unsigned char px[B * B * 4], tmp[B * B * 4];
    int pass, x, y, k, d;
    if (key == b->key) return 0;
    b->key = key;
    if (b->t.id) gfx_tex_free(&b->t);
    if (!art || gfx_tex_sample(art, B, B, px)) return 1;
    for (y = 0; y < B / 2; y++) {                            /* glReadPixels is bottom-up: flip */
        memcpy(tmp, px + y * B * 4, B * 4);
        memcpy(px + y * B * 4, px + (B - 1 - y) * B * 4, B * 4);
        memcpy(px + (B - 1 - y) * B * 4, tmp, B * 4);
    }
    for (pass = 0; pass < PASSES; pass++)
        for (d = 0; d < 2; d++) {                            /* 0 - over rows, 1 - over columns */
            memcpy(tmp, px, sizeof px);
            for (y = 0; y < B; y++) for (x = 0; x < B; x++) for (k = 0; k < 3; k++) {
                int t, sum = 0, n = 0;
                for (t = -R; t <= R; t++) {
                    int sx = d ? x : x + t, sy = d ? y + t : y;
                    if (sx >= 0 && sy >= 0 && sx < B && sy < B) { sum += tmp[(sy * B + sx) * 4 + k]; n++; }
                }
                px[(y * B + x) * 4 + k] = (unsigned char)(sum / n);
            }
        }
    for (k = 3; k < B * B * 4; k += 4) px[k] = 255;
    gfx_tex_rgba(&b->t, B, B, px);
    return 1;
}

void art_bg_draw(const art_bg *b, float x, float y, float w, float h, float r)
{
    float e = 0.5f / 64, a = w / h, uw = a < 1 ? a : 1, vh = a > 1 ? 1 / a : 1;   /* square -> panel keeping
                                                                     * proportions: crop the excess about the center (a wide panel
                                                                     * Music stretched the blur - owner 2026-10-26) */
    if (!b->t.id) return;
    uw *= 1 - 2 * e; vh *= 1 - 2 * e;
    gfx_image_round(&b->t, D(x), D(y), D(w), D(h), 0.5f - uw * 0.5f, 0.5f - vh * 0.5f, 0.5f + uw * 0.5f, 0.5f + vh * 0.5f, C_WHITE, D(r));
    rrect(x, y, w, h, r, PAL.light ? hexa(0xFFFFFF, 0.55f) : hexa(0, 0.45f));
}

/* design v35 dLive (Stream radio card): badge 30 (padding 0 10, radius 10, gap 6, dot 7, LIVE 14/800 .06em,
 * white; background #E5484D playing / #5E5E66 paused), gap 12, caption 20/600 sub */
void live_row(float x, float cy, int on)
{
    font *f = F(800, 14);
    float ls = 14 * 0.06f, tw = text_width(f, "LIVE", D(ls)) / K, bw = 10 + 7 + 6 + tw + 10;
    rrect(x, cy - 15, bw, 30, 10, hexc(on ? 0xE5484D : 0x5E5E66));
    rrect(x + 10, cy - 3.5f, 7, 7, 3.5f, hexc(0xFFFFFF));
    text_draw(f, D(x + 23), D(cy) - text_line_height(f) * 0.5f, "LIVE", hexc(0xFFFFFF), D(ls));
    t_left(600, 20, x + bw + 12, cy, on ? "Streaming" : "Paused", C_SUB);
}

void panel(float x, float y, float w, float h, float r)
{
    if (PAL.panel_shadow.a > 0) shadow(x, y, w, h, r, 30, 0, 10, PAL.panel_shadow);
    gfx_glass(D(x), D(y), D(w), D(h), D(r), GLASS_TINT, GLASS_A, GLASS_EDGE);
}

float press_box(int id, float *x, float *y, float *w, float *h, double now)
{
    float k = press_k(id, now);
    if (k < 1) {
        *x += *w * (1 - k) * 0.5f; *y += *h * (1 - k) * 0.5f;
        *w *= k; *h *= k;
    }
    return k;
}

/* ---------------------------------------------------------------- segmented switch */

void seg_static(float x, float y, int n, float iw, float ih)
{
    float w = n * iw + 10, h = ih + 10;
    rrect_b(x, y, w, h, h * 0.5f, C_SEG, C_BORDER);
}

const float *seg_item_op;                   /* item opacity (inactive 0.35), NULL - all 1; reset after the call */

void seg_dyn(float x, float y, int n, float iw, float ih, float thumb, const char *const *labels,
             const char *const *icons, int weight, float size, float icon_size, int base_id, double now)
{
    int i;
    const float *op = seg_item_op;
    seg_item_op = NULL;
    rrect(x + 5 + thumb * iw, y + 5, iw, ih, ih * 0.5f, C_ACCENT);              /* v17: thumb is the accent */
    for (i = 0; i < n; i++) {
        float m = 1 - fminf(1, fabsf(thumb - (float)i));             /* 1 - under the thumb */
        col c = col_mix(C_TEXT, C_ONACCENT, m);
        float k = base_id >= 0 ? press_k(base_id + i, now) : 1;
        float cx = x + 5 + i * iw + iw * 0.5f, cy = y + 5 + ih * 0.5f;
        float tw = labels && labels[i] ? t_width(weight, size, labels[i]) : 0;
        float gap = icons && icons[i] && tw > 0 ? 8 : 0;
        float iw2 = icons && icons[i] ? icon_size : 0, total = (iw2 + gap + tw) * k;
        float x0 = cx - total * 0.5f;
        if (k < 1) c = col_alpha(c, 1 - press_dim(k));
        if (op) c = col_alpha(c, op[i]);
        if (iw2 > 0) ico(icons[i], x0 + iw2 * k * 0.5f, cy, icon_size * k, c);
        if (tw > 0) {
            font *f = F(weight, size);
            text_draw(f, D(x0 + (iw2 + gap) * k), D(cy) - text_line_height(f) * 0.5f, tr(labels[i]), c, 0);
        }
    }
}

/* ---------------------------------------------------------------- toggle */

void toggle_draw(float x, float y, float on)
{
    float kx = x + 4 + 32 * on;
    rrect(x, y, 76, 44, 22, col_mix(C_TRACK, hexc(0x2DBE6C), fminf(1, fmaxf(0, on))));
    shadow(kx, y + 4, 36, 36, 18, 8, 0, 2, hexa(0, 0.25f));
    rrect(kx, y + 4, 36, 36, 18, C_WHITE);
}

float t_lh(int w, float size) { return text_line_height(F(w, size)) / K; }

void t_fit(int w, float size, float x, float cy, float maxw, const char *s, col c)
{
    char buf[512];
    size_t n = 0, last = 0;
    float ew;
    s = tr(s);
    if (t_width(w, size, s) <= maxw) { t_left(w, size, x, cy, s, c); return; }
    ew = t_width(w, size, "\xE2\x80\xA6");                /* text-overflow: ellipsis */
    {
        float acc = 0;
        while (s[n] && n < sizeof buf - 8) {               /* longest prefix (whole UTF-8 characters) that fits */
            char ch[8];
            size_t m = n + 1;
            while (s[m] && ((unsigned char)s[m] & 0xC0) == 0x80 && m - n < 6) m++;
            if (m > sizeof buf - 8) break;
            memcpy(ch, s + n, m - n); ch[m - n] = 0;
            acc += t_width(w, size, ch);                   /* sum of advances = string width (without letter-spacing) */
            if (acc + ew > maxw) break;
            last = n = m;
        }
    }
    memcpy(buf, s, last);
    memcpy(buf + last, "\xE2\x80\xA6", 4);
    t_left(w, size, x, cy, buf, c);
}

void seg_static2(float x, float y, int n, float iw, float ih, float pad, int border)
{
    float w = n * iw + pad * 2 + (border ? 2 : 0), h = ih + pad * 2 + (border ? 2 : 0);
    if (border) rrect_b(x, y, w, h, h * 0.5f, C_SEG, C_BORDER);
    else rrect(x, y, w, h, h * 0.5f, C_SEG);
}

void seg_dyn2(float x, float y, int n, float iw, float ih, float pad, int border, float thumb,
              const char *const *labels, int weight, float size, int base_id, double now)
{
    int i;
    float o = pad + (border ? 1 : 0);
    rrect(x + o + thumb * iw, y + o, iw, ih, ih * 0.5f, C_ACCENT);
    for (i = 0; i < n; i++) {
        float m = 1 - fminf(1, fabsf(thumb - (float)i));
        float k = base_id >= 0 ? press_k(base_id + i, now) : 1;
        col c = col_mix(C_TEXT, C_ONACCENT, m);
        if (k < 1) c = col_alpha(c, 1 - press_dim(k));
        t_center(weight, size, x + o + i * iw + iw * 0.5f, y + o + ih * 0.5f, labels[i], c);
    }
}

void toggle_draw_c(float x, float y, float on, col on_col)
{
    float kx = x + 4 + 32 * on;
    rrect(x, y, 76, 44, 22, col_mix(C_TRACK, on_col, fminf(1, fmaxf(0, on))));
    shadow(kx, y + 4, 36, 36, 18, 8, 0, 2, hexa(0, 0.25f));
    rrect(kx, y + 4, 36, 36, 18, C_WHITE);
}

void slider_draw(float x, float y, float w, float frac, const char *icon, col fill)
{
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    rrect(x, y, w, 40, 20, C_TRACK);
    if (frac > 0) {                                   /* fill inside the rounded track (overflow: hidden) */
        clip_push(x, y, w * frac, 40);
        rrect(x, y, w, 40, 20, fill);
        clip_pop();
    }
    if (icon) {
        gfx_blend_difference(1);
        ico(icon, x + 14 + 10, y + 10 + 10, 20, C_WHITE);
        gfx_blend_difference(0);
    }
}

float slider_value(float x, float w, float px)
{
    float f = (px - x) / w;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

void close_btn(int id, float x, float y, double now)
{
    float w = 52, h = 52, k = press_box(id, &x, &y, &w, &h, now);
    rrect(x, y, w, h, w * 0.5f, C_INNER);
    ico("close", x + w * 0.5f, y + h * 0.5f, 24 * k, col_alpha(C_TEXT, 1 - press_dim(k)));
}

float reset_w(void) { return 18 + 20 + 10 + t_width(700, 19, "Reset to defaults") + 18; }

void reset_btn(int id, float x, float y, double now)
{
    float w = reset_w(), h = 48, k = press_box(id, &x, &y, &w, &h, now);
    col c = col_alpha(C_SUB, 1 - press_dim(k));
    rrect_b(x, y, w, h, 24 * k, hexa(0, 0), C_TRACK);
    ico("back10", x + (18 + 10) * k, y + h * 0.5f, 20 * k, c);
    t_left(700, 19, x + (18 + 20 + 10) * k, y + h * 0.5f, "Reset to defaults", c);
}

/* scrolling: drag + inertia (decay ~ like overflow-y: auto on a touch screen) */
void sc_drag(scroller *s, float y, int phase, double now)
{
    if (phase == 0) { s->drag = 1; s->v = 0; s->last_y = y; s->last_t = now; return; }
    if (phase == 1 && s->drag) {
        float dy = s->last_y - y;
        double dt = now - s->last_t;
        s->off += dy;
        if (dt > 0.001) s->v = s->v * 0.3f + (float)(dy / dt) * 0.7f;
        s->last_y = y; s->last_t = now;
    }
    if (phase == 2) { s->drag = 0; if (now - s->last_t > 0.08) s->v = 0; s->last_t = now; }
    if (s->off < 0) s->off = 0;
    if (s->off > s->max) s->off = s->max;
}

int sc_update(scroller *s, double now)
{
    float dt;
    if (s->drag || fabsf(s->v) < 5) { s->v = s->drag ? s->v : 0; return 0; }
    dt = (float)(now - s->last_t);
    s->last_t = now;
    if (dt > 0.05f) dt = 0.05f;
    s->off += s->v * dt;
    s->v *= expf(-4.0f * dt);
    if (s->off < 0) { s->off = 0; s->v = 0; }
    if (s->off > s->max) { s->off = s->max; s->v = 0; }
    return 1;
}

/* "2px dashed" border (dash 5, step 10) */
void dash_rect(float x, float y, float w, float h, float r, col c)
{
    float t;
    for (t = x + r; t < x + w - r; t += 10) { rrect(t, y, fminf(5, x + w - r - t), 2, 0, c); rrect(t, y + h - 2, fminf(5, x + w - r - t), 2, 0, c); }
    for (t = y + r; t < y + h - r; t += 10) { rrect(x, t, 2, fminf(5, y + h - r - t), 0, c); rrect(x + w - 2, t, 2, fminf(5, y + h - r - t), 0, c); }
    for (t = 0; t < 4; t++) {                          /* rounded corners as a solid arc */
        float cx = (int)t & 1 ? x + w - r : x, cy = (int)t & 2 ? y + h - r : y;
        clip_push(cx, cy, r, r);
        gfx_rect_border(D(x), D(y), D(w), D(h), D(r), hexa(0, 0), D(2), c);
        clip_pop();
    }
}

/* settings row title 17/700 and caption 13/600 c.sub (gap 2), centered on cy */
void row_text(float x, float cy, float maxw, const char *title, const char *sub)
{
    float th = t_lh(700, 21), sh = sub ? t_lh(600, 17) : 0, y = cy - (th + (sub ? 2 + sh : 0)) * 0.5f;
    t_fit(700, 21, x, y + th * 0.5f, maxw, title, C_TEXT);
    if (sub) t_fit(600, 17, x, y + th + 2 + sh * 0.5f, maxw, sub, C_SUB);
}

/* caption + value (16/700) and below them a slider 40 (margin-top 6); y - top of the row; returns the slider's y */
float slider_row_m(float x, float y, float w, float gap, const char *label, const char *val, float frac, const char *icon, col fill)
{
    float lh = t_lh(700, 20);
    t_left(700, 20, x, y + lh * 0.5f, label, C_TEXT);
    t_right(700, 20, x + w, y + lh * 0.5f, val, C_SUB);
    slider_draw(x, y + lh + gap, w, frac, icon, fill);
    return y + lh + gap;
}

float slider_row(float x, float y, float w, const char *label, const char *val, float frac, const char *icon, col fill)
{
    return slider_row_m(x, y, w, 6, label, val, frac, icon, fill);
}

/* segments without icons, c.seg without a border (small: pad 3, h40) */
void seg_plain(float x, float y, int n, float iw, float ih, float pad, float thumb, const char *const *labels,
               int size, int base_id, double now)
{
    seg_static2(x, y, n, iw, ih, pad, 0);
    seg_dyn2(x, y, n, iw, ih, pad, 0, thumb, labels, 700, (float)size, base_id, now);
}

/* panel header: [close 52] 14 [tile 42 r12] 14 [title 28/800]; x,y - top-left of the close button */
float screen_head(float x, float cy, uint32_t color, const char *icon, const char *title, col fg)
{
    rrect(x, cy - 21, 42, 42, 12, hexc(color));
    ico(icon, x + 21, cy, 22, C_WHITE);
    t_left(800, 28, x + 56, cy, title, fg);
    return x + 56 + t_width(800, 28, title);
}

void head_static(float x, float y, uint32_t color, const char *icon, float isz, const char *title)
{
    rrect(x + 66, y + 5, 42, 42, 12, hexc(color));
    ico(icon, x + 87, y + 26, isz, C_WHITE);
    t_left(800, 28, x + 122, y + 26, title, C_TEXT);
}

/* marquee: if s is wider than maxw - holds 2 s, scrolls left at 40 px/s to the end, holds 2 s, restarts (t0 - cycle start).
 * Returns 1 if the string is long (it must be redrawn every frame). dyn=0 - static ones only (for the cache layer). */
int marquee(int w, float size, float x, float y, float maxw, const char *s, col c, float ls, double t0, double now, int dyn)
{
    float tw = t_width(w, size, s);
    if (tw <= maxw) { if (!dyn) t_top(w, size, x, y, s, c, ls); return 0; }
    if (dyn) {
        double run = (tw - maxw) / 40.0, t = fmod(now - t0, 4.0 + run);
        float off = t < 2 ? 0 : t < 2 + run ? (float)((t - 2) * 40.0) : tw - maxw, lh = t_lh(w, size);
        clip_push(x, y - 4, maxw, lh + 8);
        t_top(w, size, x - off, y, s, c, ls);
        clip_pop();
    }
    return 1;
}

/* word wrap, lines centered on cx; y - top of the first line; returns the block height */
float t_wrap_center(int w, float size, float cx, float y, float maxw, const char *s, col c)
{
    char line[256];
    float lh = t_lh(w, size), h = 0;
    const char *p = s;
    while (*p) {
        size_t n = 0, last = 0;
        while (p[n] && n < sizeof line - 1) {
            memcpy(line, p, n + 1); line[n + 1] = 0;
            if (p[n] == ' ') { line[n] = 0; if (t_width(w, size, line) > maxw) break; last = n; }
            else if (!p[n + 1] && t_width(w, size, line) <= maxw) { last = n + 1; break; }
            n++;
        }
        if (!p[n]) { memcpy(line, p, n); line[n] = 0; if (t_width(w, size, line) <= maxw || !last) last = n; }
        if (!last) last = n;
        memcpy(line, p, last); line[last] = 0;
        t_center(w, size, cx, y + h + lh * 0.5f, line, c);
        h += lh;
        p += last;
        while (*p == ' ') p++;
    }
    return h;
}

/* copy of the string truncated to size, without splitting a UTF-8 character */
void scpy(char *d, size_t n, const char *s)
{
    size_t l = strlen(s);
    if (l >= n) { l = n - 1; while (l && ((unsigned char)s[l] & 0xC0) == 0x80) l--; }
    memcpy(d, s, l);
    d[l] = 0;
}

/* dynamic array: capacity doubles; the old pointer is kept if out of memory */
void *grow(void *p, int *cap, int need, size_t sz)
{
    int c = *cap ? *cap : 64;
    void *q;
    if (need <= *cap) return p;
    while (c < need) c *= 2;
    if (!(q = realloc(p, (size_t)c * sz))) return NULL;
    *cap = c;
    return q;
}
