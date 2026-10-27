/* Text from pre-baked atlases. .fnt format (little-endian, written by tools/bake_fonts.c):
 *   "HUF1", float px, float ascent, float descent, float line_gap, int count, int atlas_w, int atlas_h,
 *   count * { int cp; float x0, y0, x1, y1, s0, t0, s1, t1, adv; }   (x/y relative to the baseline) */
#include "text.h"

#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb_truetype.h"

typedef struct { int cp; float x0, y0, x1, y1, s0, t0, s1, t1, adv; } glyph;

struct font {
    int weight;
    float design_px;
    float px, ascent, descent, line_gap;
    int count;
    glyph *g;           /* sorted by cp; allocated at load, freed in text_shutdown */
    gfx_tex atlas;
};

#define MAX_FONTS 72
static font fonts[MAX_FONTS];
static int nfonts;

/* ---- fallback font (Chinese and anything missing from the baked atlases): glyphs are rasterized on first
 * appearance into a shared 1024x1024 atlas cache (GL_ALPHA). Font on the HU: /usr/share/fonts/truetype/wqy-zenhei.ttf. */
#define FB_W 1024
#define FB_H 1024
#define FB_SLOTS 4096
typedef struct { int key; glyph g; } fb_slot;     /* key = font index << 21 | cp, 0 - empty */
static fb_slot *fb_tab;
static int fb_n, fb_x, fb_y, fb_row, fb_state;    /* fb_state: 0 not loaded yet, 1 loaded, -1 no font */
static unsigned char *fb_ttf;
static stbtt_fontinfo fb_info;
static gfx_tex fb_atlas;
static char fb_dir[512];

static int fb_open(void)
{
    const char *paths[3];
    char local[600];
    int i;
    if (fb_state) return fb_state > 0;
    fb_state = -1;
    snprintf(local, sizeof local, "%s/wqy-zenhei.ttf", fb_dir);
    paths[0] = getenv("HU_FALLBACK_FONT"); paths[1] = "/usr/share/fonts/truetype/wqy-zenhei.ttf"; paths[2] = local;
    for (i = 0; i < 3 && !fb_ttf; i++) {
        FILE *fp;
        long n;
        if (!paths[i] || !(fp = fopen(paths[i], "rb"))) continue;
        fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
        fb_ttf = malloc((size_t)n);
        if (fb_ttf && fread(fb_ttf, 1, (size_t)n, fp) != (size_t)n) { free(fb_ttf); fb_ttf = NULL; }
        fclose(fp);
        if (fb_ttf && !stbtt_InitFont(&fb_info, fb_ttf, stbtt_GetFontOffsetForIndex(fb_ttf, 0))) { free(fb_ttf); fb_ttf = NULL; }
    }
    if (!fb_ttf) { fprintf(stderr, "hu: no fallback font\n"); return 0; }
    fb_tab = calloc(FB_SLOTS, sizeof(fb_slot));
    if (!fb_tab || gfx_tex_alpha(&fb_atlas, FB_W, FB_H)) { free(fb_tab); fb_tab = NULL; free(fb_ttf); fb_ttf = NULL; return 0; }
    fb_state = 1;
    return 1;
}

static void fb_reset(void)                         /* atlas full - start over */
{
    memset(fb_tab, 0, sizeof(fb_slot) * FB_SLOTS);
    fb_n = fb_x = fb_y = fb_row = 0;
}

static const glyph *fb_find(const font *f, int cp)
{
    int key = ((int)(f - fonts) + 1) << 21 | cp, h = (int)(((unsigned)key * 2654435761u) % FB_SLOTS), gi, x0, y0, x1, y1, w, hh, adv;
    float sc;
    unsigned char *bmp;
    glyph *g;
    if (!fb_open()) return NULL;
    while (fb_tab[h].key) {
        if (fb_tab[h].key == key) return fb_tab[h].g.adv > 0 || fb_tab[h].g.x1 > fb_tab[h].g.x0 ? &fb_tab[h].g : NULL;
        h = (h + 1) % FB_SLOTS;
    }
    if (fb_n > FB_SLOTS / 2) { fb_reset(); return fb_find(f, cp); }
    gi = stbtt_FindGlyphIndex(&fb_info, cp);
    fb_tab[h].key = key; fb_n++;
    g = &fb_tab[h].g;
    memset(g, 0, sizeof *g);
    g->cp = cp;
    if (!gi) return NULL;                          /* not in the fallback either - remember that it is absent */
    sc = stbtt_ScaleForMappingEmToPixels(&fb_info, f->px);   /* em = px, same as the baked ones */
    stbtt_GetGlyphHMetrics(&fb_info, gi, &adv, NULL);
    stbtt_GetGlyphBitmapBox(&fb_info, gi, sc, sc, &x0, &y0, &x1, &y1);
    w = x1 - x0; hh = y1 - y0;
    g->adv = adv * sc;
    if (w > 0 && hh > 0) {
        if (fb_x + w + 1 > FB_W) { fb_x = 0; fb_y += fb_row + 1; fb_row = 0; }
        if (fb_y + hh > FB_H) { fb_reset(); return fb_find(f, cp); }
        bmp = malloc((size_t)w * (size_t)hh);
        if (!bmp) return g;
        stbtt_MakeGlyphBitmap(&fb_info, bmp, w, hh, w, sc, sc, gi);
        gfx_tex_alpha_sub(&fb_atlas, fb_x, fb_y, w, hh, bmp);
        free(bmp);
        g->x0 = (float)x0; g->y0 = (float)y0; g->x1 = (float)x1; g->y1 = (float)y1;
        g->s0 = (float)fb_x / FB_W; g->t0 = (float)fb_y / FB_H;
        g->s1 = (float)(fb_x + w) / FB_W; g->t1 = (float)(fb_y + hh) / FB_H;
        fb_x += w + 1;
        if (hh > fb_row) fb_row = hh;
    }
    return g;
}

static int load_font(font *f, const char *fnt_path, const char *png_path)
{
    char magic[4];
    int aw, ah;
    FILE *fp = fopen(fnt_path, "rb");
    if (!fp) { fprintf(stderr, "hu: no font %s\n", fnt_path); return -1; }
    if (fread(magic, 1, 4, fp) != 4 || memcmp(magic, "HUF1", 4) ||
        fread(&f->px, 4, 1, fp) != 1 || fread(&f->ascent, 4, 1, fp) != 1 ||
        fread(&f->descent, 4, 1, fp) != 1 || fread(&f->line_gap, 4, 1, fp) != 1 ||
        fread(&f->count, 4, 1, fp) != 1 || fread(&aw, 4, 1, fp) != 1 || fread(&ah, 4, 1, fp) != 1 ||
        f->count <= 0 || f->count > 4096) {
        fprintf(stderr, "hu: bad font %s\n", fnt_path); fclose(fp); return -1;
    }
    f->g = (glyph *)malloc(sizeof(glyph) * (size_t)f->count);
    if (!f->g || fread(f->g, sizeof(glyph), (size_t)f->count, fp) != (size_t)f->count) {
        fprintf(stderr, "hu: bad font %s\n", fnt_path); free(f->g); f->g = NULL; fclose(fp); return -1;
    }
    fclose(fp);
    if (gfx_tex_load(&f->atlas, png_path, 0)) { free(f->g); f->g = NULL; return -1; }
    return 0;
}

int text_init(const char *dir)
{
    char path[512], fnt[512], png[512], line[128];
    int w;
    float dpx;
    FILE *fp;
    snprintf(fb_dir, sizeof fb_dir, "%s", dir);
    snprintf(path, sizeof path, "%s/fonts.txt", dir);
    fp = fopen(path, "r");
    if (!fp) { fprintf(stderr, "hu: no %s\n", path); return -1; }
    while (nfonts < MAX_FONTS && fgets(line, sizeof line, fp)) {
        if (sscanf(line, "%d %f", &w, &dpx) != 2) continue;     /* "weight size [digits]" */
        font *f = &fonts[nfonts];
        memset(f, 0, sizeof *f);
        f->weight = w; f->design_px = dpx;
        snprintf(fnt, sizeof fnt, "%s/f%d_%g.fnt", dir, w, dpx);
        snprintf(png, sizeof png, "%s/f%d_%g.png", dir, w, dpx);
        if (load_font(f, fnt, png) == 0) nfonts++;
    }
    fclose(fp);
    return nfonts > 0 ? 0 : -1;
}

void text_shutdown(void)
{
    int i;
    for (i = 0; i < nfonts; i++) { free(fonts[i].g); fonts[i].g = NULL; gfx_tex_free(&fonts[i].atlas); }
    nfonts = 0;
    if (fb_state > 0) gfx_tex_free(&fb_atlas);
    free(fb_tab); fb_tab = NULL; free(fb_ttf); fb_ttf = NULL;
    fb_state = 0; fb_n = fb_x = fb_y = fb_row = 0;
}

font *text_font(int weight, float design_px)
{
    /* cache (weight, size) -> font: called for every line of every frame */
    static struct { int w; float px; font *f; } memo[128];
    static int nmemo;
    int i, best = -1;
    float bd = 1e9f;
    for (i = 0; i < nmemo; i++) if (memo[i].w == weight && memo[i].px == design_px) return memo[i].f;
    for (i = 0; i < nfonts; i++) {
        float d = (fonts[i].weight == weight ? 0.0f : 1000.0f) + (fonts[i].design_px > design_px ?
                  fonts[i].design_px - design_px : design_px - fonts[i].design_px);
        if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0 && (fonts[best].weight != weight || fonts[best].design_px != design_px))
        fprintf(stderr, "hu: font %d/%g not baked, using %d/%g\n", weight, design_px,       /* once per size */
                fonts[best].weight, fonts[best].design_px);
    if (nmemo < 128) { memo[nmemo].w = weight; memo[nmemo].px = design_px; memo[nmemo].f = best >= 0 ? &fonts[best] : NULL; nmemo++; }
    return best >= 0 ? &fonts[best] : NULL;
}

static const glyph *find(const font *f, int cp)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (f->g[m].cp == cp) return &f->g[m];
        if (f->g[m].cp < cp) lo = m + 1; else hi = m - 1;
    }
    return NULL;
}

static int next_cp(const char **s)          /* UTF-8 -> code point */
{
    const unsigned char *p = (const unsigned char *)*s;
    int cp;
    if (!*p) return 0;
    if (*p < 0x80) { cp = *p; *s += 1; }
    else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); *s += 2; }
    else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); *s += 3; }
    else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); *s += 4;
    }
    else { cp = '?'; *s += 1; }
    return cp;
}

/* glyph: baked -> fallback font -> '?'; *fb = 1 if from the fallback cache */
static const glyph *glyph_of(const font *f, int cp, int *fb)
{
    const glyph *g = find(f, cp);
    *fb = 0;
    if (!g && cp > 0x7F && (g = fb_find(f, cp)) != NULL) *fb = 1;
    if (!g) g = find(f, '?');
    return g;
}

static int tnum;
void text_tnum(int on) { tnum = on; }

/* digit width with tnum: the widest of 0-9, digit centered in its cell */
static float tnum_adv(const font *f)
{
    float m = 0;
    int c;
    for (c = '0'; c <= '9'; c++) { const glyph *g = find(f, c); if (g && g->adv > m) m = g->adv; }
    return m;
}

float text_width(const font *f, const char *s, float ls)
{
    float w = 0;
    int cp, n = 0;
    if (!f) return 0;
    while ((cp = next_cp(&s)) != 0) {
        int fb;
        const glyph *g = glyph_of(f, cp, &fb);
        if (g) { w += tnum && cp >= '0' && cp <= '9' ? tnum_adv(f) : g->adv; n++; }
    }
    return n ? w + ls * (float)(n - 1) : 0;
}

float text_line_height(const font *f) { return f ? f->ascent - f->descent + f->line_gap : 0; }
float text_ascent(const font *f) { return f ? f->ascent + f->line_gap * 0.5f : 0; }

#define MAXQ 256
void text_draw(const font *f, float x, float y, const char *s, col c, float ls)
{
    static float q[2][MAXQ * 8];                  /* 0 - font atlas, 1 - fallback cache */
    float pen, base;
    int cp, n[2] = { 0, 0 };
    if (!f || !s) return;
    base = floorf(y + text_ascent(f) + 0.5f);     /* baseline and line start on whole pixels */
    float tadv = tnum ? tnum_adv(f) : 0;
    pen = floorf(x + 0.5f);
    while ((cp = next_cp(&s)) != 0 && n[0] < MAXQ && n[1] < MAXQ) {
        int fb, dig = tnum && cp >= '0' && cp <= '9';
        const glyph *g = glyph_of(f, cp, &fb);
        float gx;
        if (!g) continue;
        gx = pen + (dig ? floorf((tadv - g->adv) * 0.5f + 0.5f) : 0);
        if (g->x1 > g->x0) {
            float *o = q[fb] + n[fb] * 8;
            o[0] = gx + g->x0; o[1] = base + g->y0; o[2] = gx + g->x1; o[3] = base + g->y1;
            o[4] = g->s0; o[5] = g->t0; o[6] = g->s1; o[7] = g->t1;
            n[fb]++;
        }
        pen += (dig ? tadv : g->adv) + ls;
    }
    if (n[0]) gfx_glyphs(&f->atlas, q[0], n[0], c);
    if (n[1]) gfx_glyphs(&fb_atlas, q[1], n[1], c);
}

void text_unescape(char *s)
{
    static const struct { const char *e; char c; } N[] = { { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' }, { "apos;", '\'' } };
    char *w = s;
    while (*s) {
        unsigned long cp = 0;
        int k = 0, ok = 0;
        unsigned i;
        if (*s != '&') { *w++ = *s++; continue; }
        if (s[1] == '#') {
            const char *p = s + 2;
            int hex = *p == 'x' || *p == 'X';
            if (hex) p++;
            while (k < 8 && (hex ? isxdigit((unsigned char)*p) : isdigit((unsigned char)*p))) {
                cp = cp * (hex ? 16 : 10) + (unsigned long)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                p++; k++;
            }
            if (k && *p == ';' && cp > 0 && cp <= 0x10FFFF) {
                s = (char *)p + 1; ok = 1;
                if (cp < 0x80) *w++ = (char)cp;
                else if (cp < 0x800) { *w++ = (char)(0xC0 | cp >> 6); *w++ = (char)(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { *w++ = (char)(0xE0 | cp >> 12); *w++ = (char)(0x80 | (cp >> 6 & 0x3F)); *w++ = (char)(0x80 | (cp & 0x3F)); }
                else { *w++ = (char)(0xF0 | cp >> 18); *w++ = (char)(0x80 | (cp >> 12 & 0x3F)); *w++ = (char)(0x80 | (cp >> 6 & 0x3F)); *w++ = (char)(0x80 | (cp & 0x3F)); }
            }
        } else for (i = 0; i < sizeof N / sizeof *N; i++)
            if (!strncmp(s + 1, N[i].e, strlen(N[i].e))) { *w++ = N[i].c; s += 1 + strlen(N[i].e); ok = 1; break; }
        if (!ok) *w++ = *s++;
    }
    *w = 0;
}
