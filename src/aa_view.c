/* Android Auto in the shell: Apps tile -> session (aa.c); while the phone shows AA, its frame is full screen
 * (center 1024x600 of the 1280x720 frame, pixel for pixel), touches go to the phone, steering wheel buttons to AA. Owner: full
 * screen, launched from the tile. The phone gave the screen back (exit via HU_NAME in AA) - our interface again, the session lives on;
 * the tile returns to AA. Video: HU - VPU without copying, PC - software decoder (for testing). */
#include "ui_int.h"
#include "aa.h"
#include "aa_wifi.h"
#include "mcu.h"
#include "media.h"
#include "platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int full, waiting, vopen, wcall;                   /* wcall: the tile called the wireless phone (aa_wifi_call) */
static double t0, wcall_t;

/* microphone for the assistant: the MCU source and the fm1388 DSP are set by the main thread (A.aa_vs / A.aa_mic), ALSA capture (input
 * tef6635-ssi: microphone after the DSP) 16 kHz mono in chunks of 1024 samples -> to the phone. libasound - dlopen.
 * The SSI has one rate for capture and playback: not plughw:0,0 (direct capture broke the audio when AA speaks with the microphone
 * open - Gemini Live). Voice session - hands-free like a CarPlay call: DSP mode 1 (16 kHz, echo cancellation),
 * the stock asound.conf pair dsnoop_16000 / cpout_16000; otherwise - "default" (dsnoop_48000 / dmix_48000). */
#include <dlfcn.h>
#include <pthread.h>
static struct {
    int ok;
    int (*open)(void **, const char *, int, int);
    int (*set)(void *, int, int, unsigned, unsigned, int, unsigned);
    long (*readi)(void *, void *, unsigned long);
    int (*recover)(void *, int, int);
    int (*close)(void *);
} SA;
static volatile int mic_on;
static pthread_t mic_th;

static void *mic_thread(void *arg)
{
    void *h = NULL;
    int16_t b[1024];
    double acc = 0;
    long cnt = 0;
    (void)arg;
    usleep(200000);                                       /* the DSP is switched in the main thread */
    {   /* voice session: 16 kHz (hands-free) - if the SSI is free; busy at 48 kHz (something is playing) - like Siri */
        const char *dev = A.aa_vs ? "plug:dsnoop_16000" : "default";
        if (SA.open(&h, dev, 1, 0) < 0 || SA.set(h, 2, 3, 1, 16000, 1, 100000) < 0) {   /* S16_LE, RW_INTERLEAVED */
            if (h) { SA.close(h); h = NULL; }
            if (A.aa_vs) { dev = "default"; if (SA.open(&h, dev, 1, 0) < 0 || SA.set(h, 2, 3, 1, 16000, 1, 100000) < 0) { if (h) SA.close(h); h = NULL; } }
        }
        if (!h) { fprintf(stderr, "hu: aa: mic: cannot open capture\n"); return NULL; }
        if (dev[0] == 'p') A.aa_cm = 1;                       /* main thread: MCU 0x0A, DSP mode 1 - until the end of the session */
        fprintf(stderr, "hu: aa: mic on %s\n", dev);
        if (A.aa_cm) usleep(200000);                         /* the DSP is switched in the main thread */
    }
    while (mic_on) {
        long n = SA.readi(h, b, 1024), i;
        if (n < 0) { SA.recover(h, (int)n, 1); continue; }
        aa_mic_send(b, (size_t)n * 2);
        for (i = 0; i < n; i++) acc += (double)b[i] * b[i];
        if ((cnt += n) >= 16000) { fprintf(stderr, "hu: aa: mic level rms %.0f\n", sqrt(acc / cnt)); acc = 0; cnt = 0; }
    }
    SA.close(h);
    return NULL;
}

static void on_mic(int open)
{
    if (!SA.ok) {
        void *l = dlopen("libasound.so.2", RTLD_NOW);
        if (l) {
            *(void **)&SA.open = dlsym(l, "snd_pcm_open"); *(void **)&SA.set = dlsym(l, "snd_pcm_set_params");
            *(void **)&SA.readi = dlsym(l, "snd_pcm_readi"); *(void **)&SA.recover = dlsym(l, "snd_pcm_recover");
            *(void **)&SA.close = dlsym(l, "snd_pcm_close");
        }
        SA.ok = SA.open && SA.set && SA.readi && SA.recover && SA.close ? 1 : -1;
    }
    A.aa_mic = open;
    if (open && !mic_on && SA.ok > 0) { mic_on = 1; if (pthread_create(&mic_th, NULL, mic_thread, NULL)) mic_on = 0; else pthread_detach(mic_th); }
    else if (!open) mic_on = 0;
}

/* latency measurement (HU_AA_LAT=1): after a touch, for 3 s - log frame arrival and display (ms since the touch) */
static double t_touch = -1e9;
static int lat_on = -1;
static int lat(void) { if (lat_on < 0) lat_on = getenv("HU_AA_LAT") != NULL; return lat_on; }
static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void on_video(const uint8_t *p, size_t n, int64_t ts)
{
    double d = mono() - t_touch;
    if (d < 3) fprintf(stderr, "hu: aa: lat frame in +%.0f ms (%u B)\n", d * 1000, (unsigned)n);
    vstream_push(p, n, ts);
}
static void on_audio(int id, const uint8_t *p, size_t n) { astream_push(id, p, n); }
static void on_audio_ctl(int id, int on)
{
    static const int RATE[3] = { 48000, 16000, 16000 }, CH[3] = { 2, 1, 1 };
    int navi = id == 1 && on && A.radio_on && !A.aa_cm;      /* prompt while the radio plays - over the radio (MCU 0x20 mix-in) */
    int r = on ? (id && A.aa_cm ? astream_open_dev(id, RATE[id], CH[id], "plug:cpout_16000")   /* voice in the session - 16 kHz */
                : navi ? astream_open_dev(id, RATE[id], CH[id], "plug:navi_16000") : astream_open(id, RATE[id], CH[id])) : 0;
    if (!on) astream_close(id);
    /* order for the main thread (audio_policy): a prompt over the radio - aa_navi before aa_voice at the start and after it
     * at the end, otherwise for a moment "aa_voice without mix-in" - HU source and back, the tuner restarts (the radio dropped out) */
    if (id == 1 && navi) A.aa_navi = 1;
    if (id == 0) A.aa_media = on;                         /* AA music - the MCU "media" source (audio_policy) */
    else { static int v[3]; v[id] = on; A.aa_voice = v[1] || v[2]; }   /* navigation / assistant voice */
    if (id == 1 && !navi) A.aa_navi = 0;
    fprintf(stderr, "hu: aa: audio %d %s%s\n", id, on ? "start" : "stop", r < 0 ? " (no output!)" : "");
}

/* hands-free (A.aa_cm, set by the microphone thread) - until the end of the session: the assistant answers when the microphone is already closed -
 * also at the phone volume, like Gemini Live */
static void on_voice(int on) { A.aa_vs = on; if (!on) A.aa_cm = 0; fprintf(stderr, "hu: aa: voice session %s\n", on ? "start" : "end"); }

static int bg;                                             /* wireless session started in the background - do not take the screen */

void aa_view_open(double now)
{
    static const aa_sinks k = { on_video, on_audio, on_audio_ctl, on_mic, on_voice };
    bg = 0;                                               /* tile: the screen goes to the phone (and to a wireless one that has not started yet) */
    if (aa_state() == AA_NATIVE) { aa_focus(1); return; }   /* session exists - give the screen back to the phone */
    if (aa_state() != AA_IDLE) return;
    if (!aa_wifi_pending() && aa_wifi_phone()) { aa_wifi_call(); wcall = 1; wcall_t = now; return; }   /* call it - we will open when it arrives */
    if (!vopen && vstream_open() == 0) vopen = 1;
    aa_night(!A.light);
    if (aa_start(&k) == 0) { waiting = 1; t0 = now; }
}

int aa_view_covers(void) { return full; }

void aa_view_update(double now)
{
    int s = aa_state();
    aa_wifi_poll(now);
    /* phone arrived over Wi-Fi: the session is in the background, as in cars (Audi MMI): AA is connected, the screen shows the shell and the
     * "Android Auto · Ready" banner, the tile shows AA right away. This is the "car's" decision (phone setting "Start Android Auto
     * automatically" = Default (set by the vehicle), owner 2026-10-26); Always / If used on the last drive - the phone itself
     * asks for the screen (focus 1, aa.c) - AA opens without the tile. The phone starts video only with its own screen -
     * wait for it and give the screen to the shell (like HOME) */
    if (wcall && now - wcall_t > 20) wcall = 0;                /* called via the tile, the phone did not come */
    if (s == AA_IDLE && !waiting && aa_wifi_pending()) { int fg = wcall; wcall = 0; aa_view_open(now); bg = !fg; s = aa_state(); }   /* via the tile - AA screen right away */
    if (bg && s == AA_RUN) { aa_focus(0); s = aa_state(); bg = waiting = 0; hud_show(14, NULL, now); }   /* banner "Android Auto · Ready" */
    else if (bg && s == AA_IDLE && now - t0 > 0.3) bg = waiting = 0;   /* failed - quietly, without a USB hint */
    static int light = -1;
    if (s == AA_IDLE) A.aa_vs = A.aa_cm = 0;                   /* phone disconnected in the middle of the session */
    A.aa_conn = s == AA_RUN || s == AA_NATIVE;                 /* MCU navigation mix-in - for the whole time of the connection */
    if (s != AA_IDLE && A.light != light) { light = A.light; aa_night(!light); }   /* theme changed - AA night mode */
    if (s != AA_IDLE && mcu_ok() && mcu_st()->vspeed >= 0) aa_speed(mcu_st()->vspeed * 15625 / 1000);   /* 0.05625 km/h = 15.625 mm/s */
    if (s != AA_IDLE && mcu_ok()) aa_car(mcu_st()->steer_ok, mcu_st()->steer, mcu_st()->ill);   /* steering angle, headlights */
    if (s == AA_IDLE) light = -1;
    if (waiting && s == AA_RUN) { waiting = 0; full = 1; damage_full(); }
    else if (waiting && s == AA_IDLE && now - t0 > 0.3) {     /* no phone in the CarPlay port (or not Android) */
        waiting = 0;
        hud_show(13, "Connect an Android phone via USB", now);
    }
    if (full && s != AA_RUN) { full = 0; damage_full(); }      /* exit to the shell / phone disconnected */
    if (!full && s == AA_RUN && !waiting) { full = 1; damage_full(); }
    if (s == AA_IDLE && !waiting && vopen) { vstream_close(); vopen = 0; }
    if (full && vplayer_new_frame()) {
        double d = mono() - t_touch;
        if (d < 3) fprintf(stderr, "hu: aa: lat shown +%.0f ms\n", d * 1000);
        damage_full();
    }
}

void aa_view_draw(double now)
{
    float u1, v1;
    int w, h;
    gfx_tex *t = vplayer_frame(&u1, &v1, &w, &h);
    (void)now;
    rrect(0, 0, 1280, 750, 0, hexc(0));
    if (!t || w <= 0 || h <= 0) return;
    {   /* the phone draws in the center: margins 256x120 of the 1280x720 frame */
        float cu = 128.0f / w * u1, cv = 60.0f / h * v1;
        gfx_image(t, 0, 0, SCREEN_W, SCREEN_H, cu, cv, u1 - cu, v1 - cv, C_WHITE);
    }
}

/* touch (design coordinates 1280x750) -> screen pixels 1024x600 */
void aa_view_touch(int type, float x, float y)
{
    int sx = (int)(D(x) + 0.5f), sy = (int)(D(y) + 0.5f);
    sx = sx < 0 ? 0 : sx > SCREEN_W - 1 ? SCREEN_W - 1 : sx;
    sy = sy < 0 ? 0 : sy > SCREEN_H - 1 ? SCREEN_H - 1 : sy;
    aa_touch(type == PF_DOWN ? AA_TOUCH_DOWN : type == PF_UP ? AA_TOUCH_UP : AA_TOUCH_MOVE, sx, sy);
    if (type == PF_UP && lat()) { t_touch = mono(); fprintf(stderr, "hu: aa: lat touch up\n"); }
}

void aa_view_key(int d) { aa_key(d > 0 ? 0x57 : 0x58, 1); aa_key(d > 0 ? 0x57 : 0x58, 0); }   /* next/prev */

/* voice button on the steering wheel: Android Auto assistant (AA screen - if ours was shown, give it back to the phone); 0 - no session */
int aa_view_voice(void)
{
    int s = aa_state();
    if (s != AA_RUN && s != AA_NATIVE) return 0;
    if (s == AA_NATIVE) aa_focus(1);
    aa_key(0x54, 1); aa_key(0x54, 0);
    return 1;
}

void aa_view_phone(void) { aa_key(0x05, 1); aa_key(0x05, 0); }   /* phone button: CALL - AA dialer / answer */

void aa_view_exit(void) { aa_focus(0); }

void aa_view_resume(void) { if (aa_state() == AA_NATIVE) aa_focus(1); }   /* after reverse - back to AA if the session is alive */                  /* own screen: session lives, the tile returns to AA */

void aa_view_shutdown(void) { aa_stop(); }
