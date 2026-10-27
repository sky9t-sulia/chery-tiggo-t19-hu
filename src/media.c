/* USB stick: scanning (in its own thread), tags and duration, photos and cover art (decoding in its own thread, upload
 * to the GPU in the main one), players - player.c (GStreamer 1.0). */
#include "media.h"
#include "ui_int.h"

#include <ctype.h>
#include <setjmp.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "../third_party/stb_image.h"   /* implementation is in gfx.c */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"   /* thumbnails in .thumbnails on the stick */
#include "../third_party/libjpeg8/jpeglib.h" /* the HU's libjpeg.so.8 - via dlopen: decoding with downscaling */

track_t *TRACKS;
video_t *VIDEOS;
image_t *IMAGES;
int NTRACKS, NVIDEOS, NIMAGES;

/* ================================================================= scanning */

static track_t *s_tr;                          /* the scan thread writes here, the main one takes it whole */
static video_t *s_vi;
static image_t *s_im;
static int s_nt, s_nv, s_ni, s_ct, s_cv, s_ci;
static volatile int scan_state;                /* 0 none, 1 running, 2 done (to be taken) */
static volatile int scan_abort;
static char scan_root[256];
static int gen, scanned_for;

static const char *ext_of(const char *n) { const char *d = strrchr(n, '.'); return d ? d + 1 : ""; }
static int ext_in(const char *e, const char *const *list) { for (; *list; list++) if (!strcasecmp(e, *list)) return 1; return 0; }
static const char *const AUD[] = { "mp3", "wav", "aac", "m4a", "flac", "ogg", "opus", "wma", "amr", NULL };
static const char *const VID[] = { "mp4", "3gp", "avi", "mkv", "mov", "m4v", "wmv", "flv", "ts", "mpg", "mpeg", "webm", NULL };
static const char *const IMG[] = { "jpg", "jpeg", "png", "gif", "bmp", NULL };
int media_kind(const char *name)   /* 1 audio, 2 video, 3 photo, 0 - other (by extension) */
{
    const char *d = strrchr(name, '.'), *e = d ? d + 1 : "";
    return ext_in(e, AUD) ? 1 : ext_in(e, VID) ? 2 : ext_in(e, IMG) ? 3 : 0;
}


static uint32_t rd32be(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t syncsafe(const unsigned char *p) { return (uint32_t)(p[0] & 0x7F) << 21 | (uint32_t)(p[1] & 0x7F) << 14 | (uint32_t)(p[2] & 0x7F) << 7 | (p[3] & 0x7F); }

/* ID3 text (encoding in the first byte) -> UTF-8 */
static void id3_text(char *out, size_t on, const unsigned char *p, uint32_t n)
{
    size_t o = 0;
    uint32_t i = 1;
    int enc;
    if (!n) { out[0] = 0; return; }
    enc = p[0];
    if (enc == 1 || enc == 2) {                                   /* UTF-16 (with BOM / BE) */
        int be = enc == 2;
        if (n >= 3 && p[1] == 0xFE && p[2] == 0xFF) { be = 1; i = 3; } else if (n >= 3 && p[1] == 0xFF && p[2] == 0xFE) { be = 0; i = 3; }
        for (; i + 1 < n && o + 4 < on; i += 2) {
            unsigned c = be ? (unsigned)p[i] << 8 | p[i + 1] : (unsigned)p[i + 1] << 8 | p[i];
            if (!c) break;
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {               /* surrogate pair */
                unsigned c2 = be ? (unsigned)p[i + 2] << 8 | p[i + 3] : (unsigned)p[i + 3] << 8 | p[i + 2];
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00); i += 2;
            }
            if (c < 0x80) out[o++] = (char)c;
            else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
            else if (c < 0x10000) { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | (c >> 6 & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
            else { out[o++] = (char)(0xF0 | c >> 18); out[o++] = (char)(0x80 | (c >> 12 & 0x3F)); out[o++] = (char)(0x80 | (c >> 6 & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
        }
    } else if (enc == 3) {                                        /* UTF-8 */
        for (; i < n && p[i] && o + 1 < on; i++) out[o++] = (char)p[i];
    } else {                                                      /* ISO-8859-1 */
        for (; i < n && p[i] && o + 2 < on; i++) {
            if (p[i] < 0x80) out[o++] = (char)p[i];
            else { out[o++] = (char)(0xC0 | p[i] >> 6); out[o++] = (char)(0x80 | (p[i] & 0x3F)); }
        }
    }
    out[o] = 0;
    while (o && out[o - 1] == ' ') out[--o] = 0;
}

/* MP3: ID3v2 tags (TIT2/TPE1/TALB, APIC cover art) and duration (Xing/VBRI or CBR) */
static void scan_mp3(FILE *f, track_t *t, long fsize)
{
    unsigned char h[10], *buf = NULL, fh[4], x[200];
    uint32_t tag = 0;
    long pos;
    if (fread(h, 1, 10, f) == 10 && !memcmp(h, "ID3", 3)) {
        int ver = h[3];
        tag = syncsafe(h + 6) + 10;
        if (tag > 10 && tag < 16 * 1024 * 1024 && (buf = malloc(tag))) {
            uint32_t i = 0, n = tag - 10;
            if (fread(buf, 1, n, f) == n)
                while (i + (ver == 2 ? 6 : 10) <= n) {
                    const unsigned char *fr = buf + i;
                    uint32_t sz = ver == 2 ? (uint32_t)fr[3] << 16 | (uint32_t)fr[4] << 8 | fr[5] : ver == 4 ? syncsafe(fr + 4) : rd32be(fr + 4);
                    uint32_t hl = ver == 2 ? 6 : 10;
                    if (!fr[0] || sz == 0 || i + hl + sz > n) break;
                    if (!memcmp(fr, ver == 2 ? "TT2" : "TIT2", ver == 2 ? 3 : 4)) id3_text(t->t, sizeof t->t, fr + hl, sz);
                    else if (!memcmp(fr, ver == 2 ? "TP1" : "TPE1", ver == 2 ? 3 : 4)) id3_text(t->a, sizeof t->a, fr + hl, sz);
                    else if (!memcmp(fr, ver == 2 ? "TAL" : "TALB", ver == 2 ? 3 : 4)) id3_text(t->al, sizeof t->al, fr + hl, sz);
                    else if (!t->art_len && !memcmp(fr, ver == 2 ? "PIC" : "APIC", ver == 2 ? 3 : 4)) {
                        /* APIC: encoding, MIME (\0), type, description (\0 or \0\0), picture */
                        const unsigned char *p = fr + hl, *e = p + sz;
                        int enc = *p++;
                        if (ver == 2) p += 3; else { while (p < e && *p) p++; p++; }
                        p++;
                        if (enc == 1 || enc == 2) { while (p + 1 < e && (p[0] || p[1])) p += 2; p += 2; } else { while (p < e && *p) p++; p++; }
                        if (p < e) { t->art_off = 10 + (long)(p - buf); t->art_len = (long)(e - p); }
                    }
                    i += hl + sz;
                }
            free(buf);
        }
    }
    /* first frame after the tag */
    pos = tag;
    fseek(f, pos, SEEK_SET);
    while (fread(fh, 1, 4, f) == 4) {
        if (fh[0] == 0xFF && (fh[1] & 0xE0) == 0xE0) break;
        if (++pos - tag > 65536) return;
        fseek(f, pos, SEEK_SET);
    }
    {
        static const int BR1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
        static const int BR2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
        static const int SR[4] = { 44100, 48000, 32000, 0 };
        int v1 = (fh[1] >> 3 & 3) == 3, mono = (fh[3] >> 6) == 3, sr = SR[fh[2] >> 2 & 3], br = (v1 ? BR1 : BR2)[fh[2] >> 4];
        int spf = v1 ? 1152 : 576, side = v1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
        if (!sr) return;
        if (!v1) sr /= (fh[1] >> 3 & 3) == 2 ? 2 : 4;
        fseek(f, pos + 4, SEEK_SET);
        if (fread(x, 1, sizeof x, f) == sizeof x) {
            const unsigned char *xi = x + side;
            if ((!memcmp(xi, "Xing", 4) || !memcmp(xi, "Info", 4)) && (xi[7] & 1)) { t->len = (int)((double)rd32be(xi + 8) * spf / sr); return; }
            if (!memcmp(x + 32, "VBRI", 4)) { t->len = (int)((double)rd32be(x + 32 + 14) * spf / sr); return; }
        }
        if (br) t->len = (int)((double)(fsize - pos) * 8 / (br * 1000.0));
    }
}

/* FLAC: STREAMINFO (duration) and VORBIS_COMMENT (TITLE/ARTIST/ALBUM) */
static void scan_flac(FILE *f, track_t *t)
{
    unsigned char h[4], si[18], *b;
    int last = 0;
    if (fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4)) return;
    while (!last && fread(h, 1, 4, f) == 4) {
        uint32_t len = (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
        int type = h[0] & 0x7F;
        last = h[0] & 0x80;
        if (type == 0 && len >= 18 && fread(si, 1, 18, f) == 18) {
            uint32_t sr = (uint32_t)si[10] << 12 | (uint32_t)si[11] << 4 | si[12] >> 4;
            uint64_t n = (uint64_t)(si[13] & 0x0F) << 32 | rd32be(si + 14);
            if (sr) t->len = (int)(n / sr);
            fseek(f, (long)len - 18, SEEK_CUR);
        } else if (type == 4 && len < 1024 * 1024 && (b = malloc(len))) {
            if (fread(b, 1, len, f) == len) {
                uint32_t p = 4 + (b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24), cnt, k;
                if (p + 4 <= len) {
                    cnt = b[p] | b[p + 1] << 8 | b[p + 2] << 16 | (uint32_t)b[p + 3] << 24; p += 4;
                    for (k = 0; k < cnt && p + 4 <= len; k++) {
                        uint32_t l = b[p] | b[p + 1] << 8 | b[p + 2] << 16 | (uint32_t)b[p + 3] << 24;
                        char *c = (char *)b + p + 4, v[256];
                        p += 4;
                        if (p + l > len) break;
                        snprintf(v, sizeof v, "%.*s", (int)(l < 255 ? l : 255), c);
                        if (!strncasecmp(v, "TITLE=", 6)) scpy(t->t, sizeof t->t, v + 6);
                        else if (!strncasecmp(v, "ARTIST=", 7)) scpy(t->a, sizeof t->a, v + 7);
                        else if (!strncasecmp(v, "ALBUM=", 6)) scpy(t->al, sizeof t->al, v + 6);
                        p += l;
                    }
                }
            }
            free(b);
        } else fseek(f, (long)len, SEEK_CUR);
    }
}

/* MP4: duration from moov/mvhd */
/* video frame size (tkhd of the moov tracks, 16.16): the largest; 0 - not found */
static void mp4_dims(FILE *f, long fsize, int *w, int *h)
{
    unsigned char hd[16];
    long pos = 0, end = fsize;
    *w = *h = 0;
    while (pos + 8 <= end) {
        uint64_t sz;
        fseek(f, pos, SEEK_SET);
        if (fread(hd, 1, 8, f) != 8) return;
        sz = rd32be(hd);
        if (sz == 1) { unsigned char e[8]; if (fread(e, 1, 8, f) != 8) return; sz = (uint64_t)rd32be(e) << 32 | rd32be(e + 4); }
        if (sz < 8) return;
        if (!memcmp(hd + 4, "moov", 4) || !memcmp(hd + 4, "trak", 4)) { if (!memcmp(hd + 4, "moov", 4)) end = pos + (long)sz; pos += 8; continue; }
        if (!memcmp(hd + 4, "tkhd", 4)) {
            unsigned char m[96];
            size_t n = fread(m, 1, sizeof m, f), o = m[0] == 1 ? 88 : 76;
            if (n >= o + 8) {
                int tw = (int)(rd32be(m + o) >> 16), th = (int)(rd32be(m + o + 4) >> 16);
                if (tw * th > *w * *h) { *w = tw; *h = th; }
            }
        }
        pos += (long)sz;
    }
}

static int mp4_len(FILE *f, long fsize)
{
    unsigned char h[16];
    long pos = 0, end = fsize;
    while (pos + 8 <= end) {
        uint64_t sz;
        fseek(f, pos, SEEK_SET);
        if (fread(h, 1, 8, f) != 8) return 0;
        sz = rd32be(h);
        if (sz == 1) { unsigned char e[8]; if (fread(e, 1, 8, f) != 8) return 0; sz = (uint64_t)rd32be(e) << 32 | rd32be(e + 4); }
        if (sz < 8) return 0;
        if (!memcmp(h + 4, "moov", 4)) { end = pos + (long)sz; pos += 8; continue; }
        if (!memcmp(h + 4, "mvhd", 4)) {
            unsigned char m[32];
            if (fread(m, 1, 32, f) != 32) return 0;
            if (m[0] == 1) { uint32_t ts = rd32be(m + 20); uint64_t d = (uint64_t)rd32be(m + 24) << 32 | rd32be(m + 28); return ts ? (int)(d / ts) : 0; }
            return rd32be(m + 12) ? (int)(rd32be(m + 16) / rd32be(m + 12)) : 0;
        }
        pos += (long)sz;
    }
    return 0;
}

static uint32_t rd32le(const unsigned char *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* WAV: data / bytes per second (fmt and data chunks) */
static int wav_len(FILE *f)
{
    unsigned char h[12], c[8], fmt[16];
    uint32_t rate = 0;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 0;
    while (fread(c, 1, 8, f) == 8) {
        uint32_t sz = rd32le(c + 4);
        if (!memcmp(c, "fmt ", 4) && sz >= 16 && fread(fmt, 1, 16, f) == 16) { rate = rd32le(fmt + 8); fseek(f, (long)(sz - 16 + (sz & 1)), SEEK_CUR); }
        else if (!memcmp(c, "data", 4)) return rate ? (int)(sz / rate) : 0;
        else fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
    }
    return 0;
}

/* OGG (Vorbis / Opus): last granule / rate (Opus - 48 kHz minus pre-skip) */
static int ogg_len(FILE *f, long fsize)
{
    unsigned char h[64], *b;
    long n = fsize < 65536 ? fsize : 65536, i;
    uint32_t rate = 0, skip = 0;
    uint64_t g = 0;
    if (fread(h, 1, 64, f) != 64 || memcmp(h, "OggS", 4)) return 0;
    if (!memcmp(h + 28, "\x01vorbis", 7)) rate = rd32le(h + 28 + 12);
    else if (!memcmp(h + 28, "OpusHead", 8)) { rate = 48000; skip = h[28 + 10] | (uint32_t)h[28 + 11] << 8; }
    if (!rate || !(b = malloc((size_t)n))) return 0;
    fseek(f, fsize - n, SEEK_SET);
    if (fread(b, 1, (size_t)n, f) == (size_t)n)
        for (i = n - 14; i >= 0; i--)
            if (!memcmp(b + i, "OggS", 4)) { g = rd32le(b + i + 6) | (uint64_t)rd32le(b + i + 10) << 32; break; }
    free(b);
    return g > skip ? (int)((g - skip) / rate) : 0;
}

/* AAC (ADTS): frames of 1024 samples, walking over the headers */
static int adts_len(FILE *f)
{
    static const int SR[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    unsigned char h[7];
    long pos = 0;
    int sr = 0;
    uint64_t frames = 0;
    while (fseek(f, pos, SEEK_SET) == 0 && fread(h, 1, 7, f) == 7) {
        int len;
        if (h[0] != 0xFF || (h[1] & 0xF6) != 0xF0) {               /* not a frame: ID3 at the start or garbage */
            if (!frames && !memcmp(h, "ID3", 3)) { unsigned char t[4]; fseek(f, pos + 6, SEEK_SET); if (fread(t, 1, 4, f) != 4) break; pos += 10 + syncsafe(t); continue; }
            break;
        }
        if (!sr && (h[2] >> 2 & 15) < 13) sr = SR[h[2] >> 2 & 15];
        len = (h[3] & 3) << 11 | h[4] << 3 | h[5] >> 5;
        if (len < 7) break;
        frames += (uint64_t)(h[6] & 3) + 1;
        pos += len;
        if (scan_abort) break;
    }
    return sr ? (int)(frames * 1024 / sr) : 0;
}

static uint32_t hue_of(const char *s)
{
    static const uint32_t H[9] = { 0xFF4F6A, 0x7B61FF, 0x14A3A3, 0xFF8A2A, 0x2F7BF6, 0xE0458F, 0x2DBE6C, 0xC2410C, 0x3B5BDB };
    unsigned h = 5381;
    for (; *s; s++) h = h * 33 + (unsigned char)*s;
    return H[h % 9];
}

static void scan_dir(const char *dir, int depth)
{
    DIR *d;
    struct dirent *e;
    char p[4096];                                       /* PATH_MAX; depth is unlimited (we do not follow links to directories) */
    if (scan_abort || !(d = opendir(dir))) return;
    while ((e = readdir(d)) && !scan_abort) {
        struct stat st;
        const char *x;
        if (e->d_name[0] == '.') continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;   /* path longer than PATH_MAX */
        if (lstat(p, &st)) continue;
        if (S_ISLNK(st.st_mode)) { if (stat(p, &st) || S_ISDIR(st.st_mode)) continue; }   /* link to a directory - a loop */
        if (S_ISDIR(st.st_mode)) {
            if (depth == 0 && (!strcasecmp(e->d_name, "tiggo") || !strcmp(e->d_name, "apps") || !strcasecmp(e->d_name, "System Volume Information"))) continue;   /* service ones; apps - Lua programs (their icons are not photos) */
            scan_dir(p, depth + 1);
            continue;
        }
        x = ext_of(e->d_name);
        if (ext_in(x, AUD)) {
            track_t *t, *g = grow(s_tr, &s_ct, s_nt + 1, sizeof *s_tr);
            if (!g) continue;
            FILE *f;
            s_tr = g; t = &s_tr[s_nt];
            memset(t, 0, sizeof *t);
            if (!(t->path = strdup(p))) continue;
            f = fopen(p, "rb");
            if (f) {
                if (!strcasecmp(x, "mp3")) scan_mp3(f, t, (long)st.st_size);
                else if (!strcasecmp(x, "flac")) scan_flac(f, t);
                else if (!strcasecmp(x, "wav")) t->len = wav_len(f);
                else if (!strcasecmp(x, "ogg") || !strcasecmp(x, "opus")) t->len = ogg_len(f, (long)st.st_size);
                else if (!strcasecmp(x, "aac")) t->len = adts_len(f);
                else if (!strcasecmp(x, "m4a")) t->len = mp4_len(f, (long)st.st_size);
                fclose(f);
            }
            if (!t->t[0]) snprintf(t->t, sizeof t->t, "%.*s", (int)(x - e->d_name - 1), e->d_name);   /* no tag - the file name */
            if (!t->a[0]) snprintf(t->a, sizeof t->a, "Unknown artist");
            t->hue = hue_of(t->t);
            t->added = (double)st.st_mtime;
            s_nt++;
        } else if (ext_in(x, VID)) {
            video_t *v, *g = grow(s_vi, &s_cv, s_nv + 1, sizeof *s_vi);
            if (!g) continue;
            FILE *f;
            s_vi = g; v = &s_vi[s_nv++];
            if (!(v->path = strdup(p))) { s_nv--; continue; }
            v->name = strrchr(v->path, '/') + 1;
            v->size = (long long)st.st_size;
            v->len = v->w = v->h = 0;
            f = fopen(p, "rb");
            if (f) {
                if (!strcasecmp(x, "mp4") || !strcasecmp(x, "m4v") || !strcasecmp(x, "mov") || !strcasecmp(x, "3gp")) {
                    v->len = mp4_len(f, (long)st.st_size);
                    mp4_dims(f, (long)st.st_size, &v->w, &v->h);
                }
                fclose(f);
            }
        } else if (ext_in(x, IMG)) {
            image_t *im, *g = grow(s_im, &s_ci, s_ni + 1, sizeof *s_im);
            if (!g) continue;
            s_im = g; im = &s_im[s_ni++];
            if (!(im->path = strdup(p))) { s_ni--; continue; }
            im->name = strrchr(im->path, '/') + 1;
            im->size = (long long)st.st_size; im->mtime = (long long)st.st_mtime;
        }
    }
    closedir(d);
}

static int cmp_vid(const void *a, const void *b) { return strcasecmp(((const video_t *)a)->name, ((const video_t *)b)->name); }
static int cmp_img(const void *a, const void *b) { return strcasecmp(((const image_t *)a)->name, ((const image_t *)b)->name); }

static void *scan_thread(void *x)
{
    (void)x;
    s_nt = s_nv = s_ni = 0;
    s_tr = NULL; s_vi = NULL; s_im = NULL; s_ct = s_cv = s_ci = 0;
    scan_dir(scan_root, 0);
    if (s_nv) qsort(s_vi, (size_t)s_nv, sizeof s_vi[0], cmp_vid);   /* empty list - NULL */
    if (s_ni) qsort(s_im, (size_t)s_ni, sizeof s_im[0], cmp_img);
    scan_state = 2;
    return NULL;
}

/* whole lists (and paths in the heap) */
static void free_lists(track_t *t, int nt, video_t *v, int nv, image_t *im, int ni)
{
    int i;
    for (i = 0; i < nt; i++) free(t[i].path);
    for (i = 0; i < nv; i++) free(v[i].path);
    for (i = 0; i < ni; i++) free(im[i].path);
    free(t); free(v); free(im);
}

int media_gen(void) { return gen; }
int media_scanning(void) { return scan_state == 1; }

/* ================================================================= photos and cover art: decoding in a thread */

enum { K_THUMB, K_FULL, K_ART };
typedef struct {
    int kind, idx, state;                     /* state: 0 empty, 1 queued, 2 decoding, 3 pixels ready, 4 in GPU, 5 error */
    double used;
    int w, h;
    unsigned char *px;
    gfx_tex tex;
} slot_t;
#define NSLOT 40
static slot_t slots[NSLOT];
static pthread_mutex_t img_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t img_cv = PTHREAD_COND_INITIALIZER;
static int img_started, img_gen;              /* img_gen - stick changed: discard unfinished work */
static double clock_now;

/* ---- JPEG with downscaling while decoding (libjpeg scale 1/2..1/8): we do not hold huge photos in memory whole.
 * 8064x6048 -> 1008x756 (2 MB instead of ~300). libjpeg errors terminate the process by default - we intercept them with longjmp. */
static struct {
    int tried, ok;
    struct jpeg_error_mgr *(*std_error)(struct jpeg_error_mgr *);
    void (*create)(j_decompress_ptr, int, size_t);
    void (*src)(j_decompress_ptr, FILE *);
    int (*header)(j_decompress_ptr, boolean);
    boolean (*start)(j_decompress_ptr);
    JDIMENSION (*lines)(j_decompress_ptr, JSAMPARRAY, JDIMENSION);
    boolean (*finish)(j_decompress_ptr);
    void (*destroy)(j_decompress_ptr);
} J;
struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jerr_exit(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }
static void jerr_msg(j_common_ptr c) { (void)c; }

static int jpeg_load(void)
{
    void *l;
    if (J.tried) return J.ok;
    J.tried = 1;
    if (!(l = dlopen("libjpeg.so.8", RTLD_NOW))) return 0;      /* not on PC - then stb_image whole */
    J.std_error = (struct jpeg_error_mgr *(*)(struct jpeg_error_mgr *))dlsym(l, "jpeg_std_error");
    J.create = (void (*)(j_decompress_ptr, int, size_t))dlsym(l, "jpeg_CreateDecompress");
    J.src = (void (*)(j_decompress_ptr, FILE *))dlsym(l, "jpeg_stdio_src");
    J.header = (int (*)(j_decompress_ptr, boolean))dlsym(l, "jpeg_read_header");
    J.start = (boolean (*)(j_decompress_ptr))dlsym(l, "jpeg_start_decompress");
    J.lines = (JDIMENSION (*)(j_decompress_ptr, JSAMPARRAY, JDIMENSION))dlsym(l, "jpeg_read_scanlines");
    J.finish = (boolean (*)(j_decompress_ptr))dlsym(l, "jpeg_finish_decompress");
    J.destroy = (void (*)(j_decompress_ptr))dlsym(l, "jpeg_destroy_decompress");
    J.ok = J.std_error && J.create && J.src && J.header && J.start && J.lines && J.finish && J.destroy;
    return J.ok;
}

/* RGB; the long side is not less than target (if the source is larger) */
static unsigned char *jpeg_scaled(const char *path, int target, int *ow, int *oh)
{
    struct jpeg_decompress_struct ci;
    struct jerr je;
    unsigned char *volatile buf = NULL;
    FILE *volatile f;
    int d;
    if (!jpeg_load() || !(f = fopen(path, "rb"))) return NULL;
    ci.err = J.std_error(&je.pub);
    je.pub.error_exit = jerr_exit;
    je.pub.output_message = jerr_msg;
    if (setjmp(je.jb)) { fprintf(stderr, "hu: jpeg: %s: error %d\n", path, je.pub.msg_code); J.destroy(&ci); fclose(f); free(buf); return NULL; }
    J.create(&ci, JPEG_LIB_VERSION, sizeof ci);
    J.src(&ci, f);
    J.header(&ci, TRUE);
    for (d = 8; d > 1 && (int)(ci.image_width > ci.image_height ? ci.image_width : ci.image_height) / d < target; d /= 2) {}
    ci.scale_num = 1; ci.scale_denom = (unsigned)d;
    ci.out_color_space = JCS_RGB;
    ci.dct_method = JDCT_IFAST;
    ci.do_fancy_upsampling = FALSE;
    J.start(&ci);
    buf = malloc((size_t)ci.output_width * ci.output_height * 3);
    if (buf) while (ci.output_scanline < ci.output_height) {
        JSAMPROW row = buf + (size_t)ci.output_scanline * ci.output_width * 3;
        J.lines(&ci, &row, 1);
    }
    *ow = (int)ci.output_width; *oh = (int)ci.output_height;
    J.finish(&ci);
    J.destroy(&ci);
    fclose(f);
    return buf;
}

/* RGB picture from the stick: JPEG - with downscaling, the rest - stb_image */
static unsigned char *load_rgb(const char *path, int target, int *w, int *h)
{
    int n;
    const char *e = strrchr(path, '.');
    unsigned char *p = NULL;
    if (e && (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg"))) p = jpeg_scaled(path, target, w, h);
    if (!p) {
        unsigned char *s = stbi_load(path, w, h, &n, 3);
        if (s) { p = malloc((size_t)*w * *h * 3); if (p) memcpy(p, s, (size_t)*w * *h * 3); stbi_image_free(s); }
    }
    return p;
}

/* thumbnail on the stick: <stick>/.thumbnails/<hash of path+size+date>.jpg */
#define THUMB 320                         /* thumbnail: grid cell ~205 px, Photos widget 320 px */
static void thumb_path(char *o, size_t n, const image_t *im)
{
    uint64_t h = 1469598103934665603ULL;
    const char *p;
    for (p = im->path; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ULL;
    h ^= (uint64_t)im->size * 31 + (uint64_t)im->mtime;
    snprintf(o, n, "%s/.thumbnails/%016llx_%d.jpg", scan_root, (unsigned long long)h, THUMB);
}

/* downscale to max on the long side (block averaging), RGB -> RGBA */
static unsigned char *shrink(const unsigned char *src, int w, int h, int max, int *ow, int *oh)
{
    float sc = (float)max / (w > h ? w : h);
    int W, H, x, y;
    unsigned char *o;
    if (sc > 1) sc = 1;
    W = (int)(w * sc + 0.5f); H = (int)(h * sc + 0.5f);
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    if (!(o = malloc((size_t)W * H * 4))) return NULL;
    for (y = 0; y < H; y++) {
        int y0 = (int)((long long)y * h / H), y1 = (int)((long long)(y + 1) * h / H);
        if (y1 <= y0) y1 = y0 + 1;
        for (x = 0; x < W; x++) {
            int x0 = (int)((long long)x * w / W), x1 = (int)((long long)(x + 1) * w / W), sx, sy, n = 0;
            unsigned r = 0, g = 0, b = 0;
            if (x1 <= x0) x1 = x0 + 1;
            for (sy = y0; sy < y1; sy += (y1 - y0 > 4 ? (y1 - y0) / 4 : 1))         /* up to 4x4 samples per pixel */
                for (sx = x0; sx < x1; sx += (x1 - x0 > 4 ? (x1 - x0) / 4 : 1)) {
                    const unsigned char *p = src + ((size_t)sy * w + sx) * 3;
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            o[((size_t)y * W + x) * 4 + 0] = (unsigned char)(r / n);
            o[((size_t)y * W + x) * 4 + 1] = (unsigned char)(g / n);
            o[((size_t)y * W + x) * 4 + 2] = (unsigned char)(b / n);
            o[((size_t)y * W + x) * 4 + 3] = 255;
        }
    }
    *ow = W; *oh = H;
    return o;
}

static void *img_thread(void *x)
{
    (void)x;
    for (;;) {
        slot_t *s = NULL;
        int i, w, h, n, g;
        char path[4096], thumb[4200];
        long off = 0, len = 0;
        unsigned char *raw = NULL, *out = NULL;
        thumb[0] = 0;
        pthread_mutex_lock(&img_mu);
        for (;;) {
            double best = -1;
            for (i = 0; i < NSLOT; i++) if (slots[i].state == 1 && slots[i].used > best) { best = slots[i].used; s = &slots[i]; }   /* fresh ones first */
            if (s) break;
            pthread_cond_wait(&img_cv, &img_mu);
        }
        s->state = 2;
        g = img_gen;
        if (s->kind == K_ART) { snprintf(path, sizeof path, "%s", TRACKS[s->idx].path); off = TRACKS[s->idx].art_off; len = TRACKS[s->idx].art_len; }
        else { snprintf(path, sizeof path, "%s", IMAGES[s->idx].path); thumb_path(thumb, sizeof thumb, &IMAGES[s->idx]); }
        pthread_mutex_unlock(&img_mu);
        if (s->kind == K_ART) {
            FILE *f = fopen(path, "rb");
            unsigned char *b = len > 0 && len < 8 * 1024 * 1024 ? malloc((size_t)len) : NULL;
            if (f && b && !fseek(f, off, SEEK_SET) && fread(b, 1, (size_t)len, f) == (size_t)len) raw = stbi_load_from_memory(b, (int)len, &w, &h, &n, 3);
            if (f) fclose(f);
            free(b);
            out = raw ? shrink(raw, w, h, 600, &w, &h) : NULL;
            stbi_image_free(raw);
        } else if (s->kind == K_THUMB && thumb[0] && (raw = stbi_load(thumb, &w, &h, &n, 3))) {   /* ready thumbnail */
            out = shrink(raw, w, h, THUMB, &w, &h);
            stbi_image_free(raw);
        } else {
            unsigned char *rgb = load_rgb(path, s->kind == K_FULL ? 1280 : THUMB, &w, &h);
            out = rgb ? shrink(rgb, w, h, s->kind == K_FULL ? 1280 : THUMB, &w, &h) : NULL;
            free(rgb);
            if (out && s->kind == K_THUMB && thumb[0]) {                /* save the thumbnail to the stick */
                char dir[300], tmp[300];
                snprintf(dir, sizeof dir, "%s/.thumbnails", scan_root);
                mkdir(dir, 0755);
                snprintf(tmp, sizeof tmp, "%s.tmp", thumb);
                if (stbi_write_jpg(tmp, w, h, 4, out, 85)) rename(tmp, thumb); else unlink(tmp);
            }
        }
        pthread_mutex_lock(&img_mu);
        if (g != img_gen) { free(out); s->state = 0; }                    /* the stick was changed while decoding */
        else if (out) { s->px = out; s->w = w; s->h = h; s->state = 3; }
        else s->state = 5;
        pthread_mutex_unlock(&img_mu);
    }
    return NULL;
}

static gfx_tex *get_img(int kind, int idx)
{
    int i, free_i = -1;
    double old = 1e18;
    gfx_tex *r = NULL;
    if (!img_started) { pthread_t t; img_started = 1; if (!pthread_create(&t, NULL, img_thread, NULL)) pthread_detach(t); }
    pthread_mutex_lock(&img_mu);
    for (i = 0; i < NSLOT; i++) if (slots[i].state && slots[i].kind == kind && slots[i].idx == idx) break;
    if (i < NSLOT) {
        slots[i].used = clock_now;
        if (slots[i].state == 4) r = &slots[i].tex;
    } else {
        for (i = 0; i < NSLOT; i++) {                     /* free or the one not needed for the longest time (not in work) */
            if (!slots[i].state) { free_i = i; break; }
            if ((slots[i].state == 4 || slots[i].state == 5) && slots[i].used < old) { old = slots[i].used; free_i = i; }
        }
        if (free_i >= 0) {
            slot_t *s = &slots[free_i];
            if (s->state == 4) gfx_tex_free(&s->tex);
            s->kind = kind; s->idx = idx; s->state = 1; s->used = clock_now;
            pthread_cond_signal(&img_cv);
        }
    }
    pthread_mutex_unlock(&img_mu);
    return r;
}

gfx_tex *media_thumb(int i) { return i >= 0 && i < NIMAGES ? get_img(K_THUMB, i) : NULL; }
gfx_tex *media_full(int i) { return i >= 0 && i < NIMAGES ? get_img(K_FULL, i) : NULL; }
gfx_tex *media_art(int t) { return t >= 0 && t < NTRACKS && TRACKS[t].art_len > 0 ? get_img(K_ART, t) : NULL; }

static void img_reset(void)
{
    int i;
    pthread_mutex_lock(&img_mu);
    img_gen++;
    for (i = 0; i < NSLOT; i++) {
        if (slots[i].state == 4) gfx_tex_free(&slots[i].tex);
        if (slots[i].state == 3) free(slots[i].px);
        if (slots[i].state != 2) slots[i].state = 0;     /* the one being decoded will be discarded by the thread itself */
    }
    pthread_mutex_unlock(&img_mu);
}

/* wallpaper from a stick photo: downscale to 1280 (the GPU does not take huge ones), apply the theme dimming; synchronous (on press) */
static int wall_load(gfx_tex *t, const char *path, col ov)
{
    int w, h, k;
    unsigned char *raw, *px;
    if (!(raw = load_rgb(path, 1280, &w, &h))) return -1;
    px = shrink(raw, w, h, 1280, &w, &h);
    free(raw);
    if (!px) return -1;
    for (k = 0; k < w * h * 4; k += 4) {
        px[k + 0] = (unsigned char)(px[k + 0] * (1 - ov.a) + ov.r * 255 * ov.a + 0.5f);
        px[k + 1] = (unsigned char)(px[k + 1] * (1 - ov.a) + ov.g * 255 * ov.a + 0.5f);
        px[k + 2] = (unsigned char)(px[k + 2] * (1 - ov.a) + ov.b * 255 * ov.a + 0.5f);
    }
    gfx_tex_rgba(t, w, h, px);
    free(px);
    return 0;
}

int media_wallpaper(gfx_tex *t, int i, col ov) { return i < 0 || i >= NIMAGES ? -1 : wall_load(t, IMAGES[i].path, ov); }

/* "From USB" wallpaper - a copy in the data partition (owner: stick removed - the wallpaper stays), already at screen size */
static gfx_tex wall_th;
static int wall_th_st;                   /* 0 - not loaded, 1 - present, -1 - no file */

int media_wallpaper_file(gfx_tex *t, col ov)
{
    char p[512];
    data_path(p, sizeof p, "wallpaper.jpg");
    return wall_load(t, p, ov);
}

int media_wallpaper_save(int i)
{
    char p[512], tmp[520];
    int w, h, ok;
    unsigned char *raw, *px;
    if (i < 0 || i >= NIMAGES || data_dir() < 0 || !(raw = load_rgb(IMAGES[i].path, 1280, &w, &h))) return -1;
    px = shrink(raw, w, h, 1280, &w, &h);
    free(raw);
    if (!px) return -1;
    data_path(p, sizeof p, "wallpaper.jpg");
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    ok = stbi_write_jpg(tmp, w, h, 4, px, 90) && !data_commit(tmp, p);
    free(px);
    if (!ok) { unlink(tmp); fprintf(stderr, "hu: wallpaper: save failed\n"); return -1; }
    if (wall_th_st > 0) gfx_tex_free(&wall_th);
    wall_th_st = 0;
    return 0;
}

gfx_tex *media_wallpaper_thumb(void)
{
    if (!wall_th_st) {
        char p[512];
        int w, h;
        unsigned char *raw, *px = NULL;
        data_path(p, sizeof p, "wallpaper.jpg");
        if ((raw = load_rgb(p, 320, &w, &h))) { px = shrink(raw, w, h, 320, &w, &h); free(raw); }
        if (px) { gfx_tex_rgba(&wall_th, w, h, px); free(px); wall_th_st = 1; } else wall_th_st = -1;
    }
    return wall_th_st > 0 ? &wall_th : NULL;
}

/* ================================================================= main loop */

void media_poll(double now)
{
    int i, up = 0;
    clock_now = now;
    if (A.usb_conn && scanned_for != 1 && scan_state == 0) {          /* inserted - scan */
        pthread_t t;
        snprintf(scan_root, sizeof scan_root, "%s", A.usb_mnt);
        scan_abort = 0; scan_state = 1; scanned_for = 1;
        if (pthread_create(&t, NULL, scan_thread, NULL)) scan_state = 0; else pthread_detach(t);
    }
    if (!A.usb_conn && scanned_for) {                                 /* removed - lists are empty */
        scan_abort = 1; scanned_for = 0;
        player_stop();
        img_reset();
        pthread_mutex_lock(&img_mu);
        free_lists(TRACKS, NTRACKS, VIDEOS, NVIDEOS, IMAGES, NIMAGES); TRACKS = NULL; VIDEOS = NULL; IMAGES = NULL;
        NTRACKS = NVIDEOS = NIMAGES = 0;
        pthread_mutex_unlock(&img_mu);
        gen++;
        base_dirty(); screen_dirty(); layout_again();
    }
    if (scan_state == 2) {
        scan_state = 0;
        if (A.usb_conn && !scan_abort) {
            img_reset();
            pthread_mutex_lock(&img_mu);                          /* the image thread reads the paths - under lock */
            free_lists(TRACKS, NTRACKS, VIDEOS, NVIDEOS, IMAGES, NIMAGES);
            TRACKS = s_tr; NTRACKS = s_nt; VIDEOS = s_vi; NVIDEOS = s_nv; IMAGES = s_im; NIMAGES = s_ni;
            s_tr = NULL; s_vi = NULL; s_im = NULL;
            pthread_mutex_unlock(&img_mu);
            if (A.trk >= NTRACKS) A.trk = 0;
            if (A.vsel >= NVIDEOS) A.vsel = 0;
            gen++;
            fprintf(stderr, "hu: usb: %d tracks, %d videos, %d images\n", NTRACKS, NVIDEOS, NIMAGES);
            base_dirty(); screen_dirty(); layout_again();
        } else { free_lists(s_tr, s_nt, s_vi, s_nv, s_im, s_ni); s_tr = NULL; s_vi = NULL; s_im = NULL; }   /* the stick was removed during scanning */
    }
    pthread_mutex_lock(&img_mu);                                      /* ready pixels -> GPU */
    for (i = 0; i < NSLOT; i++)
        if (slots[i].state == 1 && now - slots[i].used > 0.5) { slots[i].state = 0; up = 1; }   /* not requested for a long time (left the screen) - discard; a redraw will request the visible ones again */
        else if (slots[i].state == 3) {
            gfx_tex_rgba(&slots[i].tex, slots[i].w, slots[i].h, slots[i].px);
            free(slots[i].px); slots[i].px = NULL;
            slots[i].state = 4;
            up = 1;
        }
    pthread_mutex_unlock(&img_mu);
    if (up) { screen_dirty(); base_dirty(); }
}

