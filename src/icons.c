#include "icons.h"

#include <stdio.h>
#include <string.h>

typedef struct { char name[32]; float u0, v0, u1, v1; } icon;

#define MAX_ICONS 256                  /* atlas 1024x2048 - up to 210 cells */
static icon tab[MAX_ICONS];
static int n;
static gfx_tex atlas;

int icons_init(const char *dir)
{
    char path[512], nm[64];
    int x, y, w, h;
    FILE *fp;
    snprintf(path, sizeof path, "%s/icons.png", dir);
    if (gfx_tex_load(&atlas, path, 1)) return -1;
    snprintf(path, sizeof path, "%s/icons.txt", dir);
    fp = fopen(path, "r");
    if (!fp) { fprintf(stderr, "hu: no %s\n", path); gfx_tex_free(&atlas); return -1; }
    while (n < MAX_ICONS && fscanf(fp, "%63s %d %d %d %d", nm, &x, &y, &w, &h) == 5) {
        snprintf(tab[n].name, sizeof tab[n].name, "%s", nm);
        tab[n].u0 = (float)x / atlas.w; tab[n].v0 = (float)y / atlas.h;
        tab[n].u1 = (float)(x + w) / atlas.w; tab[n].v1 = (float)(y + h) / atlas.h;
        n++;
    }
    fclose(fp);
    return 0;
}

void icons_shutdown(void)
{
    gfx_tex_free(&atlas);
    n = 0;
}

static const icon *find(const char *name)
{
    int i;
    for (i = 0; i < n; i++) if (!strcmp(tab[i].name, name)) return &tab[i];
    return NULL;
}

void icon_draw(const char *name, float x, float y, float size, col c)
{
    const icon *ic = find(name);
    if (!ic) { static int warned; if (!warned++) fprintf(stderr, "hu: no icon %s\n", name); return; }
    gfx_image(&atlas, x, y, size, size, ic->u0, ic->v0, ic->u1, ic->v1, c);
}

void icon_draw_c(const char *name, float cx, float cy, float size, col c)
{
    icon_draw(name, cx - size * 0.5f, cy - size * 0.5f, size, c);
}

void icon_draw_rot(const char *name, float x, float y, float size, col c, float deg)
{
    const icon *ic = find(name);
    if (!ic) return;
    gfx_image_rot(&atlas, x, y, size, size, ic->u0, ic->v0, ic->u1, ic->v1, c, deg);
}
