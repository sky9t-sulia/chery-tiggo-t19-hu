/* Android Auto: session (see aa.h, NOTES_aa.md). One thread reads USB and answers the phone; touches/buttons come from the
 * interface thread. All writes (and encryption) are under one mutex: TLS write order = frame order on the wire.
 * Frame: [channel][flags][length 2] ([total length 4] for FIRST) [data]; data = [id 2][protobuf/raw], encrypted as a whole. */
#include "version.h"
#include "aa.h"
#include "aa_cert.h"
#include "aa_pb.h"
#include "aa_tls.h"
#include "aa_usb.h"
#include "aa_wifi.h"
#include "text.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <linux/sockios.h>
#include <time.h>
#include <unistd.h>
#include "../third_party/stb_image.h"   /* cover art from AA; implementation is in gfx.c */

/* channels (chosen by the HU, as in openauto) */
enum { CH_CTRL = 0, CH_INPUT = 1, CH_SENSOR = 2, CH_VIDEO = 3, CH_MEDIA = 4, CH_SPEECH = 5, CH_SYSTEM = 6, CH_MIC = 7, CH_MSTAT = 8, NCH = 9 };
/* frame flags */
enum { F_FIRST = 1, F_LAST = 2, F_BULK = 3, F_CONTROL = 4, F_ENC = 8 };
#define MAX_FRAME 16384
/* video: 1280x720, margins 256x120 -> the phone UI 1024x600 in the center (= our screen, pixel for pixel) */
#define VW 1280
#define VH 720
#define MW 256
#define MH 120

static aa_sinks K;
static aa_usb U = { -1, 0, 0, 0, 0 };
static int tcp = -1;                                       /* wireless (aa_wifi): TCP socket instead of USB */
static pthread_t th;
static pthread_mutex_t txmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t wmu = PTHREAD_MUTEX_INITIALIZER;                  /* USB write order */
static volatile int st = AA_IDLE, quit, running, enc_on, night, speed_on, speed_mm = -1, drive_on, driving;
static volatile int steer_on, steer_v, steer_ok, light_on, light_v = -1;   /* steering angle (0.1 deg), headlights (-1 unknown) */
static int session[NCH], pings, pongs;
static struct { uint8_t *b; size_t n, cap; } rx[NCH];      /* message assembly from frames - per channel (the phone interleaves) */

static int64_t now_us(void) { struct timeval t; gettimeofday(&t, NULL); return (int64_t)t.tv_sec * 1000000 + t.tv_usec; }

/* what is playing on the phone (MediaPlaybackStatus channel, aasdk mediaplayback/ - .proto): 0x8001 state {1 state 1 stopped,
 * 2 playing, 3 paused; 3 seconds}, 0x8003 track {1 song, 2 artist, 3 album, 4 cover art (JPEG/PNG), 6 duration} */
static pthread_mutex_t npmu = PTHREAD_MUTEX_INITIALIZER;
static aa_np_t np;
static unsigned char *np_px;                               /* RGBA cover art (decoded here, the UI makes the texture) */
static int np_w, np_h;

static void pb_cpy(char *d, size_t dn, const uint8_t *p, size_t n) { if (n >= dn) n = dn - 1; memcpy(d, p, n); d[n] = 0; }

static void on_mstat(int id, const uint8_t *m, size_t n)
{
    pb_r r = { m, m + n };
    int f, wt;
    uint64_t v;
    const uint8_t *p;
    size_t len;
    if (id == 0x8001) {
        pthread_mutex_lock(&npmu);
        while (pb_next(&r, &f, &wt, &v, &p, &len) == 1) {
            if (f == 1 && wt == 0) np.state = (int)v;
            else if (f == 3 && wt == 0) { np.pos = (int)v; { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); np.pos_t = ts.tv_sec + ts.tv_nsec * 1e-9; } }   /* like pf_now */
        }
        np.valid = 1;
        pthread_mutex_unlock(&npmu);
    } else if (id == 0x8003) {
        const uint8_t *art = NULL;
        size_t art_n = 0;
        unsigned char *px = NULL;
        int w = 0, h = 0, c;
        aa_np_t t;
        pthread_mutex_lock(&npmu); t = np; pthread_mutex_unlock(&npmu);
        t.song[0] = t.artist[0] = t.album[0] = 0; t.dur = 0;
        while (pb_next(&r, &f, &wt, &v, &p, &len) == 1) {
            if (f == 1 && wt == 2) pb_cpy(t.song, sizeof t.song, p, len);
            else if (f == 2 && wt == 2) pb_cpy(t.artist, sizeof t.artist, p, len);
            else if (f == 3 && wt == 2) pb_cpy(t.album, sizeof t.album, p, len);
            else if (f == 4 && wt == 2) { art = p; art_n = len; }
            else if (f == 6 && wt == 0) t.dur = (int)v;
        }
        text_unescape(t.song); text_unescape(t.artist); text_unescape(t.album);
        if (art_n > 0 && art_n < 4 * 1024 * 1024 && (px = stbi_load_from_memory(art, (int)art_n, &w, &h, &c, 4)) && (w > 1024 || h > 1024)) {
            stbi_image_free(px); px = NULL;                        /* we do not take huge ones */
        }
        fprintf(stderr, "hu: aa: now playing \"%s\" - %s (%d s, art %dx%d)\n", t.song, t.artist, t.dur, px ? w : 0, px ? h : 0);
        pthread_mutex_lock(&npmu);
        t.valid = 1; t.seq = np.seq + 1; t.art_seq = t.seq;
        np = t;
        if (np_px) stbi_image_free(np_px);
        np_px = px; np_w = w; np_h = h;
        pthread_mutex_unlock(&npmu);
    }
}

int aa_np(aa_np_t *out)
{
    pthread_mutex_lock(&npmu);
    *out = np;
    pthread_mutex_unlock(&npmu);
    return out->valid && (st == AA_RUN || st == AA_NATIVE);   /* and when the screen belongs to the HU (dashboard, Music) - that is where it is needed; before,
                                                                * only AA_RUN - on our own screen the AA track and cover art were not
                                                                * shown (in the car 2026-10-26) */
}

unsigned char *aa_np_art(int *w, int *h)
{
    unsigned char *px;
    pthread_mutex_lock(&npmu);
    px = np_px; np_px = NULL; *w = np_w; *h = np_h;
    pthread_mutex_unlock(&npmu);
    return px;
}

/* transport: USB or TCP - same bytes. Read: bytes, 0 - timeout, -1 - link lost */
static int tr_read(void *b, size_t n, int ms)
{
    struct pollfd q = { tcp, POLLIN, 0 };
    ssize_t r;
    if (tcp < 0) return aa_usb_read(&U, b, n, ms);
    if ((r = poll(&q, 1, ms)) <= 0) return r < 0 && errno != EINTR ? -1 : 0;
    r = read(tcp, b, n);
    return r > 0 ? (int)r : r < 0 && (errno == EINTR || errno == EAGAIN) ? 0 : -1;
}

static int tr_write(const void *b, size_t n, int ms)
{
    const uint8_t *p = b;
    if (tcp < 0) return aa_usb_write(&U, b, n, ms);
    while (n) {
        struct pollfd q = { tcp, POLLOUT, 0 };
        ssize_t r;
        if (poll(&q, 1, ms) <= 0) return -1;
        r = write(tcp, p, n);
        if (r < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return (int)(p - (const uint8_t *)b);
}

/* ---------------------------------------------------------------- sending */

static int send_frame(int ch, int flg, const uint8_t *pl, size_t n, uint32_t total)
{
    uint8_t f[MAX_FRAME + 128];
    size_t h = 4;
    int c = (int)n, r;
    int64_t t;
    if (flg & F_ENC) {
        pthread_mutex_lock(&txmu);                                /* one SSL for reading and writing - only for encryption */
        c = aa_tls_encrypt(pl, n, f + 8, sizeof f - 8);
        pthread_mutex_unlock(&txmu);
        if (c < 0) return -1;
    } else memcpy(f + 8, pl, n);
    if ((flg & 3) == F_FIRST) h = 8;
    f[8 - h] = (uint8_t)ch; f[9 - h] = (uint8_t)flg; f[10 - h] = (uint8_t)(c >> 8); f[11 - h] = (uint8_t)c;
    if (h == 8) { f[4] = (uint8_t)(total >> 24); f[5] = (uint8_t)(total >> 16); f[6] = (uint8_t)(total >> 8); f[7] = (uint8_t)total; }
    t = now_us();
    r = tr_write(f + 8 - h, h + (size_t)c, 10000);
    if ((t = now_us() - t) > 50000) fprintf(stderr, "hu: aa: write ch %d %u B took %lld ms\n", ch, (unsigned)(h + (size_t)c), (long long)(t / 1000));
    return r < 0 ? -1 : 0;
}

/* message: id + body; over 16384 - in FIRST/MIDDLE/LAST pieces, each encrypted separately */
static int send_msg(int ch, int control, int enc, int id, const void *body, size_t n)
{
    uint8_t *m = malloc(n + 2);
    size_t total = n + 2, off = 0;
    int r = 0, base = (control ? F_CONTROL : 0) | (enc ? F_ENC : 0);
    if (!m) return -1;
    m[0] = (uint8_t)(id >> 8); m[1] = (uint8_t)id;
    if (n) memcpy(m + 2, body, n);
    pthread_mutex_lock(&wmu);                                     /* write order (TLS) - its own lock: waiting for USB does not hold it */
    if (total <= MAX_FRAME) r = send_frame(ch, base | F_BULK, m, total, 0);   /* receive (decryption under txmu) */
    else while (off < total && !r) {
        size_t k = total - off > MAX_FRAME ? MAX_FRAME : total - off;
        int t = off == 0 ? F_FIRST : off + k == total ? F_LAST : 0;
        r = send_frame(ch, base | t, m + off, k, (uint32_t)total);
        off += k;
    }
    pthread_mutex_unlock(&wmu);
    free(m);
    if (r) fprintf(stderr, "hu: aa: send ch %d id %04x failed\n", ch, id);
    return r;
}

static int send_pb(int ch, int id, pb_w *w) { return w->err ? -1 : send_msg(ch, 0, enc_on, id, w->b, w->n); }

/* ---------------------------------------------------------------- service list */

static void av_audio(pb_w *w, int ch, int type, int rate, int chans)
{
    pb_begin(w, 1);                                  /* ChannelDescriptor */
    pb_varint(w, 1, (uint64_t)ch);
    pb_begin(w, 3);                                  /* AVChannel */
    pb_varint(w, 1, 1);                              /* AUDIO / PCM */
    pb_varint(w, 2, (uint64_t)type);                 /* 1 navigation, 2 system, 3 music */
    pb_begin(w, 3); pb_varint(w, 1, (uint64_t)rate); pb_varint(w, 2, 16); pb_varint(w, 3, (uint64_t)chans); pb_end(w);
    pb_bool(w, 5, 1);
    pb_end(w);
    pb_end(w);
}

static void send_sdr(void)
{
    static uint8_t b[1024];
    static const uint8_t keys[] = { 0x05, 0x06, 0x54, 0x55, 0x57, 0x58, 0x7E, 0x7F };   /* call, hang up, voice, play/pause, next, prev, play, pause */
    pb_w w;
    pb_init(&w, b, sizeof b);
    pb_begin(&w, 1); pb_varint(&w, 1, CH_INPUT);                      /* touches + steering wheel buttons */
    pb_begin(&w, 4);
    pb_bytes(&w, 1, keys, sizeof keys);                               /* packed */
    pb_begin(&w, 2); pb_varint(&w, 1, VW - MW); pb_varint(&w, 2, VH - MH); pb_end(&w);   /* touches - in the UI area (1024x600) */
    pb_end(&w); pb_end(&w);
    pb_begin(&w, 1); pb_varint(&w, 1, CH_SENSOR);                     /* parked/driving, night */
    pb_begin(&w, 2);
    pb_begin(&w, 1); pb_varint(&w, 1, 13); pb_end(&w);
    pb_begin(&w, 1); pb_varint(&w, 1, 10); pb_end(&w);
    pb_begin(&w, 1); pb_varint(&w, 1, 3); pb_end(&w);                  /* car speed (MCU 0x57) - for navigation */
    pb_begin(&w, 1); pb_varint(&w, 1, 14); pb_end(&w);                 /* DEAD_RECKONING: steering angle (MCU 0x58) */
    pb_begin(&w, 1); pb_varint(&w, 1, 17); pb_end(&w);                 /* LIGHT: headlights (MCU 0x16) */
    pb_end(&w); pb_end(&w);
    pb_begin(&w, 1); pb_varint(&w, 1, CH_VIDEO);                      /* H.264 video */
    pb_begin(&w, 3);
    pb_varint(&w, 1, 3);
    pb_begin(&w, 4);
    pb_varint(&w, 1, 2);                             /* 1280x720 */
    pb_varint(&w, 2, getenv("HU_AA_30") ? 2 : 1);    /* 60 fps (per Google: 1 = 60, 2 = 30; verified - 30 gives timestamps 33.3 ms apart) */
    pb_varint(&w, 3, MW); pb_varint(&w, 4, MH);
    pb_varint(&w, 5, 140);                           /* dpi: phone UI scale */
    pb_end(&w);
    pb_bool(&w, 5, 1);
    pb_end(&w); pb_end(&w);
    av_audio(&w, CH_MEDIA, 3, 48000, 2);
    av_audio(&w, CH_SPEECH, 1, 16000, 1);
    av_audio(&w, CH_SYSTEM, 2, 16000, 1);
    pb_begin(&w, 1); pb_varint(&w, 1, CH_MIC);                        /* microphone 16k/1 - the phone does not start without it */
    pb_begin(&w, 5);
    pb_varint(&w, 1, 1);
    pb_begin(&w, 2); pb_varint(&w, 1, 16000); pb_varint(&w, 2, 16); pb_varint(&w, 3, 1); pb_end(&w);
    pb_end(&w); pb_end(&w);
    pb_begin(&w, 1); pb_varint(&w, 1, CH_MSTAT); pb_begin(&w, 9); pb_end(&w); pb_end(&w);   /* what is playing (MediaPlaybackStatusService) */
    pb_str(&w, 2, "Chery");                          /* car */
    pb_str(&w, 3, "Tiggo 4");
    pb_str(&w, 4, "2019");
    pb_str(&w, 5, "T19");
    pb_varint(&w, 6, 0);                             /* driver on the left (Google: LEFT = 0; verified - AA panel on the left) */
    pb_str(&w, 7, HU_NAME);                          /* HU */
    pb_str(&w, 8, HU_NAME);
    pb_str(&w, 9, "1");
    pb_str(&w, 10, "1.0");
    pb_bool(&w, 11, 0);
    pb_bool(&w, 12, 0);                              /* AA clock - show */
    send_pb(CH_CTRL, 0x0006, &w);
}

/* ---------------------------------------------------------------- receive */

static void ack(int ch)
{
    uint8_t b[24];
    pb_w w;
    pb_init(&w, b, sizeof b);
    pb_varint(&w, 1, (uint64_t)(int64_t)session[ch]); pb_varint(&w, 2, 1);
    send_pb(ch, 0x8004, &w);
}

static void reply1(int ch, int id, int field, int64_t v)    /* reply with one field */
{
    uint8_t b[24];
    pb_w w;
    pb_init(&w, b, sizeof b);
    pb_varint(&w, field, (uint64_t)v);
    send_pb(ch, id, &w);
}

static void video_focus(int projected, int unrequested)
{
    uint8_t b[16];
    pb_w w;
    pb_init(&w, b, sizeof b);
    pb_varint(&w, 1, projected ? 1 : 2); pb_bool(&w, 2, unrequested);
    send_pb(CH_VIDEO, 0x8008, &w);
}

static void sensor(int type, int v)                         /* SensorEventIndication: 13 driving_status / 10 night / 3 speed (mm/s) */
{
    uint8_t b[24];
    pb_w w;
    pb_init(&w, b, sizeof b);
    pb_begin(&w, type); pb_varint(&w, 1, (uint64_t)v); pb_end(&w);
    send_pb(CH_SENSOR, 0x8003, &w);
}

/* DeadReckoningData {1 steering_angle_e1} (0.1 deg), LightData {1 head_light_state: 1 off / 2 on} - aasdk
 * sensorsource/message */
static void sensor_steer(int e1)
{
    uint8_t b[24];
    pb_w w;
    pb_init(&w, b, sizeof b);
    pb_begin(&w, 14); pb_varint(&w, 1, (uint64_t)(int64_t)e1); pb_end(&w);
    send_pb(CH_SENSOR, 0x8003, &w);
}
static void sensor_light(int on)
{
    uint8_t b[24];
    pb_w w;
    pb_init(&w, b, sizeof b);
    pb_begin(&w, 17); pb_varint(&w, 1, on ? 2 : 1); pb_end(&w);
    send_pb(CH_SENSOR, 0x8003, &w);
}

static int64_t pb_field(const uint8_t *p, size_t n, int field)   /* first varint field or -1 */
{
    pb_r r = { p, p + n };
    int f, wt;
    uint64_t v;
    const uint8_t *q;
    size_t l;
    while (pb_next(&r, &f, &wt, &v, &q, &l) > 0) if (f == field && wt == 0) return (int64_t)v;
    return -1;
}

static void handshake_step(const uint8_t *in, size_t n)
{
    static uint8_t out[8192];
    size_t on = 0;
    int r = aa_tls_handshake(in, n, out, sizeof out, &on);
    if (on) send_msg(CH_CTRL, 0, 0, 0x0003, out, on);
    if (r < 0) { fprintf(stderr, "hu: aa: TLS handshake failed\n"); quit = 1; return; }
    if (r == 1) {
        static const uint8_t ok[] = { 0x08, 0x00 };
        send_msg(CH_CTRL, 0, 0, 0x0004, ok, 2);                   /* AUTH_COMPLETE - still in plaintext */
        enc_on = 1;
        fprintf(stderr, "hu: aa: TLS ok\n");
    }
}

/* phone service message - to the log: id, length, first bytes (finding out what Gemini waits for) */
static void log_msg(const char *what, int ch, int id, const uint8_t *p, size_t n)
{
    char h[3 * 24 + 1];
    size_t i;
    for (i = 0; i < n && i < 24; i++) sprintf(h + 3 * i, " %02x", p[i]);
    h[3 * i] = 0;
    fprintf(stderr, "hu: aa: %s ch %d id 0x%04x len %u:%s\n", what, ch, id, (unsigned)n, h);
}

static void on_control(int id, const uint8_t *p, size_t n)
{
    if (id != 0x000B && id != 0x000C) log_msg("ctrl", CH_CTRL, id, p, n);   /* except pings */
    switch (id) {
    case 0x0002:                                                  /* VERSION_RESPONSE: major, minor, status */
        if (n < 6 || (p[4] | p[5]) != 0) { fprintf(stderr, "hu: aa: version mismatch\n"); quit = 1; return; }
        fprintf(stderr, "hu: aa: phone protocol %d.%d\n", p[0] << 8 | p[1], p[2] << 8 | p[3]);
        if (aa_tls_init(AA_CERT, AA_KEY) < 0) { quit = 1; return; }
        handshake_step(NULL, 0);
        break;
    case 0x0003: handshake_step(p, n); break;
    case 0x0005: fprintf(stderr, "hu: aa: service discovery\n"); send_sdr(); break;
    case 0x000B: {                                                /* ping from the phone - reply with the same time */
        uint8_t b[24];
        pb_w w;
        int64_t t = pb_field(p, n, 1);
        pb_init(&w, b, sizeof b);
        if (t >= 0) pb_varint(&w, 1, (uint64_t)t);
        send_msg(CH_CTRL, 0, 0, 0x000C, w.b, w.n);
        break;
    }
    case 0x000C: pongs++; break;
    case 0x000D: reply1(CH_CTRL, 0x000E, 1, 2); break;            /* navigation focus - with the phone */
    case 0x000F: send_msg(CH_CTRL, 0, enc_on, 0x0010, NULL, 0); quit = 1; break;   /* phone is saying goodbye */
    case 0x0010: quit = 1; break;
    case 0x0011:                                                  /* VoiceSessionNotification: 1 start, 2 end */
        if (K.voice) K.voice(pb_field(p, n, 1) == 1);
        break;
    case 0x0012: {                                                /* audio focus: RELEASE -> LOSS, otherwise GAIN (openauto) */
        int64_t t = pb_field(p, n, 1);
        reply1(CH_CTRL, 0x0013, 1, t == 4 ? 3 : 1);
        break;
    }
    default: break;                                               /* voice, battery and the rest - not needed */
    }
}

static int audio_id(int ch) { return ch == CH_MEDIA ? 0 : ch == CH_SPEECH ? 1 : 2; }

static void on_channel(int ch, int flg, int id, const uint8_t *p, size_t n)
{
    if (ch != CH_VIDEO && id != 0x0000 && id != 0x0001 && id != 0x8004) log_msg("chan", ch, id, p, n);   /* except data and ack */
    if (id == 0x0007 && (flg & F_CONTROL)) {                      /* CHANNEL_OPEN_REQUEST */
        uint8_t ok[] = { 0x08, 0x00 };
        send_msg(ch, 1, enc_on, 0x0008, ok, 2);
        return;
    }
    if (ch == CH_MSTAT) { on_mstat(id, p, n); return; }
    if (ch == CH_VIDEO || ch == CH_MEDIA || ch == CH_SPEECH || ch == CH_SYSTEM) {
        switch (id) {
        case 0x8000: {                                            /* SETUP -> OK, max_unacked (video 2 - like openauto, 4 gave jerks; audio 1), configuration 0 */
            uint8_t b[] = { 0x08, 0x02, 0x10, 0x01, 0x18, 0x00 };
            if (ch == CH_VIDEO) b[3] = (uint8_t)(getenv("HU_AA_UNACK") ? atoi(getenv("HU_AA_UNACK")) : 2);
            send_msg(ch, 0, enc_on, 0x8003, b, sizeof b);
            if (ch == CH_VIDEO) video_focus(1, 0);                /* the phone does not start video without this */
            break;
        }
        case 0x8001:                                              /* START */
            session[ch] = (int)pb_field(p, n, 1);
            if (ch == CH_VIDEO) { st = AA_RUN; fprintf(stderr, "hu: aa: video start\n"); }
            else if (K.audio_ctl) K.audio_ctl(audio_id(ch), 1);
            break;
        case 0x8002:                                              /* STOP */
            if (ch != CH_VIDEO && K.audio_ctl) K.audio_ctl(audio_id(ch), 0);
            break;
        case 0x0000:                                              /* data with a timestamp (8 bytes) */
        case 0x0001: {                                            /* without a timestamp (for video - SPS/PPS) */
            int64_t ts = -1;
            if (id == 0) {
                int i;
                if (n < 8) break;
                for (ts = 0, i = 0; i < 8; i++) ts = ts << 8 | p[i];
                p += 8; n -= 8;
            }
            if (ch == CH_VIDEO) {
                static int64_t t0;
                static int cnt;
                if (K.video) K.video(p, n, ts);
                if (!t0) t0 = now_us();
                if (++cnt, now_us() - t0 >= 5000000) {           /* actual frame rate - to the log every 5 s */
                    fprintf(stderr, "hu: aa: video %.1f fps\n", cnt * 1e6 / (double)(now_us() - t0));
                    t0 = now_us(); cnt = 0;
                }
            }
            else if (K.audio) K.audio(audio_id(ch), p, n);
            ack(ch);
            break;
        }
        case 0x8007: {                                            /* phone asks for focus: 1 - AA, 2 - the HU's own screen */
            int64_t m = pb_field(p, n, 2);
            if (ch == CH_VIDEO) { st = m == 2 ? AA_NATIVE : AA_RUN; video_focus(m != 2, 0); fprintf(stderr, "hu: aa: phone asks video focus %d (1 AA, 2 HU screen)\n", (int)m); }
            break;
        }
        }
        return;
    }
    if (ch == CH_MIC) {
        if (id == 0x8000) { uint8_t b[] = { 0x08, 0x02, 0x10, 0x01, 0x18, 0x00 }; send_msg(ch, 0, enc_on, 0x8003, b, sizeof b); }
        else if (id == 0x8005) {                                  /* open/close the microphone: no audio yet - OK */
            uint8_t b[] = { 0x08, 0x00, 0x10, 0x00 };
            int open = pb_field(p, n, 1) == 1;
            fprintf(stderr, "hu: aa: mic %s\n", open ? "open" : "close");
            send_msg(ch, 0, enc_on, 0x8006, b, sizeof b);
            if (K.mic) K.mic(open);
        }
        return;
    }
    if (ch == CH_INPUT && id == 0x8002) { uint8_t ok[] = { 0x08, 0x00 }; send_msg(ch, 0, enc_on, 0x8003, ok, 2); return; }
    if (ch == CH_SENSOR && id == 0x8001) {                        /* SENSOR_START: OK and the value right away */
        int64_t t = pb_field(p, n, 1);
        uint8_t ok[] = { 0x08, 0x00 };
        send_msg(ch, 0, enc_on, 0x8002, ok, 2);
        if (t == 13) { drive_on = 1; sensor(13, driving ? 31 : 0); fprintf(stderr, "hu: aa: driving status %s\n", driving ? "moving" : "parked"); }   /* see aa_speed */
        else if (t == 10) sensor(10, night);
        else if (t == 14) { steer_on = 1; fprintf(stderr, "hu: aa: phone wants steering angle\n"); if (steer_ok) sensor_steer(steer_v); }
        else if (t == 17) { light_on = 1; fprintf(stderr, "hu: aa: phone wants lights\n"); if (light_v >= 0) sensor_light(light_v); }
        else if (t == 3) { speed_on = 1; fprintf(stderr, "hu: aa: phone wants car speed\n"); if (speed_mm >= 0) sensor(3, speed_mm); }
    }
}

static void dispatch(int ch, int flg, const uint8_t *m, size_t n)
{
    int id;
    if (n < 2) return;
    id = m[0] << 8 | m[1];
    if (ch == CH_CTRL) on_control(id, m + 2, n - 2);
    else if (ch < NCH) on_channel(ch, flg, id, m + 2, n - 2);
}

static int rx_add(int ch, const uint8_t *p, size_t n)
{
    if (rx[ch].n + n > rx[ch].cap) {
        size_t c = (rx[ch].n + n) * 2;
        uint8_t *b = realloc(rx[ch].b, c);
        if (!b) return -1;
        rx[ch].b = b; rx[ch].cap = c;
    }
    memcpy(rx[ch].b + rx[ch].n, p, n); rx[ch].n += n;
    return 0;
}

/* USB byte stream -> frames (frames are not aligned to USB transfers) */
static uint8_t sbuf[4 * MAX_FRAME];
static size_t sn;

static void parse(void)
{
    static uint8_t plain[MAX_FRAME + 64];
    size_t off = 0;
    while (sn - off >= 4) {
        uint8_t *h = sbuf + off;
        int ch = h[0], flg = h[1], type = flg & 3;
        size_t hl = type == F_FIRST ? 8 : 4, len = (size_t)(h[2] << 8 | h[3]);
        const uint8_t *pl;
        size_t pn;
        if (sn - off < hl + len) break;
        pl = h + hl;
        if (ch >= NCH) { off += hl + len; continue; }
        if (type == F_FIRST || type == F_BULK) rx[ch].n = 0;     /* start of a new message - the old fragment is not needed */
        if (flg & F_ENC) {
            int d;
            pthread_mutex_lock(&txmu);                            /* one SSL for reading and writing */
            d = aa_tls_decrypt(pl, len, plain, sizeof plain);
            pthread_mutex_unlock(&txmu);
            if (d < 0) { fprintf(stderr, "hu: aa: decrypt failed\n"); quit = 1; return; }
            pl = plain; pn = (size_t)d;
        } else pn = len;
        if (rx_add(ch, pl, pn) < 0) { quit = 1; return; }
        if (type == F_LAST || type == F_BULK) { dispatch(ch, flg, rx[ch].b, rx[ch].n); rx[ch].n = 0; }
        off += hl + len;
    }
    memmove(sbuf, sbuf + off, sn - off); sn -= off;
}

/* ---------------------------------------------------------------- thread */

/* Wireless AA: after the AP restarts, video runs 4-5 s and stalls (not a byte from the phone, then ping timeout;
 * in the car 2026-10-26). To understand where it gets stuck - a snapshot: our TCP socket (to the log) + dongle, AP, phone ping
 * ("wifi.sh diag" -> /dev/hu/wifi-diag.log, in the background). The "ok" snapshot is the reference while video runs; "stall" - the stream stalled. */
static void wifi_diag(const char *tag)
{
    struct tcp_info ti;
    socklen_t n = sizeof ti;
    int inq = -1, outq = -1;
    char c[96];
    if (tcp < 0) return;
    memset(&ti, 0, sizeof ti);
    getsockopt(tcp, IPPROTO_TCP, TCP_INFO, &ti, &n);
    ioctl(tcp, SIOCINQ, &inq); ioctl(tcp, SIOCOUTQ, &outq);
    fprintf(stderr, "hu: aa: diag %s: tcp state %u unacked %u retrans %u/%u lost %u rtt %u us rto %u ms cwnd %u "
            "last rx %u ms last tx %u ms last ack %u ms inq %d outq %d\n", tag, ti.tcpi_state, ti.tcpi_unacked,
            ti.tcpi_retransmits, ti.tcpi_total_retrans, ti.tcpi_lost, ti.tcpi_rtt, ti.tcpi_rto / 1000, ti.tcpi_snd_cwnd,
            ti.tcpi_last_data_recv, ti.tcpi_last_data_sent, ti.tcpi_last_ack_recv, inq, outq);
    snprintf(c, sizeof c, "sh /dev/wifi/wifi.sh diag %s > /dev/null 2>&1 &", tag);
    if (system(c)) { }
}

static void session_run(void)
{
    static const uint8_t ver[] = { 0x00, 0x01, 0x00, 0x01 };     /* 1.1 (like openauto) */
    int64_t ping_t = now_us();
    memset(session, 0xff, sizeof session); pings = pongs = 0; enc_on = 0; speed_on = 0; drive_on = 0; steer_on = light_on = 0; sn = 0;
    st = AA_CONNECT;
    send_msg(CH_CTRL, 0, 0, 0x0001, ver, sizeof ver);
    int64_t rx_t = 0, run_t = 0;
    int diag_ok = 0, stall = 0;
    while (!quit) {
        int r = tr_read(sbuf + sn, sizeof sbuf - sn, 100);
        if (r < 0) { fprintf(stderr, "hu: aa: read failed (phone gone?)\n"); break; }
        if (tcp >= 0 && aa_wifi_lost()) { fprintf(stderr, "hu: aa: wireless phone gone\n"); break; }   /* do not wait for the ping */
        if (r > 0) {
            int64_t t = now_us();
            if (st == AA_RUN && rx_t && t - rx_t > 200000) fprintf(stderr, "hu: aa: rx gap %lld ms\n", (long long)((t - rx_t) / 1000));
            if (stall) { fprintf(stderr, "hu: aa: rx back after %lld ms\n", (long long)((t - rx_t) / 1000)); wifi_diag("back"); stall = 0; }
            rx_t = t;
            sn += (size_t)r; parse();
            if ((t = now_us() - t) > 50000) fprintf(stderr, "hu: aa: parse took %lld ms\n", (long long)(t / 1000));
        }
        if (tcp >= 0 && st == AA_RUN) {                           /* wireless: snapshots (see wifi_diag) */
            int64_t t = now_us();
            if (!run_t) run_t = t;
            if (!diag_ok && t - run_t > 20000000) { diag_ok = 1; wifi_diag("ok"); }
            if (rx_t && !stall && t - rx_t > 2000000) { stall = 1; wifi_diag("stall"); }
            if (rx_t && stall == 1 && t - rx_t > 7000000) { stall = 2; wifi_diag("stall2"); }
        }
        if (enc_on && now_us() - ping_t > 5000000) {             /* ping every 5 s; 2 unanswered - link lost */
            uint8_t b[16];
            pb_w w;
            ping_t = now_us();
            if (pings - pongs > 1) { fprintf(stderr, "hu: aa: ping timeout\n"); break; }
            pb_init(&w, b, sizeof b); pb_varint(&w, 1, (uint64_t)ping_t);
            send_msg(CH_CTRL, 0, 0, 0x000B, w.b, w.n);
            pings++;
        }
    }
}

static void *thread(void *arg)
{
    int i, r = -1;
    (void)arg;
    st = AA_SEARCH;
    if ((tcp = aa_wifi_take()) >= 0) { r = 0; fprintf(stderr, "hu: aa: wireless (tcp)\n"); }   /* wireless - immediately */
    else for (i = 0; i < 40 && !quit; i++) {                         /* after AOA - wait for reconnection (up to 10 s) */
        if ((r = aa_usb_find(&U)) == 0) break;
        if (r < 0 && i == 0) break;                               /* no phone at all - immediately (HUD) */
        usleep(250000);
    }
    if (!r && !quit) session_run();
    if (tcp >= 0) { close(tcp); tcp = -1; aa_wifi_done(); } else aa_usb_close(&U);
    aa_tls_free();
    for (i = 0; i < NCH; i++) { free(rx[i].b); rx[i].b = NULL; rx[i].n = rx[i].cap = 0; }
    if (K.audio_ctl) for (i = 0; i < 3; i++) K.audio_ctl(i, 0);
    st = AA_IDLE; enc_on = 0; running = 0;
    pthread_mutex_lock(&npmu); np.valid = 0; if (np_px) stbi_image_free(np_px); np_px = NULL; pthread_mutex_unlock(&npmu);
    fprintf(stderr, "hu: aa: session end\n");
    return NULL;
}

int aa_start(const aa_sinks *s)
{
    if (running) return 0;
    K = *s; quit = 0; running = 1;
    if (pthread_create(&th, NULL, thread, NULL)) { running = 0; return -1; }
    pthread_detach(th);
    return 0;
}

void aa_stop(void)
{
    if (!running) return;
    if (enc_on) { uint8_t b[] = { 0x08, 0x01 }; send_msg(CH_CTRL, 0, 1, 0x000F, b, 2); }   /* bye-bye: QUIT */
    quit = 1;
}

int aa_state(void) { return st; }
int aa_wireless(void) { return tcp >= 0; }

void aa_touch(int action, int x, int y)
{
    uint8_t b[64];
    pb_w w;
    if (st != AA_RUN || !enc_on) return;
    pb_init(&w, b, sizeof b);
    pb_varint(&w, 1, (uint64_t)now_us() * 1000);            /* timestamp - nanoseconds, like openauto */
    pb_begin(&w, 3);
    pb_begin(&w, 1);
    pb_varint(&w, 1, (uint64_t)x); pb_varint(&w, 2, (uint64_t)y); pb_varint(&w, 3, 0);   /* = pixels of our screen */
    pb_end(&w);
    pb_varint(&w, 2, 0); pb_varint(&w, 3, (uint64_t)action);
    pb_end(&w);
    send_pb(CH_INPUT, 0x8001, &w);
}

void aa_key(int code, int down)
{
    uint8_t b[48];
    pb_w w;
    if ((st != AA_RUN && st != AA_NATIVE) || !enc_on) return;   /* and when the screen belongs to the HU: track buttons on the dashboard / Music
                                                                 * (before, only AA_RUN - did not switch, 2026-10-26) */
    pb_init(&w, b, sizeof b);
    pb_varint(&w, 1, (uint64_t)now_us());
    pb_begin(&w, 4);
    pb_begin(&w, 1); pb_varint(&w, 1, (uint64_t)code); pb_bool(&w, 2, down); pb_varint(&w, 3, 0); pb_bool(&w, 4, 0); pb_end(&w);
    pb_end(&w);
    send_pb(CH_INPUT, 0x8001, &w);
}

/* car speed for AA (SpeedData 1: m/s x 1000); sent on change, at most 10 times per second */
/* driving / standing (DRIVING_STATUS: 31 FULLY_RESTRICTED - no video, keyboards etc. while driving / 0 UNRESTRICTED) - by
 * MCU speed (we know neither the handbrake nor the selector): above 15 km/h - driving, below 15 km/h for 10 s in a row - standing (owner
 * 2026-10-27). No speed (the MCU did not send it) - standing */
static void drive_update(int mm_s)
{
    static int64_t slow_t;
    int64_t t = now_us();
    int d = driving;
    if (mm_s > 4167) { d = 1; slow_t = 0; }                  /* 15 km/h = 4167 mm/s */
    else if (!slow_t) slow_t = t;
    else if (t - slow_t >= 10000000) d = 0;
    if (d != driving) {
        driving = d;
        fprintf(stderr, "hu: aa: driving status %s (%d mm/s)\n", d ? "moving" : "parked", mm_s);
        if (enc_on && drive_on) sensor(13, d ? 31 : 0);
    }
}

void aa_speed(int mm_s)
{
    static int64_t t;
    static int sent = -1;                                    /* what was sent to the phone (a skipped one will be resent, a zero at the end matters) */
    speed_mm = mm_s;
    drive_update(mm_s);
    if (!enc_on || !speed_on) { sent = -1; return; }
    if (mm_s != sent && now_us() - t >= 100000) {
        static int64_t lt;
        t = now_us(); sent = mm_s; sensor(3, mm_s);
        if (t - lt >= 5000000) { lt = t; fprintf(stderr, "hu: aa: car speed %d mm/s\n", mm_s); }   /* to the log - every 5 s */
    }
}

/* steering angle (MCU 0x58: + left, by range ~+-550 - steering wheel degrees; NOT verified) and headlights; from the main loop. Angle - in 0.1
 * deg, left is minus (like PERF_STEERING_ANGLE in Android); at most 10 times per second */
void aa_car(int steer_valid, int steer_deg, int lights)
{
    static int64_t t;
    static int sent = 0x7fffffff;
    if (lights >= 0 && lights != light_v) { light_v = lights; if (enc_on && light_on) sensor_light(lights); }
    if (!steer_valid) return;
    steer_v = -steer_deg * 10; steer_ok = 1;
    if (!enc_on || !steer_on) { sent = 0x7fffffff; return; }
    if (steer_v != sent && now_us() - t >= 100000) { t = now_us(); sent = steer_v; sensor_steer(steer_v); }
}

void aa_night(int on)
{
    night = on;
    if (enc_on) sensor(10, on);
}

void aa_mic_send(const void *pcm, size_t n)
{
    uint8_t m[8 + 4096];
    int64_t t = now_us();
    int i;
    if (!enc_on || n > 4096) return;
    for (i = 0; i < 8; i++) m[i] = (uint8_t)(t >> (56 - 8 * i));   /* timestamp, us (BE) */
    memcpy(m + 8, pcm, n);
    send_msg(CH_MIC, 0, 1, 0x0000, m, 8 + n);
}

void aa_audio_loss(void)
{
    uint8_t b[] = { 0x08, 0x03, 0x10, 0x01 };                  /* AudioFocusNotification: LOSS, unsolicited */
    if (enc_on) send_msg(CH_CTRL, 0, 1, 0x0013, b, sizeof b);
}

void aa_focus(int projected)
{
    if (!enc_on) return;
    video_focus(projected, 1);
    st = projected ? AA_RUN : AA_NATIVE;
}
