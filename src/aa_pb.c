/* Minimal protobuf (see aa_pb.h). A nested message is written in place with 5 bytes reserved for the length; pb_end
 * shifts the body to the real varint length. */
#include "aa_pb.h"

#include <string.h>

void pb_init(pb_w *w, void *buf, size_t cap) { w->b = buf; w->n = 0; w->cap = cap; w->err = 0; w->depth = 0; }

static void put(pb_w *w, uint8_t c) { if (w->n < w->cap) w->b[w->n++] = c; else w->err = 1; }
static void put_var(pb_w *w, uint64_t v) { while (v >= 0x80) { put(w, (uint8_t)(v | 0x80)); v >>= 7; } put(w, (uint8_t)v); }
static void tag(pb_w *w, int field, int wire) { put_var(w, (uint64_t)field << 3 | (unsigned)wire); }

void pb_varint(pb_w *w, int field, uint64_t v) { tag(w, field, 0); put_var(w, v); }
void pb_bool(pb_w *w, int field, int v) { pb_varint(w, field, v ? 1 : 0); }
void pb_bytes(pb_w *w, int field, const void *p, size_t n)
{
    tag(w, field, 2); put_var(w, n);
    if (w->n + n > w->cap) { w->err = 1; return; }
    memcpy(w->b + w->n, p, n); w->n += n;
}
void pb_str(pb_w *w, int field, const char *s) { pb_bytes(w, field, s, strlen(s)); }

void pb_begin(pb_w *w, int field)
{
    tag(w, field, 2);
    if (w->depth >= 8 || w->n + 5 > w->cap) { w->err = 1; return; }
    w->stack[w->depth++] = w->n;
    w->n += 5;
}

void pb_end(pb_w *w)
{
    size_t at, body, k = 0;
    uint8_t v[5];
    uint64_t len;
    if (w->err || !w->depth) { w->err = 1; return; }
    at = w->stack[--w->depth]; body = at + 5; len = w->n - body;
    do { v[k++] = (uint8_t)((len & 0x7F) | (len >= 0x80 ? 0x80 : 0)); len >>= 7; } while (len);
    memcpy(w->b + at, v, k);
    memmove(w->b + at + k, w->b + body, w->n - body);
    w->n -= 5 - k;
}

static int get_var(pb_r *r, uint64_t *v)
{
    int s = 0;
    *v = 0;
    while (r->p < r->end && s < 64) {
        uint8_t c = *r->p++;
        *v |= (uint64_t)(c & 0x7F) << s;
        if (!(c & 0x80)) return 0;
        s += 7;
    }
    return -1;
}

int pb_next(pb_r *r, int *field, int *wire, uint64_t *v, const uint8_t **p, size_t *len)
{
    uint64_t t;
    if (r->p >= r->end) return 0;
    if (get_var(r, &t) < 0) return -1;
    *field = (int)(t >> 3); *wire = (int)(t & 7); *v = 0; *p = NULL; *len = 0;
    switch (*wire) {
    case 0: return get_var(r, v) < 0 ? -1 : 1;
    case 1: if (r->end - r->p < 8) return -1; *p = r->p; *len = 8; r->p += 8; return 1;
    case 5: if (r->end - r->p < 4) return -1; *p = r->p; *len = 4; r->p += 4; return 1;
    case 2:
        if (get_var(r, &t) < 0 || (uint64_t)(r->end - r->p) < t) return -1;
        *p = r->p; *len = (size_t)t; r->p += t; return 1;
    }
    return -1;
}
