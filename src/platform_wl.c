/* PC: Wayland window (xdg-shell) + EGL on Mesa. The mouse acts as a finger (LMB = touch). */
#include "platform.h"
#include "gl.h"
#include "xdg-shell-client-protocol.h"

#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl.h>

static struct wl_display *wd;
static struct wl_registry *reg;
static struct wl_compositor *comp;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct wl_pointer *ptr;
static struct wl_surface *wsurf;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *top;
static struct wl_egl_window *ewin;

static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLContext ctx = EGL_NO_CONTEXT;
static EGLSurface surf = EGL_NO_SURFACE;

static int configured, quit;
static float px, py;
static int pressed;

#define QN 64
static pf_event q[QN];
static int qh, qt;
static void push(int type) { int n = (qt + 1) % QN; if (n == qh) return; q[qt].type = type; q[qt].x = px; q[qt].y = py; qt = n; }

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t s) { (void)d; xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };
static void xs_conf(void *d, struct xdg_surface *s, uint32_t serial) { (void)d; xdg_surface_ack_configure(s, serial); configured = 1; }
static const struct xdg_surface_listener xs_l = { xs_conf };
static void top_conf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) { (void)d; (void)t; (void)w; (void)h; (void)st; }
static void top_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; quit = 1; }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) { (void)d; (void)t; (void)c; }
static const struct xdg_toplevel_listener top_l = { top_conf, top_close, top_bounds, top_caps };

static void p_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)p; (void)s; (void)sf; px = (float)wl_fixed_to_double(x); py = (float)wl_fixed_to_double(y); }
static void p_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf)
{ (void)d; (void)p; (void)s; (void)sf; if (pressed) { pressed = 0; push(PF_UP); } }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)p; (void)t; px = (float)wl_fixed_to_double(x); py = (float)wl_fixed_to_double(y); if (pressed) push(PF_MOVE); }
static void p_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st)
{ (void)d; (void)p; (void)s; (void)t; if (b != BTN_LEFT) return; pressed = st == WL_POINTER_BUTTON_STATE_PRESSED; push(pressed ? PF_DOWN : PF_UP); }
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_l = { p_enter, p_leave, p_motion, p_button, p_axis, NULL, NULL, NULL, NULL, NULL, NULL };

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ptr) { ptr = wl_seat_get_pointer(s); wl_pointer_add_listener(ptr, &ptr_l, NULL); }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_l = { seat_caps, seat_name };

static void reg_add(void *d, struct wl_registry *r, uint32_t id, const char *iface, uint32_t v)
{
    (void)d; (void)v;
    if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, id, &wl_compositor_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) { wm = wl_registry_bind(r, id, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(wm, &wm_l, NULL); }
    else if (!strcmp(iface, wl_seat_interface.name) && !seat) { seat = wl_registry_bind(r, id, &wl_seat_interface, 1); wl_seat_add_listener(seat, &seat_l, NULL); }
}
static void reg_rm(void *d, struct wl_registry *r, uint32_t id) { (void)d; (void)r; (void)id; }
static const struct wl_registry_listener reg_l = { reg_add, reg_rm };

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

void pf_console(void) {}

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
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_NONE };
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;

    wd = wl_display_connect(NULL);
    if (!wd) { fprintf(stderr, "hu: no wayland display\n"); return -1; }
    reg = wl_display_get_registry(wd);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(wd);
    wl_display_roundtrip(wd);
    if (!comp || !wm) { fprintf(stderr, "hu: compositor lacks xdg-shell\n"); pf_shutdown(); return -1; }

    wsurf = wl_compositor_create_surface(comp);
    xsurf = xdg_wm_base_get_xdg_surface(wm, wsurf);
    xdg_surface_add_listener(xsurf, &xs_l, NULL);
    top = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_add_listener(top, &top_l, NULL);
    xdg_toplevel_set_title(top, "Car Media (hu)");
    xdg_toplevel_set_min_size(top, w, h);
    xdg_toplevel_set_max_size(top, w, h);
    wl_surface_commit(wsurf);
    while (!configured && wl_display_dispatch(wd) >= 0) {}

    ewin = wl_egl_window_create(wsurf, w, h);
    dpy = eglGetDisplay((EGLNativeDisplayType)wd);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) { fprintf(stderr, "hu: eglInitialize failed\n"); pf_shutdown(); return -1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) { fprintf(stderr, "hu: no EGL config\n"); pf_shutdown(); return -1; }
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)ewin, NULL);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "hu: EGL context/surface failed 0x%x\n", eglGetError()); pf_shutdown(); return -1;
    }
    eglSwapInterval(dpy, 1);
    log_ext();
    return 0;
}

int pf_poll(pf_event *ev)
{
    struct pollfd p;
    if (!wd) return 0;
    while (wl_display_prepare_read(wd) != 0) wl_display_dispatch_pending(wd);
    wl_display_flush(wd);
    p.fd = wl_display_get_fd(wd); p.events = POLLIN; p.revents = 0;
    if (poll(&p, 1, 0) > 0) wl_display_read_events(wd); else wl_display_cancel_read(wd);
    wl_display_dispatch_pending(wd);
    if (quit) { quit = 0; ev->type = PF_QUIT; ev->x = ev->y = 0; return 1; }
    if (qh == qt) return 0;
    *ev = q[qh]; qh = (qh + 1) % QN;
    return 1;
}

void pf_swap(void)
{
    eglSwapBuffers(dpy, surf);
}

void pf_shutdown(void)
{
    if (dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
        if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
        eglTerminate(dpy);
        eglReleaseThread();
        dpy = EGL_NO_DISPLAY; surf = EGL_NO_SURFACE; ctx = EGL_NO_CONTEXT;
    }
    if (ewin) { wl_egl_window_destroy(ewin); ewin = NULL; }
    if (top) { xdg_toplevel_destroy(top); top = NULL; }
    if (xsurf) { xdg_surface_destroy(xsurf); xsurf = NULL; }
    if (wsurf) { wl_surface_destroy(wsurf); wsurf = NULL; }
    if (ptr) { wl_pointer_destroy(ptr); ptr = NULL; }
    if (seat) { wl_seat_destroy(seat); seat = NULL; }
    if (wm) { xdg_wm_base_destroy(wm); wm = NULL; }
    if (comp) { wl_compositor_destroy(comp); comp = NULL; }
    if (reg) { wl_registry_destroy(reg); reg = NULL; }
    if (wd) { wl_display_disconnect(wd); wd = NULL; }
}
