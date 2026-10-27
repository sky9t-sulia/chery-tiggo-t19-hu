/* Lua programs: <folder>/ manifest.lua (description), main.lua (program), icon.png (icon).
 * Built-in ones are in NAND (LUA_NAND_DIR, work without a stick), the rest in <USB stick>/apps (the same folder on the stick wins). A program draws in the "Running app" window with system elements (the ui table:
 * buttons with press animation, text in baked fonts, icons, theme colors) and receives touches.
 * Sandbox: no io / os (except time) / package / load / dofile, memory and instruction limit per call.
 * How to write programs - apps/README.md. */
#ifndef LUA_APP_H
#define LUA_APP_H

#include "gfx.h"

#include <stdint.h>

typedef struct {
    char *dir;                          /* full path of the program folder (on the heap) */
    char id[64];                        /* folder name */
    char name[64], desc[128], version[24], author[64];
    uint32_t color;                     /* tile color if there is no icon.png */
    long main_size;
    gfx_tex icon;                       /* icon.png; id 0 - none */
    int usb;                            /* from the stick (removed - close) */
} lua_app;

#define LUA_NAND_DIR "/mnt/data1/hu-apps"   /* built-in programs (the user allowed writing, 2026-10-22) */

void lua_apps_scan(const char *mnt);    /* rescan: NAND + <mnt>/apps (mnt NULL - no stick) */
int  lua_apps_count(void);
const lua_app *lua_apps_get(int i);
int  lua_apps_find(const char *dir);    /* program by its folder path, -1 - none */

int  lua_app_start(int i);              /* load and run main.lua; -1 - error (to log) */
void lua_app_stop(void);
int  lua_app_running(void);             /* index of the running one, -1 - none */
int  lua_app_failed(void);              /* 1 - program crashed (error to log), 2 - called ui.close(): the core closes the window */
/* program canvas (design px): the program draws via ui.*, alpha - window opacity */
void lua_app_draw(float x, float y, float w, float h, double now);
void lua_app_touch(int phase, float x, float y, double now);   /* 0 down, 1 move, 2 up; coordinates - screen design px */
int  lua_app_busy(double now);          /* redraw needed (button animations, ui.redraw, tick) */

#endif
