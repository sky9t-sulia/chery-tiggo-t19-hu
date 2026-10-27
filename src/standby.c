/* "Screen off" screen (design v27, Standby: car power button). Black background; three blurred blobs in the colors of the current wallpaper
 * (mix-blend screen, drifting slowly: sbA 26 s, sbB 32 s, sbC 22 s, alternate), vignette, clock 96 centered
 * (no drift - owner), Wake screen button, dimming by brightness. Drawn at 15 frames/s (ui.c). */
#include "ui_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static gfx_tex blob, vign;
float sb_btn[2];                                            /* where the "Wake screen" button is (design px), for touches */
static uint32_t pal[3] = { 0x0A84FF, 0x7B61FF, 0xFF4F6A };

/* scene colors - as in the design (this.SC: gradient colors with brightness > 40, first three; if short, repeat the first) */
static const uint32_t SCENE_PAL[N_SCENE][3] = {
    { 0xFF6A3D, 0x5B3CFF, 0xC2185B }, { 0x0077B6, 0x00B4A0, 0x023E8A }, { 0x2EE59D, 0x7B2FF7, 0x0F4C81 },
    { 0x2D6A4F, 0x95A85A, 0x1B4332 }, { 0x3A3D44, 0x2A2D34, 0x3A3D44 }, { 0xFF9E3D, 0xE0457B, 0x6A1B9A },
    { 0x00C2D1, 0x1565C0, 0x26A69A }, { 0x1E2A78, 0x3949AB, 0x1E2A78 }, { 0xD84315, 0x8E1B1B, 0xFF7043 },
    { 0xF06292, 0x8E24AA, 0xFF8A80 }, { 0xC8A46B, 0x8D6E63, 0xE0B98A },
};

static float lum(uint32_t c) { return 0.3f * (c >> 16) + 0.59f * ((c >> 8) & 255) + 0.11f * (c & 255); }

/* custom wallpaper (image): the three most frequent bright (brightness > 40, as in the design) and mutually dissimilar colors */
static void palette_from_wall(void)
{
    enum { W = 32, H = 18 };
    unsigned char px[W * H * 4];
    int cnt[4096], i, k, n = 0;
    if (gfx_tex_sample(gfx_get_wallpaper(), W, H, px)) return;
    memset(cnt, 0, sizeof cnt);
    for (i = 0; i < W * H; i++) cnt[(px[i * 4] >> 4) << 8 | (px[i * 4 + 1] >> 4) << 4 | px[i * 4 + 2] >> 4]++;
    while (n < 3) {
        int best = -1;
        for (i = 0; i < 4096; i++) {
            uint32_t c = (uint32_t)(((i >> 8) * 17) << 16 | (((i >> 4) & 15) * 17) << 8 | (i & 15) * 17);
            if (!cnt[i] || lum(c) <= 40) continue;
            for (k = 0; k < n; k++) {                       /* dissimilar to those already taken */
                int dr = (int)(pal[k] >> 16) - (int)(c >> 16), dg = (int)((pal[k] >> 8) & 255) - (int)((c >> 8) & 255), db = (int)(pal[k] & 255) - (int)(c & 255);
                if (dr * dr + dg * dg + db * db < 60 * 60) break;
            }
            if (k < n) continue;
            if (best < 0 || cnt[i] > cnt[best]) best = i;
        }
        if (best < 0) break;
        pal[n++] = (uint32_t)(((best >> 8) * 17) << 16 | (((best >> 4) & 15) * 17) << 8 | (best & 15) * 17);
    }
    if (n == 0) {                                           /* like the design with no colors: accent, purple, pink */
        pal[0] = (uint32_t)(PAL.accent.r * 255 + 0.5f) << 16 | (uint32_t)(PAL.accent.g * 255 + 0.5f) << 8 | (uint32_t)(PAL.accent.b * 255 + 0.5f);
        pal[1] = 0x7B61FF; pal[2] = 0xFF4F6A; n = 3;
    }
    for (; n < 3; n++) pal[n] = pal[0];
}

void standby_palette(void)
{
    if (A.scene >= 0 && A.scene < N_SCENE) memcpy(pal, SCENE_PAL[A.scene], sizeof pal);
    else palette_from_wall();
}

static void make_textures(void)
{
    enum { B = 128, VW = 128, VH = 75 };
    unsigned char *p = malloc(B * B * 4), *v = malloc(VW * VH * 4);
    int x, y;
    if (!p || !v) { free(p); free(v); return; }
    for (y = 0; y < B; y++)                                 /* radial-gradient(closest-side, color -> transparent) + blur(60px) */
        for (x = 0; x < B; x++) {
            float dx = (x + 0.5f) / B * 2 - 1, dy = (y + 0.5f) / B * 2 - 1, r = sqrtf(dx * dx + dy * dy), a = 1 - r;
            a = a < 0 ? 0 : a;                              /* like CSS: linear from center to edge */
            a = a < 0.12f ? a * a / 0.24f : a - 0.06f;      /* blur(60px): fades smoothly to zero at the edge, no jump or kink */
            p[(y * B + x) * 4] = p[(y * B + x) * 4 + 1] = p[(y * B + x) * 4 + 2] = p[(y * B + x) * 4 + 3] = (unsigned char)(a * 255 + 0.5f);
        }
    for (y = 0; y < VH; y++)                                /* radial-gradient(120% 90% at 50% 40%, .55, .2 70%, .6 100%) */
        for (x = 0; x < VW; x++) {
            float dx = ((x + 0.5f) / VW - 0.5f) / 1.2f, dy = ((y + 0.5f) / VH - 0.4f) / 0.9f, r = sqrtf(dx * dx + dy * dy);
            float a = r < 0.7f ? 0.55f + (0.2f - 0.55f) * r / 0.7f : r < 1 ? 0.2f + (0.6f - 0.2f) * (r - 0.7f) / 0.3f : 0.6f;
            v[(y * VW + x) * 4] = v[(y * VW + x) * 4 + 1] = v[(y * VW + x) * 4 + 2] = 0;
            v[(y * VW + x) * 4 + 3] = (unsigned char)(a * 255 + 0.5f);
        }
    gfx_tex_rgba(&blob, B, B, p);
    gfx_tex_rgba(&vign, VW, VH, v);
    free(p); free(v);
}

/* CSS keyframes (translate in % of blob size, scale), alternate, ease-in-out on each segment */
static const float KF[3][3][3] = {
    { { -18, 10, 1 }, { 12, -14, 1.25f }, { -6, -4, 0.9f } },
    { { 20, -8, 1.1f }, { -14, 12, 0.85f }, { 8, 16, 1.2f } },
    { { 0, 18, 0.9f }, { 16, 4, 1.3f }, { -20, -10, 1 } },
};
static const float DUR[3] = { 26, 32, 22 }, POS[3][2] = { { -5, 20 }, { 35, -10 }, { 20, 35 } };

static float ease(float t) { return t < 0.5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t); }   /* ~ease-in-out */

/* op - screen visibility (0..1), glow - blobs (0..0.85), clk - clock (0..1, also scale .85 -> 1);
 * at - seconds since the screen appeared (the CSS blob animation starts at the first keyframe) */
void standby_draw(double at, float op, float glow, float clk)
{
    float W = SCREEN_W / K, H = SCREEN_H / K, cx0 = -0.1f * W, cy0 = -0.1f * H, cw = 1.2f * W, ch = 1.2f * H;
    char t[8];
    time_t tt = time(NULL);
    struct tm tm;
    int i;
    if (!blob.id) make_textures();
    rrect(0, 0, W, H, 0, hexa(0, op));
    gfx_blend_screen(1);
    for (i = 0; i < 3; i++) {
        float ph = fmodf((float)at, 2 * DUR[i]) / DUR[i], u, seg;
        float tx, ty, sc, bw = 0.7f * cw, bh = 0.9f * ch, x, y;
        int a;
        if (ph > 1) ph = 2 - ph;                            /* alternate */
        u = ph * 2; a = u >= 1 ? 1 : 0; seg = ease(u - a);
        tx = KF[i][a][0] + (KF[i][a + 1][0] - KF[i][a][0]) * seg;
        ty = KF[i][a][1] + (KF[i][a + 1][1] - KF[i][a][1]) * seg;
        sc = KF[i][a][2] + (KF[i][a + 1][2] - KF[i][a][2]) * seg;
        x = cx0 + POS[i][0] / 100 * cw + tx / 100 * bw + bw * (1 - sc) * 0.5f;
        y = cy0 + POS[i][1] / 100 * ch + ty / 100 * bh + bh * (1 - sc) * 0.5f;
        gfx_image(&blob, D(x), D(y), D(bw * sc), D(bh * sc), 0, 0, 1, 1, hexa(pal[i], glow * op));
    }
    gfx_blend_screen(0);
    gfx_image(&vign, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 1, 1, hexa(0xFFFFFF, op));
    localtime_r(&tt, &tm);
    snprintf(t, sizeof t, "%02d:%02d", tm.tm_hour, tm.tm_min);
    {   /* we skip the design's drift (sbDrift) - the clock stays put (owner: motion is distracting) */
        float dx = 0, dy = 0, s = 0.85f + 0.15f * clk;
        font *f = F(500, 96);
        float w, top = H * 0.5f - (96 + 40 + 72) * 0.5f + dy, bx = W * 0.5f - 36 + dx, by = top + 96 + 40;   /* column: clock, 40, button */
        text_tnum(1);
        w = text_width(f, t, D(-0.03f * 96));
        gfx_xform(D(W * 0.5f + dx), D(top + 48), s, 0, 1);
        text_draw(f, D(W * 0.5f + dx) - w * 0.5f, D(top + 48) - text_line_height(f) * 0.5f, t,
                  hexa(0xF5F5F7, 0.82f * clk * op), D(-0.03f * 96));
        gfx_xform_reset();
        text_tnum(0);
        /* v28: "Wake screen" button 72, rgba(255,255,255,.08), border .18, power icon 30. backdrop blur(20px) is not
         * drawn: the glass blur samples the wallpaper, but under the button are the dark Screen off blobs (DEVIATIONS.md) */
        sb_btn[0] = bx; sb_btn[1] = by;
        rrect_b(bx, by, 72, 72, 36, hexa(0xFFFFFF, 0.08f * clk * op), hexa(0xFFFFFF, 0.18f * clk * op));
        ico("wake", bx + 36, by + 36, 30, hexa(0xF5F5F7, 0.85f * clk * op));
    }
    rrect(0, 0, W, H, 0, hexa(0, (10 - A.bright) * 0.055f * op));   /* dimming by brightness */
}
