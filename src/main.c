/* hu - our own HU shell renderer on OpenGL ES 2.
 * Main loop: events -> ui_frame (draws only what changed) -> present the frame.
 * If nothing changes, no frame is drawn and the process sleeps. Animations run on the real clock.
 * Resources: the HU_ASSETS directory (default ./assets). Exit: SIGINT/SIGTERM or closing the window (PC). */
#include "gfx.h"
#include "mcu.h"
#include "platform.h"
#include "theme.h"
#include "gl.h"
#include "ui.h"
#include "version.h"

#include <dirent.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

/* the version string stays in the binary (not removed by strip): strings hu | grep hu-build */
const char hu_build_id[] __attribute__((used)) = HU_BUILD_ID;

/* write the version to HU_DIR/version (SSH: cat /dev/hu/version); on the PC it just won't be written - harmless */
static void write_version(void)
{
    const char *as = getenv("HU_ASSETS");
    char path[512];
    FILE *f;
    if (as && strstr(as, "/assets")) {
        size_t n = (size_t)(strstr(as, "/assets") - as);
        if (n < sizeof path - 16) { memcpy(path, as, n); snprintf(path + n, sizeof path - n, "/version"); }
        else return;
    } else snprintf(path, sizeof path, "/dev/hu/version");
    if ((f = fopen(path, "w"))) { fprintf(f, "%s\n", HU_BUILD_ID); fclose(f); }
}

/* HU_SHOT=file.ppm: save a frame ~1.5 s after start and exit (look check without a human) */
static void shot(const char *path)
{
    static unsigned char px[SCREEN_W * SCREEN_H * 4];
    FILE *f = fopen(path, "wb");
    int y, x;
    if (!f) return;
    glReadPixels(0, 0, SCREEN_W, SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (y = SCREEN_H - 1; y >= 0; y--)
        for (x = 0; x < SCREEN_W; x++) fwrite(px + (y * SCREEN_W + x) * 4, 1, 3, f);
    fclose(f);
}
static void on_signal(int s) { (void)s; running = 0; }
/* crash (SIGSEGV, abort on a corrupted heap, etc.): stack and memory map go to the log, then proceed as if we weren't there (hu-run
 * will restart). hu addresses: via build/arm/hu.debug (addr2line), libraries: via /proc/self/maps. 2026-10-26: it crashed once with
 * "free(): invalid next size" after the car was switched on - impossible to find without a stack */
static void on_crash(int s)
{
    void *bt[48];
    char b[256];
    int n = backtrace(bt, 48), f;
    ssize_t r;
    snprintf(b, sizeof b, "hu: crash: signal %d, stack (%d):\n", s, n);
    if (write(2, b, strlen(b)) < 0) { }
    backtrace_symbols_fd(bt, n, 2);
    if ((f = open("/proc/self/maps", O_RDONLY)) >= 0) {
        while ((r = read(f, b, sizeof b)) > 0) if (write(2, b, (size_t)r) < 0) break;
        close(f);
    }
    signal(s, SIG_DFL);
    raise(s);
}
/* console (terminal on screen, USB keyboard): SIGUSR1 - hand the screen to the kernel console, SIGUSR2 - take it back. hu keeps
 * running (MCU "I am alive", sound), but does not draw or accept touches */
static volatile sig_atomic_t tty_req;
static void on_tty(int s) { tty_req = s == SIGUSR1 ? 1 : 2; }

/* leaving the console without a keyboard - Home button (MCU): SIGHUP to everything on the 2nd console (tty2 = 4:2), the shell exits,
 * console.sh returns the screen */
static void console_hangup(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        char p[300], b[512], *q;
        FILE *f;
        int tty = 0;
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        snprintf(p, sizeof p, "/proc/%s/stat", e->d_name);
        if (!(f = fopen(p, "r"))) continue;
        if (fgets(b, sizeof b, f) && (q = strrchr(b, ')')) && sscanf(q + 2, "%*c %*d %*d %*d %d", &tty) == 1 && tty == (4 << 8 | 2))
            kill(atoi(e->d_name), SIGHUP);
        fclose(f);
    }
    closedir(d);
}

int main(void)
{
    const char *dir = getenv("HU_ASSETS") ? getenv("HU_ASSETS") : "assets";
    double fps_t;
    int frames = 0, idle = 0, rc = 1;
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_tty;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    {   /* crashes go to the log with a stack; backtrace once in advance: the first call loads libgcc_s (malloc), which in the handler
         * would fail on a corrupted heap */
        void *bt[2];
        static const int sig[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
        unsigned i;
        backtrace(bt, 2);
        sa.sa_handler = on_crash;
        sa.sa_flags = SA_RESETHAND;
        for (i = 0; i < sizeof sig / sizeof *sig; i++) sigaction(sig[i], &sa, NULL);
        sa.sa_flags = 0;
    }

    if (pf_init(SCREEN_W, SCREEN_H)) return 1;
    if (gfx_init(SCREEN_W, SCREEN_H)) goto out_pf;
    if (ui_init(dir)) goto out_gfx;
    fprintf(stderr, "hu: running (%s)\n", HU_BUILD_ID);
    write_version();
    /* the boot screen (huboot) is run by the MCU itself (backlight, "I am alive") - close it and wait for it to exit before our own mcu_open:
     * one writer at a time on the MCU port. Up to 1 s */
    if (system("killall huboot > /dev/null 2>&1") == 0) {
        int i;
        for (i = 0; i < 20 && system("pidof huboot > /dev/null 2>&1") == 0; i++) usleep(50000);
    }
    if (mcu_open() == 0) ui_mcu_ready();          /* hardware backlight etc. - via MCU */
    fps_t = pf_now();
    {
    const char *shot_path = getenv("HU_SHOT");
    double t0 = fps_t;
    int tty = 0, full = 0;                     /* full - after the console: draw both buffers in full */
    while (running) {
        double now = pf_now();
        pf_event ev;
        int age;
        mcu_poll(now);
        if (tty_req) {
            if (tty_req == 1 && !tty) { glFinish(); pf_console(); tty = 1; fprintf(stderr, "hu: console - screen to tty\n"); }
            else if (tty_req == 2 && tty) { tty = 0; full = 3; fprintf(stderr, "hu: console - screen back\n"); }
            tty_req = 0;
        }
        if (tty) {
            mcu_state *m = mcu_st();
            int i;
            while (pf_poll(&ev)) if (ev.type == PF_QUIT) running = 0;
            for (i = 0; i < m->nkeys; i++) if (m->keys[i] == MCU_KEY_HOME) { console_hangup(); fprintf(stderr, "hu: console - Home, closing\n"); }
            m->nkeys = 0;                      /* key presses under the console are not needed by the shell */
            usleep(20000);
            continue;
        }
        while (pf_poll(&ev)) {
            if (ev.type == PF_QUIT) running = 0;
            else ui_event(&ev, now);
        }
        {   /* for testing: HU_TAP="x,y;x,y" - taps 0.4 s after start, then every 0.6 s */
            static const char *tap;
            static double next;
            float x, y;
            if (!next) { tap = getenv("HU_TAP"); next = t0 + 0.4; }
            if (tap && *tap && now >= next && sscanf(tap, "%f,%f", &x, &y) == 2) {
                pf_event te;
                te.x = x; te.y = y;
                te.type = PF_DOWN; ui_event(&te, now);
                te.type = PF_UP; ui_event(&te, now + 0.05);
                tap = strchr(tap, ';'); if (tap) tap++;
                next = now + 0.6;
            }
        }
        age = full ? 0 : pf_buffer_age();      /* 0 - unknown: ui draws the whole frame */
        if (full) full--;
        {
            static int last_age = -1;
            if (age != last_age) { fprintf(stderr, "hu: buffer age %d\n", age); last_age = age; }
        }
        if (shot_path && now - t0 > (getenv("HU_SHOT_T") ? atof(getenv("HU_SHOT_T")) : 1.5)) {
            ui_frame(now, 0);                  /* full frame */
            shot(shot_path);
            break;
        }
        {
            /* profile: cpu - frame build (submission to the GPU), swap - GPU/vsync wait, max - worst interval */
            static double t_cpu, t_swap, max_dt, last_frame;
            double a = pf_now(), b;
            if (ui_frame(now, age)) {
                b = pf_now(); t_cpu += b - a;
                pf_swap(); t_swap += pf_now() - b; frames++;
                if (last_frame > 0 && b - last_frame > max_dt) max_dt = b - last_frame;
                last_frame = b;
            } else { usleep(ui_video_active() ? 2000 : 8000); idle++; last_frame = 0; }   /* nothing changed - sleep, the GPU rests */

            if (now - fps_t >= 2.0) {
                long nd, nc;
                double px;
                gfx_stats(&nd, &nc, &px);
                fprintf(stderr, "hu: fps %.1f (idle %d%%) scr %d  cpu %.1f ms  swap %.1f ms  max %.0f ms  draws/frame %ld (culled %ld)  fill %.2f Mpx/frame\n",
                        frames / (now - fps_t), frames + idle ? idle * 100 / (frames + idle) : 0, ui_screen(),
                        frames ? t_cpu * 1000 / frames : 0, frames ? t_swap * 1000 / frames : 0, max_dt * 1000,
                        frames ? nd / frames : 0, frames ? nc / frames : 0, frames ? px / frames / 1e6 : 0);
                frames = idle = 0; fps_t = now; t_cpu = t_swap = max_dt = 0;
            }
        }
    }
    }
    rc = 0;
    fprintf(stderr, "hu: exit\n");
    ui_shutdown();
    mcu_close();
out_gfx:
    gfx_shutdown();
out_pf:
    pf_shutdown();
    return rc;
}
