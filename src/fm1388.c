/* FM1388 via libfm1388.so, see fm1388.h. Like ShellApp: ctx = malloc(56); lib_initial(ctx);
 * h = lib_open_device(ctx, 0 - non-exclusive, &err); mode - set_mode_2(h, m) if get_mode(h) differs
 * (set_mode without _2 silently skips modes 2 and 7). */
#include "fm1388.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static void *h;
static int tried;
static int (*get_mode)(void *);
static int (*set_mode_2)(void *, int);

static void open_dev(void)
{
    void *lib, *ctx;
    int (*initial)(void *);
    void *(*open_device)(void *, int, int *);
    int err = 0;
    tried = 1;
    lib = dlopen("libfm1388.so", RTLD_NOW);
    if (!lib) return;                                   /* PC */
    initial = (int (*)(void *))dlsym(lib, "lib_initial");
    open_device = (void *(*)(void *, int, int *))dlsym(lib, "lib_open_device");
    get_mode = (int (*)(void *))dlsym(lib, "get_mode");
    set_mode_2 = (int (*)(void *, int))dlsym(lib, "set_mode_2");
    if (!initial || !open_device || !get_mode || !set_mode_2 || !(ctx = malloc(56))) { fprintf(stderr, "hu: fm1388: no api\n"); return; }
    if (!initial(ctx)) { fprintf(stderr, "hu: fm1388: lib_initial failed\n"); return; }
    h = open_device(ctx, 0, &err);
    fprintf(stderr, "hu: fm1388: %s (err %d)\n", h ? "open" : "open failed", err);
}

void fm1388_set_mode(int mode)
{
    int cur;
    if (!tried) open_dev();
    if (!h) return;
    cur = get_mode(h);
    if (cur == mode) return;
    set_mode_2(h, mode);
    fprintf(stderr, "hu: fm1388: mode %d -> %d (now %d)\n", cur, mode, get_mode(h));
}
