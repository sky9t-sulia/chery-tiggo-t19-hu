/* Minimal protobuf for Android Auto messages: writing into a buffer (nested ones via pb_begin/pb_end) and parsing
 * by field. No protobuf library needed. */
#ifndef AA_PB_H
#define AA_PB_H

#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t *b; size_t n, cap; int err; size_t stack[8]; int depth; } pb_w;

void pb_init(pb_w *w, void *buf, size_t cap);
void pb_varint(pb_w *w, int field, uint64_t v);
void pb_bool(pb_w *w, int field, int v);
void pb_bytes(pb_w *w, int field, const void *p, size_t n);
void pb_str(pb_w *w, int field, const char *s);
void pb_begin(pb_w *w, int field);           /* nested message: length is filled in by pb_end */
void pb_end(pb_w *w);

/* parsing: pb_next - next field (1) or end (0) / error (-1); varint in *v, length-delimited in *p and *len */
typedef struct { const uint8_t *p, *end; } pb_r;
int pb_next(pb_r *r, int *field, int *wire, uint64_t *v, const uint8_t **p, size_t *len);

#endif
