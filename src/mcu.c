/* MCU link over UART, see mcu.h. The protocol was captured by intercepting ShincoService traffic (tools/uarthook). */
#include "mcu.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define PORT "/dev/ttymxc2"                 /* HU_MCU_PORT - another port (PC: tools/fake_mcu.py) */

static int fd = -1;
static unsigned char seq;
static double last_alive;
static unsigned char rx[512];
static int nrx;
static mcu_state st = { .vspeed = -1, .power = -1, .lock_fb = -1, .auto_light_c = -1, .drl_c = -1, .spd_alarm_c = -1, .bl = -1, .ill = -1 };
static double t_open;                                      /* second part of the start - 2 s after opening */
static int stage;                                          /* 1 - port opened, 2 - waiting 2 s, 0 - start finished */
int mcu_light_only;                                        /* boot screen: without start (see mcu.h) */
static void send_cmd(unsigned char cmd, const unsigned char *data, int len);
static unsigned char vol_tail[3] = { 0x07, 0x0C, 0x00 };   /* bytes 3..5 of command 0x25 - as in the MCU state */

mcu_state *mcu_st(void) { return &st; }

/* frame from the MCU: cmd, data */
static void on_frame(unsigned char cmd, const unsigned char *d, int len)
{
    static int got12;
    if (cmd == 0x12) {                                         /* start snapshot: the MCU repeats it until we reply 0x12 */
        send_cmd(0x12, NULL, 0);
        if (got12) return;                                     /* we apply only the first one - after that the live state is 0x16 */
        got12 = 1;
    }
    if ((cmd == 0x16 || cmd == 0x12) && len >= 23) {          /* audio state */
        st.media = d[2]; st.phone = d[3];
        vol_tail[0] = d[5]; vol_tail[1] = d[6]; vol_tail[2] = d[7];
        st.bass = d[10] - 10; st.treble = d[11] - 10; st.mute = d[12] != 0; st.middle = d[14] - 10;
        st.speed = (d[15] >> 6) & 3; st.def = d[22];
        st.region = (d[21] >> 2) & 3;                          /* radio region, codes as in 0x36 */
        st.ill = (d[17] >> 2) & 1;                             /* headlights on (verified in the car: 0x84 / 0x80) */
        st.audio_new = st.region_new = 1;
    } else if (cmd == 0x30 && len >= 5) {                      /* tuner state */
        st.am = d[0] == 0x02; st.freq = d[1] << 8 | d[2];
        st.preset = d[3] & 0x80 ? d[3] & 0x7F : 0; st.idx = d[3] & 0x7F;
        st.seeking = (d[4] & 0x60) == 0x40; st.scanning = (d[4] & 0x20) != 0;
        st.tuner_new = 1;
    } else if ((cmd == 0x33 || cmd == 0x34) && len >= 1 && (d[0] == 1 || d[0] == 2)) {   /* band lists */
        int b = d[0] - 1, i, n = 0;
        if (cmd == 0x34) {                                     /* [band, 20 x f] */
            for (i = 0; i < 20 && 1 + i * 2 + 1 < len; i++) { int f = d[1 + i * 2] << 8 | d[2 + i * 2]; if (f) st.fav[b][n++] = f; }
            st.nfav[b] = n;
        } else {                                               /* [band, current f, 40 x f, ...] */
            for (i = 0; i < 40 && 3 + i * 2 + 1 < len; i++) { int f = d[3 + i * 2] << 8 | d[4 + i * 2]; if (f) st.found[b][n++] = f; }
            st.nfound[b] = n;
        }
        st.lists_new = 1;
    } else if (cmd == 0x53 && len >= 4 && d[0] < 24 && d[1] < 60 && d[3] < 60) {   /* car clock (no date) + cluster backlight */
        st.clk_h = d[0]; st.clk_m = d[1]; st.clk_s = d[3]; st.clk_new = 1;
        st.bl = d[2] & 0x0f; st.veh_new = 1;
    } else if (cmd == 0x09 && len >= 13 && !st.fs_ok) {      /* fast save [01, 12 bytes] - as 0x09 gave it */
        memcpy(st.fs, d + 1, 12); st.fs_ok = 1;
    } else if (cmd == 0x57 && len >= 2) {                      /* car speed (NOTES_mcu_messages.md) */
        st.vspeed = d[0] | d[1] << 8;
    } else if (cmd == 0x58 && len >= 2) {                      /* steering angle */
        st.steer = ((d[0] << 8 | d[1]) >> 4) - 2048; st.steer_ok = 1;
    } else if (cmd == 0x02 && len >= 1) {                      /* knob (power icon): [01] screen off, [00] on; */
        unsigned char v = d[0];                                /* the stock one replies the same (backlight and sound are handled by the MCU) */
        send_cmd(0x02, &v, 1);
        st.user_off = v != 0; st.user_off_new = 1;
    } else if (cmd == 0x13 && len >= 2 && d[1] == 0x40) {      /* volume knob: [0x80 +- clicks, 40] */
        st.knob += (int)d[0] - 0x80;
    } else if (cmd == 0x13 && len >= 2) {                      /* steering wheel button: [01, code] press, [00, code] release */
        if (st.nkeys < 8) st.keys[st.nkeys++] = d[1] | (d[0] == 1 ? 0 : MCU_KEY_UP);
    } else if (cmd == 0x51 && len >= 6) {                      /* climate: NOTES_mcu.md */
        memcpy(st.clim, d, 8 < len ? 8 : (size_t)len);
        st.clim_on = !(d[3] & 0x02); st.fan = d[2] & 7; st.ac = (d[2] & 0x08) != 0;
        st.air = (d[3] >> 4) & 7; st.temp = d[4] >> 3; st.recirc = !(d[5] & 0x04);   /* 0x04 - fresh air (in the car) */
        st.clim_new = 1;
    } else if (cmd == 0x50 && len >= 1) {                      /* power, door, vehicle settings (arrives twice) */
        st.power = d[0] & 3; st.door = (d[0] >> 3) & 1;
        st.lock_fb = ((d[0] >> 5) & 1) + 2 * ((d[0] >> 6) & 1);   /* lock confirmation (ShellApp BCM4 raw[0]) */
        if (len >= 2) { st.auto_light_c = d[1] & 1; st.drl_c = (d[1] >> 1) & 1; }   /* auto light, DRL (raw[1]) */
        st.veh_new = 1;
    } else if (cmd == 0x52 && len >= 2) {                      /* ICM2: speeding warning (raw[1]) */
        st.spd_alarm_c = d[1] != 0; st.spd_idx_c = d[1]; st.veh_new = 1;
    } else if (cmd == 0x04 && len >= 1) {                      /* power (in the car 2026-10-23): 02 - all on, 01 - the MCU turned off the screen
                                                               * and sound (ignition off / door opened), 00 - will cut power */
        unsigned char v = d[0];
        send_cmd(0x04, &v, 1);                                 /* the stock one replies the same */
        st.shutdown = v == 0;
        st.off = v != 2;
    }
}

int mcu_ok(void) { return fd >= 0; }


/* exchange log: /dev/hu/mcu.log (HU RAM), format as in tools/uarthook - parsed by tools/mcu_watch.py */
static FILE *logf;
static void xlog(char dir, const unsigned char *b, int n)
{
    int i;
    struct timespec ts;
    if (!logf) return;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(logf, "%ld.%03ld 0 mxc2 %c", (long)ts.tv_sec, ts.tv_nsec / 1000000, dir);
    for (i = 0; i < n; i++) fprintf(logf, " %02x", b[i]);
    fputc('\n', logf);
    fflush(logf);
}

static void put(const unsigned char *b, int n)
{
    xlog('W', b, n);
    if (fd >= 0 && write(fd, b, (size_t)n) != n) fprintf(stderr, "hu: mcu write: %s\n", strerror(errno));
}

static void send_cmd(unsigned char cmd, const unsigned char *data, int len)
{
    unsigned char f[64];
    unsigned sum;
    int i, n = 0;
    if (fd < 0 || len > 48) return;
    f[n++] = 0x01; f[n++] = 0x55; f[n++] = 0xAA;
    f[n++] = seq; f[n++] = cmd; f[n++] = 0x00; f[n++] = (unsigned char)len;
    for (i = 0; i < len; i++) f[n++] = data[i];
    for (sum = 0, i = 3; i < n; i++) sum += f[i];
    f[n++] = (unsigned char)sum;
    seq = (unsigned char)((seq + 1) & 0x7F);
    put(f, n);
}

int mcu_open(void)
{
    struct termios t;
    const char *port = getenv("HU_MCU_PORT") ? getenv("HU_MCU_PORT") : PORT;
    fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { fprintf(stderr, "hu: mcu: no %s (%s)\n", port, strerror(errno)); return -1; }
    memset(&t, 0, sizeof t);
    t.c_cflag = CS8 | PARENB | PARODD | CLOCAL | CREAD;
    t.c_iflag = 0; t.c_oflag = 0; t.c_lflag = 0;
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    cfsetispeed(&t, B115200); cfsetospeed(&t, B115200);
    if (tcsetattr(fd, TCSANOW, &t)) {
        fprintf(stderr, "hu: mcu: tcsetattr: %s\n", strerror(errno));
        close(fd); fd = -1; return -1;
    }
    tcflush(fd, TCIOFLUSH);
    logf = fopen(getenv("HU_MCU_LOG") ? getenv("HU_MCU_LOG") : "/dev/hu/mcu.log", "a");
    fprintf(stderr, "hu: mcu: %s open\n", port);
    /* start as in ShincoService (recorded in the car, NOTES_mcu.md): without 0xFC / 0x08 / 0xF1 / 0x0E the MCU does not send
     * car events (steering wheel buttons, doors, power). 0x00 - the MCU replies with state 0x12; 0xF1 [01] and 0x0E - after 2 s */
    if (mcu_light_only) return 0;                       /* boot screen: only the "I'm alive" keep-alive and brightness (mcu_poll) */
    send_cmd(0x00, NULL, 0);
    send_cmd(0xFC, NULL, 0);
    { static const unsigned char z = 0x00; send_cmd(0x08, &z, 1); }
    send_cmd(0x33, NULL, 0);
    stage = 1;
    return 0;
}

void mcu_close(void)
{
    if (logf) fclose(logf);
    logf = NULL;
    if (fd >= 0) close(fd);
    fd = -1;
}

/* parsing incoming: frames 01 AA 55 seq cmd 00 len data sum -> we acknowledge; 06 AA 55 seq 00 - acknowledgements to us */
static void parse(void)
{
    int i = 0;
    while (i < nrx) {
        if (rx[i] == 0x06 && i + 5 <= nrx && rx[i + 1] == 0xAA && rx[i + 2] == 0x55) { i += 5; continue; }
        if (rx[i] == 0x01 && i + 3 <= nrx && rx[i + 1] == 0xAA && rx[i + 2] == 0x55) {
            int len, total;
            if (i + 7 > nrx) break;                        /* waiting for the header */
            len = rx[i + 6];
            total = 7 + len + 1;
            if (i + total > nrx) break;                    /* waiting for the whole frame */
            {
                unsigned char ack[5] = { 0x06, 0x55, 0xAA, rx[i + 3], 0x00 };
                put(ack, 5);
            }
            on_frame(rx[i + 4], rx + i + 7, len);
            i += total;
            continue;
        }
        if ((rx[i] == 0x01 || rx[i] == 0x06) && i + 3 > nrx) break;   /* start of a frame at the boundary of reads */
        i++;                                               /* garbage / out of sync */
    }
    if (i > 0) { memmove(rx, rx + i, (size_t)(nrx - i)); nrx -= i; }
}

void mcu_poll(double now)
{
    ssize_t r;
    if (fd < 0) return;
    while ((r = read(fd, rx + nrx, sizeof rx - (size_t)nrx)) > 0) {
        xlog('R', rx + nrx, (int)r);
        nrx += (int)r;
        parse();
        if (nrx >= (int)sizeof rx) nrx = 0;               /* overflow - reset */
    }
    if (stage == 1) { t_open = now; stage = 2; }
    if (stage == 2 && now - t_open >= 2.0) {               /* second part of the start */
        static const unsigned char one = 0x01;
        send_cmd(0xF1, &one, 1);
        send_cmd(0x0E, NULL, 0);
        stage = 0;
    }
    if (now - last_alive >= 5.0) {                         /* the "I'm alive" keep-alive as in ShincoService */
        static const unsigned char one = 0x01;
        last_alive = now;
        send_cmd(0x01, &one, 1);
    }
}

void mcu_set_brightness(int v)
{
    unsigned char d[2];
    if (v < 1) v = 1;
    if (v > 10) v = 10;
    d[0] = 0x04; d[1] = (unsigned char)v;
    send_cmd(0x11, d, 2);
}

/* ---------------------------------------------------------------- sound */
void mcu_set_volumes(int media, int phone)
{
    unsigned char d[5] = { (unsigned char)media, (unsigned char)phone, vol_tail[0], vol_tail[1], vol_tail[2] };
    send_cmd(0x25, d, 5);
}

void mcu_set_default_volume(int v) { unsigned char d = (unsigned char)v; send_cmd(0x14, &d, 1); }

void mcu_set_eq(int bass, int treble, int middle, int mute)
{
    unsigned char d[4] = { (unsigned char)(bass + 10), (unsigned char)(treble + 10), (unsigned char)(mute ? 2 : 0), (unsigned char)(middle + 10) };
    send_cmd(0x21, d, 4);
}

void mcu_set_speed_volume(int n) { unsigned char d[2] = { (unsigned char)n, 0 }; send_cmd(0x23, d, 2); }
void mcu_beep(void) { static const unsigned char d[2] = { 0x02, 0x00 }; send_cmd(0x24, d, 2); }

/* ---------------------------------------------------------------- radio */
void mcu_source(int src) { unsigned char d[2] = { 0x00, (unsigned char)src }; send_cmd(0x15, d, 2); }

/* fast save (like ShellApp FastSaving): 0x09 [12 bytes], the MCU keeps it during sleep and power loss and
 * returns it at start (0x09 [01, 12 bytes]) - track position without writing to NAND */
void mcu_fastsave(const unsigned char d[12]) { send_cmd(0x09, d, 12); }

/* navigation mix-in over a source (NOTES_navi_mix.md): 0x20 [on, mix-in volume 0..10, type 0x21 CarPlay (with 0x20 -
 * stock navigation - the MCU did not duck the source, verified in the car), source ducking: 0x14 (like CarPlay
 * onDuckAudio) / 0x64 no ducking / off 0xFF] - like ShellApp AudioSetting::setMixVolumeInfo; the audio goes to the
 * sound-navi card (navi_16000). Like CarPlay: on for the whole connection time, on a prompt - only ducking */
void mcu_navi_mix(int on, int vol, int duck)
{
    unsigned char d[4] = { (unsigned char)(on ? 1 : 0), (unsigned char)(vol < 0 ? 0 : vol > 10 ? 10 : vol), 0x21,
                           (unsigned char)(!on ? 0xFF : duck ? 0x14 : 0x64) };
    send_cmd(0x20, d, 4);
}
void mcu_vehicle(int item, int value)                         /* 0x50 [item, value, FF FF FF] - like ShellApp RRM1 */
{
    unsigned char d[5] = { (unsigned char)item, (unsigned char)value, 0xFF, 0xFF, 0xFF };
    send_cmd(0x50, d, 5);
}
/* 0x53 (RRM3): b0 h / b1 min / b3 sec (0xFF - do not change), b2 backlight 1..10, b5/b6 date (day=0 - do not change), b7=0 */
static int bl_clamp(int v) { return v < 1 ? 1 : v > 10 ? 10 : v; }
void mcu_cluster_backlight(int level)
{
    unsigned char d[8] = { 0xFF, 0xFF, (unsigned char)bl_clamp(level), 0xFF, 0, 0, 0, 0 };
    send_cmd(0x53, d, 8);
}
void mcu_cluster_time(int h, int m, int bl)
{
    /* like the stock setTime + initRRM3Info: b0 h, b1 min, b2 backlight, b3 sec(0), b5/b6 current month+year (day=0).
     * Month/year are taken from the system clock - the cluster has no date, but a time frame without a valid month is discarded. */
    time_t t = time(NULL);
    struct tm tm;
    int mon, yr;
    unsigned char d[8];
    localtime_r(&t, &tm);
    mon = tm.tm_mon + 1; yr = tm.tm_year - 100;
    if (yr < 0) yr = 0; else if (yr > 127) yr = 127;
    d[0] = (unsigned char)h; d[1] = (unsigned char)m; d[2] = (unsigned char)bl_clamp(bl); d[3] = 0; d[4] = 0;
    d[5] = (unsigned char)((mon >> 1) << 5);                   /* day=0, month>>1 */
    d[6] = (unsigned char)((mon & 1) | (yr << 1));             /* month&1, year-2000 */
    d[7] = 0;
    send_cmd(0x53, d, 8);
}
void mcu_scan(void) { static const unsigned char d = 0x02; send_cmd(0x30, &d, 1); }
void mcu_pick_found(int n) { unsigned char d = (unsigned char)(0x10 + n); send_cmd(0x30, &d, 1); }
void mcu_pick_fav(int n) { unsigned char d = (unsigned char)(0x40 + n); send_cmd(0x30, &d, 1); }
void mcu_fav_toggle(void) { static const unsigned char d = 0x0C; send_cmd(0x30, &d, 1); }
void mcu_set_region(int code) { unsigned char d = (unsigned char)code; send_cmd(0x36, &d, 1); send_cmd(0x33, NULL, 0); }
void mcu_fav_remove(int am, int n) { unsigned char d[2] = { (unsigned char)(am ? 2 : 1), (unsigned char)(0x80 + n) }; send_cmd(0x38, d, 2); }
void mcu_seek(int up) { unsigned char d = up ? 0x05 : 0x06; send_cmd(0x30, &d, 1); }
void mcu_band(int am) { unsigned char d = am ? 0x08 : 0x07; send_cmd(0x30, &d, 1); }
void mcu_tune(int am, int f)
{
    unsigned char d[2] = { (unsigned char)(f >> 8), (unsigned char)f };
    (void)am;
    send_cmd(0x32, d, 2);
}

/* ---------------------------------------------------------------- climate */
static int f_index(int i)                                  /* °F index as in ACControler::initParam: °F - 63 */
{
    if (i >= 30) return 0x1E;
    return (int)((17.5f + 0.5f * i) * 1.8f + 32 + 0.5f) - 63;
}

static void climate_send(int cmd, int level)
{
    int l = st.clim[4] >> 3, r = st.clim[5] >> 3;
    unsigned char d[7];
    if (level >= 0) { l += level - st.temp; if (l < 0) l = 0; }   /* like electricTempAdd/Sub: index together with the level */
    d[0] = (unsigned char)cmd; d[1] = (unsigned char)l; d[2] = (unsigned char)r;
    d[3] = (unsigned char)f_index(l); d[4] = (unsigned char)f_index(r);
    d[5] = (unsigned char)(level >= 0 ? level : st.temp); d[6] = 0;
    send_cmd(0x5B, d, 7);
}

void mcu_climate(int cmd) { climate_send(cmd, -1); }
void mcu_climate_mode(int air) { static const unsigned char M[5] = { 0x02, 0x03, 0x04, 0x05, 0x01 }; if (air >= 0 && air < 5) climate_send(M[air], -1); }
void mcu_climate_temp(int level) { climate_send(MCU_CLIM_TEMP, level); }

void mcu_camera(int on) { unsigned char d = (unsigned char)(on ? 1 : 0); send_cmd(0x18, &d, 1); }
void mcu_user_off(int on) { unsigned char d = (unsigned char)(on ? 1 : 0); send_cmd(0x03, &d, 1); }
