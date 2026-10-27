/* Platform: window + EGL/GLES2 context, input (finger / mouse), time.
 * Implementations: platform_fb.c (HU: Vivante fbdev, evdev touchscreen), platform_wl.c (PC: Wayland). */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <time.h>

enum { PF_NONE = 0, PF_DOWN, PF_MOVE, PF_UP, PF_QUIT };

typedef struct {
    int type;
    float x, y;     /* screen pixels */
} pf_event;

int  pf_init(int w, int h);     /* 0 - ok */
int  pf_poll(pf_event *ev);     /* 1 - event received, 0 - queue empty */
void pf_swap(void);             /* present the frame (waits for vsync) */
void pf_console(void);       /* screen goes to the kernel console (tty): fb0 to the first buffer (yoffset 0) */
int  pf_buffer_age(void);       /* how many frames ago the current back buffer was drawn (0 - unknown) */
void pf_shutdown(void);

static inline double pf_now(void)   /* seconds, monotonic real clock */
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#endif
