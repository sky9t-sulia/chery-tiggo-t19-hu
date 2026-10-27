/* Shell UI: column, screens, bottom climate strip, HUD, transitions.
 * Draws only the changed rectangles (scissor); static content comes from gfx cache layers. */
#ifndef UI_H
#define UI_H

#include "platform.h"

int  ui_init(const char *assets_dir);
void ui_shutdown(void);
int  ui_video_active(void);             /* video is playing - the main loop sleeps less (frame aimed at the nearest scanout) */
void ui_event(const pf_event *ev, double now);
/* 1 - frame drawn (pf_swap needed), 0 - nothing changed, nothing to draw */
int  ui_frame(double now, int buffer_age);

int  ui_screen(void);
void ui_mcu_ready(void);               /* MCU is connected: brightness is hardware */                  /* current screen (for the log) */

#endif
