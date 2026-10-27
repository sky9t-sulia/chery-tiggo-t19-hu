/* Baking font atlases (runs on the PC, in the tiggo-pc container).
* usage: bake_fonts <ttf_dir> <out_dir> <scale>   (reads <out_dir>/fonts.txt: "weight design_size" per line)
* For each pair writes f<weight>_<size>.png (white + alpha) and .fnt (metrics, see src/text.c).
* Glyphs are rasterized at screen size (design size * scale), with 2x horizontal supersampling. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

typedef struct { int cp; float x0, y0, x1, y1, s0, t0, s1, t1, adv; } glyph;

/* character ranges (ascending) and the font for them: 0 - Figtree (Latin, symbols), 1 - Onest (Cyrillic; Figtree
* lacks it), 2 - Inter (⌫: not in Figtree, the browser takes it from the system Adwaita Sans = Inter; only SemiBold exists).
* digits - for large digits (calculator etc.): ASCII, ×, ÷, −, ⌫ */
static const int RANGES[][4] = { { 0x20, 0x7E, 0, 1 }, { 0xA0, 0xD6, 0, 0 }, { 0xD7, 0xD7, 0, 1 }, { 0xD8, 0xF6, 0, 0 },
                                 { 0xF7, 0xF7, 0, 1 }, { 0xF8, 0xFF, 0, 0 }, { 0x400, 0x45F, 1, 0 }, { 0x490, 0x491, 1, 0 },
                                 { 0x2010, 0x203A, 0, 0 }, { 0x2212, 0x2212, 0, 1 }, { 0x232B, 0x232B, 2, 1 } };
#define NR (int)(sizeof RANGES / sizeof RANGES[0])

static unsigned char *read_file(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    unsigned char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)*n);
    if (b && fread(b, 1, (size_t)*n, f) != (size_t)*n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static const char *style(int w)
{
    return w >= 800 ? "ExtraBold" : w >= 700 ? "Bold" : w >= 600 ? "SemiBold" : w >= 500 ? "Medium" : "Light";
}

static int bake(const char *ttf_dir, const char *out_dir, int weight, float dpx, float scale, int digits_only)
{
    char path[512];
    long n;
    unsigned char *ttf, *ttf2, *ttf3, *bmp = NULL, *rgba, *data[3];
    stbtt_fontinfo info, info2, info3, *fi[3];
    stbtt_pack_context pc;
    stbtt_pack_range pr[NR];
    stbtt_packedchar *chars[NR];
    int r, i, k, total = 0, aw = 256, ah = 256, ok = 0, asc, desc, gap;
    float px = dpx * scale, sc;
    glyph *g;
    FILE *fp;

    if (weight == 400) snprintf(path, sizeof path, "%s/mono/LiberationMono-Regular.ttf", ttf_dir);   /* monospaced (log); 300 - Light */
    else snprintf(path, sizeof path, "%s/Figtree-%s.ttf", ttf_dir, style(weight));
    ttf = read_file(path, &n);
    if (!ttf || !stbtt_InitFont(&info, ttf, 0)) { fprintf(stderr, "cannot read %s\n", path); free(ttf); return -1; }
    if (weight == 400) snprintf(path, sizeof path, "%s/mono/LiberationMono-Regular.ttf", ttf_dir);
    else snprintf(path, sizeof path, "%s/onest/Onest-%s.ttf", ttf_dir, style(weight));
    ttf2 = read_file(path, &n);
    if (!ttf2 || !stbtt_InitFont(&info2, ttf2, 0)) { fprintf(stderr, "cannot read %s\n", path); free(ttf); free(ttf2); return -1; }
    snprintf(path, sizeof path, "%s/inter/Inter-%s.ttf", ttf_dir, style(weight));
    ttf3 = read_file(path, &n);                    /* optional: no file - no those characters */
    if (ttf3 && !stbtt_InitFont(&info3, ttf3, 0)) { free(ttf3); ttf3 = NULL; }
    data[0] = ttf; data[1] = ttf2; data[2] = ttf3; fi[0] = &info; fi[1] = &info2; fi[2] = &info3;
    for (r = 0; r < NR; r++) {
        int cnt = RANGES[r][1] - RANGES[r][0] + 1;
        if ((digits_only && !RANGES[r][3]) || !data[RANGES[r][2]]) cnt = 0;
        chars[r] = calloc((size_t)cnt, sizeof(stbtt_packedchar));
        memset(&pr[r], 0, sizeof pr[r]);
        pr[r].font_size = STBTT_POINT_SIZE(px);      /* px - em size, like font-size in CSS (not the ascent-descent height) */
        pr[r].first_unicode_codepoint_in_range = RANGES[r][0];
        pr[r].num_chars = cnt;
        pr[r].chardata_for_range = chars[r];
        total += cnt;
    }
    while (!ok && aw <= 2048) {                    /* smallest square atlas that fits everything */
        free(bmp);
        bmp = calloc((size_t)aw * (size_t)ah, 1);
        stbtt_PackBegin(&pc, bmp, aw, ah, 0, 1, NULL);
        stbtt_PackSetOversampling(&pc, 2, 1);
        ok = 1;
        for (r = 0; r < NR && ok; r++)
            ok = !pr[r].num_chars || stbtt_PackFontRanges(&pc, data[RANGES[r][2]], 0, &pr[r], 1);
        stbtt_PackEnd(&pc);
        if (!ok) { aw *= 2; ah *= 2; }
    }
    if (!ok) { fprintf(stderr, "atlas overflow %d/%g\n", weight, dpx); return -1; }

    rgba = NULL;                                   /* the atlas is a single channel (coverage), GL_ALPHA on the GPU */
    snprintf(path, sizeof path, "%s/f%d_%g.png", out_dir, weight, dpx);
    stbi_write_png(path, aw, ah, 1, bmp, aw);

    g = calloc((size_t)total, sizeof(glyph));
    k = 0;
    for (r = 0; r < NR; r++)
        for (i = 0; i < pr[r].num_chars; i++) {
            stbtt_aligned_quad q;
            float xp = 0, yp = 0;
            int cp = RANGES[r][0] + i;
            if (!stbtt_FindGlyphIndex(fi[RANGES[r][2]], cp)) continue;
            stbtt_GetPackedQuad(chars[r], aw, ah, i, &xp, &yp, &q, 0);
            g[k].cp = cp; g[k].x0 = q.x0; g[k].y0 = q.y0; g[k].x1 = q.x1; g[k].y1 = q.y1;
            g[k].s0 = q.s0; g[k].t0 = q.t0; g[k].s1 = q.s1; g[k].t1 = q.t1; g[k].adv = xp;
            k++;
        }
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    sc = stbtt_ScaleForMappingEmToPixels(&info, px);   /* em = px, like in the browser */
    /* line-height: normal in the browser is computed from hhea ascent/descent */
    {
        float fa = asc * sc, fd = desc * sc, fg = gap * sc;
        snprintf(path, sizeof path, "%s/f%d_%g.fnt", out_dir, weight, dpx);
        fp = fopen(path, "wb");
        fwrite("HUF1", 1, 4, fp);
        fwrite(&px, 4, 1, fp); fwrite(&fa, 4, 1, fp); fwrite(&fd, 4, 1, fp); fwrite(&fg, 4, 1, fp);
        fwrite(&k, 4, 1, fp); fwrite(&aw, 4, 1, fp); fwrite(&ah, 4, 1, fp);
        fwrite(g, sizeof(glyph), (size_t)k, fp);
        fclose(fp);
    }
    printf("f%d_%g: %gpx, %d glyphs, atlas %dx%d\n", weight, dpx, px, k, aw, ah);
    for (r = 0; r < NR; r++) free(chars[r]);
    free(g); free(rgba); free(bmp); free(ttf); free(ttf2); free(ttf3);
    return 0;
}

int main(int argc, char **argv)
{
    char path[512];
    int w, fails = 0;
    float d;
    FILE *fp;
    if (argc != 4) { fprintf(stderr, "usage: %s ttf_dir out_dir scale\n", argv[0]); return 2; }
    snprintf(path, sizeof path, "%s/fonts.txt", argv[2]);
    fp = fopen(path, "r");
    if (!fp) { fprintf(stderr, "no %s\n", path); return 1; }
    {
        char line[128], set[16];
        while (fgets(line, sizeof line, fp)) {
            set[0] = 0;
            if (sscanf(line, "%d %f %15s", &w, &d, set) >= 2)
                fails += bake(argv[1], argv[2], w, d, (float)atof(argv[3]), !strcmp(set, "digits")) != 0;
        }
    }
    fclose(fp);
    return fails ? 1 : 0;
}
