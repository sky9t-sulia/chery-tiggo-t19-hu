/* HU: output straight to video memory via Vivante EGL (fbdev), no weston. Touchscreen is evdev.
 * Touchscreen: /dev/input/event1 (sitronix-i2c-touch-mt), can be overridden with HU_TOUCH=/dev/input/eventN. */
#include "platform.h"
#include "gl.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Vivante fbdev API (libEGL.so on the HU) */
extern void *fbGetDisplayByIndex(int index);
extern void *fbCreateWindow(void *display, int x, int y, int width, int height);
extern void  fbDestroyWindow(void *window);
extern void  fbDestroyDisplay(void *display);

static void *fb_dpy, *fb_win;
static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLContext ctx = EGL_NO_CONTEXT;
static EGLSurface surf = EGL_NO_SURFACE;

static int scr_w, scr_h;
static int tfd = -1;                         /* touchscreen */
static int ax_min, ax_max, ay_min, ay_max;   /* touch coordinate range */
static int cur_x, cur_y, touching, was_touching, moved;

static void touch_open(void)
{
    const char *path = getenv("HU_TOUCH");
    struct input_absinfo ai;
    if (!path) path = "/dev/input/event1";
    tfd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (tfd < 0) { fprintf(stderr, "hu: touch %s: not available\n", path); return; }
    ax_min = 0; ax_max = scr_w - 1; ay_min = 0; ay_max = scr_h - 1;
    if (ioctl(tfd, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0 && ai.maximum > ai.minimum) { ax_min = ai.minimum; ax_max = ai.maximum; }
    else if (ioctl(tfd, EVIOCGABS(ABS_X), &ai) == 0 && ai.maximum > ai.minimum) { ax_min = ai.minimum; ax_max = ai.maximum; }
    if (ioctl(tfd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0 && ai.maximum > ai.minimum) { ay_min = ai.minimum; ay_max = ai.maximum; }
    else if (ioctl(tfd, EVIOCGABS(ABS_Y), &ai) == 0 && ai.maximum > ai.minimum) { ay_min = ai.minimum; ay_max = ai.maximum; }
    fprintf(stderr, "hu: touch %s x[%d..%d] y[%d..%d]\n", path, ax_min, ax_max, ay_min, ay_max);
}

#ifndef EGL_BUFFER_AGE_EXT
#define EGL_BUFFER_AGE_EXT 0x313D
#endif
static int has_age;

static void log_ext(void)
{
    const char *e = eglQueryString(dpy, EGL_EXTENSIONS);
    has_age = e && strstr(e, "EGL_EXT_buffer_age") != NULL;
    fprintf(stderr, "hu: EGL %s\nhu: EGL ext: %s\n", eglQueryString(dpy, EGL_VERSION), e ? e : "-");
}

/* the kernel console draws at the start of fb0, but we may have left the second buffer displayed (FB_MULTI_BUFFER) */
void pf_console(void)
{
    struct fb_var_screeninfo v;
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) return;
    if (!ioctl(fd, FBIOGET_VSCREENINFO, &v)) { v.xoffset = v.yoffset = 0; ioctl(fd, FBIOPAN_DISPLAY, &v); }
    close(fd);
}

int pf_buffer_age(void)
{
    EGLint age = 0;
    if (!has_age || !eglQuerySurface(dpy, surf, EGL_BUFFER_AGE_EXT, &age)) return 0;
    return age;
}

int pf_init(int w, int h)
{
    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;

    scr_w = w; scr_h = h;
    fb_dpy = fbGetDisplayByIndex(0);
    if (!fb_dpy) { fprintf(stderr, "hu: fbGetDisplayByIndex failed\n"); return -1; }
    fb_win = fbCreateWindow(fb_dpy, 0, 0, w, h);
    if (!fb_win) { fprintf(stderr, "hu: fbCreateWindow failed\n"); pf_shutdown(); return -1; }

    dpy = eglGetDisplay((EGLNativeDisplayType)fb_dpy);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) { fprintf(stderr, "hu: eglInitialize failed\n"); pf_shutdown(); return -1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) { fprintf(stderr, "hu: no EGL config\n"); pf_shutdown(); return -1; }
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)fb_win, NULL);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "hu: EGL context/surface failed 0x%x\n", eglGetError()); pf_shutdown(); return -1;
    }
    eglSwapInterval(dpy, 1);
    log_ext();
    touch_open();
    return 0;
}

int pf_poll(pf_event *ev)
{
    struct input_event ie;
    if (tfd < 0) return 0;
    while (read(tfd, &ie, sizeof ie) == (ssize_t)sizeof ie) {
        if (ie.type == EV_ABS) {
            if (ie.code == ABS_MT_POSITION_X || ie.code == ABS_X) { cur_x = ie.value; moved = 1; }
            else if (ie.code == ABS_MT_POSITION_Y || ie.code == ABS_Y) { cur_y = ie.value; moved = 1; }
            else if (ie.code == ABS_MT_TRACKING_ID) touching = ie.value >= 0;
        } else if (ie.type == EV_KEY && ie.code == BTN_TOUCH) {
            touching = ie.value != 0;
        } else if (ie.type == EV_SYN && ie.code == SYN_REPORT) {
            ev->x = (float)(cur_x - ax_min) * (float)scr_w / (float)(ax_max - ax_min + 1);
            ev->y = (float)(cur_y - ay_min) * (float)scr_h / (float)(ay_max - ay_min + 1);
            ev->type = PF_NONE;
            if (touching && !was_touching) ev->type = PF_DOWN;
            else if (!touching && was_touching) ev->type = PF_UP;
            else if (touching && moved) ev->type = PF_MOVE;
            was_touching = touching; moved = 0;
            if (ev->type != PF_NONE) return 1;
        }
    }
    return 0;
}

void pf_swap(void)
{
    eglSwapBuffers(dpy, surf);
}

void pf_shutdown(void)
{
    if (tfd >= 0) { close(tfd); tfd = -1; }
    if (dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
        if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
        eglTerminate(dpy);
        dpy = EGL_NO_DISPLAY; surf = EGL_NO_SURFACE; ctx = EGL_NO_CONTEXT;
    }
    if (fb_win) { fbDestroyWindow(fb_win); fb_win = NULL; }
    if (fb_dpy) { fbDestroyDisplay(fb_dpy); fb_dpy = NULL; }
}
