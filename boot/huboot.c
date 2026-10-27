/* huboot - boot screen (design v34 Boot.dc.html) shown until the shell starts. A separate tiny program: no GPU and
* libraries, only /dev/fb0. Starts first (/etc/rcS.d/S01huboot, in the background), the picture is from the design (boot_img.h,
* compressed), only the running segment is drawn on top (owner: no fade-in and no blinking dots, fixed color).
* The MCU turns on the screen backlight, and it stays silent until someone talks to it (in the car 2026-10-25: huboot drew from 2.6 s,
* the screen stayed dark until hu at 13 s) - so huboot talks to the MCU itself with the same mcu.c as hu, but without startup (mcu_light_only): acks, MCU keep-alive,
* brightness - maximum (owner; hu sets the saved one later). hu closes huboot before its own mcu_open and waits for it to exit -
* one at a time on the port.
* MCU only after the hardware GPIOs (S08 tiggo: 56/91/87): earlier the MCU turned on the amplifier while the HU pins were not ready - a squeal
* of ~1 s (in the car 2026-10-25); the stock shell and hu also talked to the MCU only after them. No GPIO for 8 s - open anyway.
* Exit: SIGTERM (hu before the MCU, tiggo before the stock UI) or on its own after 120 s. */
#include "boot_img.h"
#include "mcu.h"

#include <fcntl.h>
#include <stdio.h>
#include <linux/fb.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* line: left 50% - 190, top 372, 380 x 2; segment 90 x 2, translateX -90 -> 380 over 1.6 s, cubic-bezier(.65,0,.35,1),
* clipped by the line frame (overflow hidden - the box-shadow glow is not visible) */
#define LX (512 - 190)
#define LY 372
#define LW 380
#define RW 90
static const unsigned char TRACK[3] = { 0x1E, 0x1E, 0x22 }, RUN[3] = { 0x3B, 0x8B, 0xFF };

static volatile sig_atomic_t quit;
static void on_term(int s) { (void)s; quit = 1; }

static unsigned char *fb;
static int bpp, stride;
static struct fb_var_screeninfo v;

static void put(int x, int y, const unsigned char c[3])
{
    unsigned char *p = fb + y * stride + x * (bpp / 8);
    unsigned r = c[0], g = c[1], b = c[2];
    if (bpp == 16) {
        unsigned short w = (unsigned short)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
        memcpy(p, &w, 2);
    } else {
        unsigned w = r << v.red.offset | g << v.green.offset | b << v.blue.offset | (v.transp.length ? 0xFFu << v.transp.offset : 0);
        memcpy(p, &w, 4);
    }
}

/* cubic-bezier(x1, y1, x2, y2) of t (like CSS): solve x(s) = t by bisection, the answer is y(s) */
static double bez(double t, double x1, double y1, double x2, double y2)
{
    double lo = 0, hi = 1, s = t;
    int i;
    for (i = 0; i < 30; i++) {
        double x;
        s = (lo + hi) * 0.5;
        x = 3 * (1 - s) * (1 - s) * s * x1 + 3 * (1 - s) * s * s * x2 + s * s * s;
        if (x < t) lo = s; else hi = s;
    }
    return 3 * (1 - s) * (1 - s) * s * y1 + 3 * (1 - s) * s * s * y2 + s * s * s;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

/* log in RAM (/dev/huboot.log): when it started (since power-on), screen status, why it exited */
static void logf_(const char *m, int a, int b)
{
    FILE *f = fopen("/dev/huboot.log", "a");
    if (!f) return;
    fprintf(f, "%.2f %s %d %d\n", now(), m, a, b);
    fclose(f);
}

/* detach the console from the screen - like hu-run.sh (fbcon_off), otherwise the login prompt lands over the splash. /sys is mounted
* only by S02sysfs.sh (we are S01) - try on every frame until it works. 1 - done */
static int fbcon_off(void)
{
    char p[64], b[64] = "";
    int i, g, ok = 0;
    for (i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "/sys/class/vtconsole/vtcon%d/name", i);
        if ((g = open(p, O_RDONLY)) < 0) continue;
        if (read(g, b, sizeof b - 1) < 0) b[0] = 0;
        close(g);
        if (!strstr(b, "frame buffer")) continue;
        snprintf(p, sizeof p, "/sys/class/vtconsole/vtcon%d/bind", i);
        if ((g = open(p, O_WRONLY)) < 0) continue;
        ok = write(g, "0", 1) == 1;
        close(g);
    }
    if (ok && (g = open("/sys/class/graphics/fbcon/cursor_blink", O_WRONLY)) >= 0) { if (write(g, "0", 1) < 0) {} close(g); }
    return ok;
}

/* GPIO 87 is set as output (S08 tiggo: third after 56 and 91) */
static int gpio_ready(void)
{
    char b[8] = "";
    int g = open("/sys/class/gpio/gpio87/direction", O_RDONLY);
    if (g < 0) return 0;
    if (read(g, b, sizeof b - 1) < 0) b[0] = 0;
    close(g);
    return !strncmp(b, "out", 3);
}

int main(void)
{
    struct fb_fix_screeninfo f;
    struct sigaction sa;
    int fd = -1, i, x, y, n, lit = 0, opened = 0, con = 0, drawn = 0;
    double t0;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    logf_("start", getpid(), 0);
    setenv("HU_MCU_LOG", "/dev/huboot-mcu.log", 0);    /* /dev/hu does not exist yet */
    for (i = 0; i < 40 && (fd = open("/dev/fb0", O_RDWR)) < 0; i++) usleep(50000);   /* the kernel is still bringing up the screen */
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) { logf_("no fb0", i, fd); return 1; }
    bpp = (int)v.bits_per_pixel; stride = (int)f.line_length;
    logf_("fb0 xres/yres", (int)v.xres, (int)v.yres);
    logf_("fb0 bpp/yoffset", bpp, (int)v.yoffset);
    if ((bpp != 16 && bpp != 32) || v.xres < IMG_W || v.yres < IMG_H) { logf_("fb0 unsupported", bpp, 0); return 1; }
    fb = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { logf_("mmap failed", 0, 0); return 1; }
    logf_("unblank", ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK), 0);   /* the screen may be blanked early in boot */
    v.xoffset = v.yoffset = 0;                         /* first buffer on screen */
    logf_("pan", ioctl(fd, FBIOPAN_DISPLAY, &v), 0);
    /* design picture - inside the loop (drawn): once more after the console is detached */
    t0 = now();
    while (!quit && now() - t0 < 120) {
        double ph = fmod(now() - t0, 1.6) / 1.6;
        if (!mcu_ok() && !opened && (gpio_ready() || now() - t0 > 8)) {
            opened = 1;
            mcu_light_only = 1;                             /* full startup is hu's job (mcu.h) */
            logf_("mcu open", mcu_open(), gpio_ready());
        }
        mcu_poll(now());
        if (!con && fbcon_off()) { con = 1; drawn = 0; logf_("fbcon off", 0, 0); }   /* console text may have already landed - redraw */
        if (!drawn) {
            for (i = n = 0; i < IMG_RUNS; i++) {
                int k;
                for (k = 0; k < img_n[i]; k++, n++) put(n % IMG_W, n / IMG_W, img_c[i]);
            }
            drawn = 1;
        }
        if (!lit && mcu_ok()) { mcu_set_brightness(10); logf_("backlight", 10, 0); lit = 1; }   /* after MCU startup */
        int rx = -RW + (int)lround(bez(ph, 0.65, 0, 0.35, 1) * (LW + RW));
        for (x = 0; x < LW; x++) {
            const unsigned char *c = x >= rx && x < rx + RW ? RUN : TRACK;
            for (y = 0; y < 2; y++) put(LX + x, LY + y, c);
        }
        usleep(16000);
    }
    logf_(quit ? "exit: term" : "exit: timeout", 0, 0);
    mcu_close();
    munmap(fb, f.smem_len);
    close(fd);
    return 0;
}
