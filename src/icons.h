/* Icons from the atlas (white, mipmaps), color is set at draw time. */
#ifndef ICONS_H
#define ICONS_H

#include "gfx.h"

int  icons_init(const char *dir);      /* dir/icons.png + dir/icons.txt */
void icons_shutdown(void);
/* name: "music", "music_w24", ... ; (x, y) - top-left corner, size - side length in screen pixels */
void icon_draw(const char *name, float x, float y, float size, col c);
/* centered on the point (cx, cy) */
void icon_draw_c(const char *name, float cx, float cy, float size, col c);

/* rotated about the center, degrees (x, y - top-left corner) */
void icon_draw_rot(const char *name, float x, float y, float size, col c, float deg);

#endif
