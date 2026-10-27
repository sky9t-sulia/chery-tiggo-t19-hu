/* Bluetooth on PC: no module - the interface shows "Bluetooth is off".
 * HU_FAKE_BT=1 - a fake connected phone for checking the UI: paired + connected, AVRCP music with metadata
 * (play / pause / next / prev work), battery and signal, a few contacts. */
#include "bt.h"
#include "ui_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fake;
static int trk;
static double tnow;                          /* time of the last bt_poll */
static const struct { const char *t, *ar, *al; int len; } TRK[] = {
    { "Frozen", "Madonna", "Ray of Light", 372 },
    { "The Sound of Silence", "Simon & Garfunkel", "Sounds of Silence", 185 },
    { "Bohemian Rhapsody", "Queen", "A Night at the Opera", 354 },
};
#define NTRK (int)(sizeof TRK / sizeof TRK[0])

static void fake_track(int i, double now)
{
    trk = (i + NTRK) % NTRK;
    snprintf(A.bt_title, sizeof A.bt_title, "%s", TRK[trk].t);
    snprintf(A.bt_artist, sizeof A.bt_artist, "%s", TRK[trk].ar);
    snprintf(A.bt_album, sizeof A.bt_album, "%s", TRK[trk].al);
    A.bt_len = TRK[trk].len; A.bt_pos = 0; A.bt_pos_t = now; A.bt_seq++;
}

int bt_init(void)
{
    static const char *names[] = { "Anna", "Dad", "Office" }, *nums[] = { "+380501112233", "+380672223344", "+380443334455" };
    int i;
    if (!getenv("HU_FAKE_BT")) return -1;
    fake = 1;
    A.bt_on = 1;
    snprintf(A.paired[0].id, sizeof A.paired[0].id, "AA:BB:CC:DD:EE:01");
    snprintf(A.paired[0].name, sizeof A.paired[0].name, "Galaxy S24 Ultra");
    A.paired[0].kind = 0; A.n_paired = 1;
    snprintf(A.bt_conn, sizeof A.bt_conn, "%s", A.paired[0].id);
    A.ph_batt = 4; A.ph_sig = 3;
    A.bt_av = 1; A.bt_stream = 1; A.bt_playing = 1;
    fake_track(0, 0);
    if ((PB = calloc(3, sizeof *PB))) {
        for (i = 0; i < 3; i++) {
            snprintf(PB[i].name, sizeof PB[i].name, "%s", names[i]);
            snprintf(PB[i].num[0], sizeof PB[i].num[0], "%s", nums[i]);
            PB[i].kind[0] = i == 2 ? 'w' : 'm'; PB[i].nnum = 1;
        }
        A.n_cont = 3; A.pb_state = 2;
    }
    fprintf(stderr, "hu: bt: fake phone (HU_FAKE_BT)\n");
    return 0;
}

void bt_poll(double now)
{
    tnow = now;
    if (!fake || !A.bt_playing) return;
    if (A.bt_pos_t <= 0) A.bt_pos_t = now;
    if (A.bt_pos + (now - A.bt_pos_t) >= A.bt_len) fake_track(trk + 1, now);   /* track ended - next one */
}

void bt_power(int on) { if (fake) A.bt_on = on; }
void bt_discoverable(int on) { (void)on; }
void bt_search(int on) { (void)on; }
void bt_pair(const char *id) { (void)id; }
void bt_connect(const char *id) { if (fake) snprintf(A.bt_conn, sizeof A.bt_conn, "%s", id); }
void bt_disconnect(const char *id) { (void)id; if (fake) { A.bt_conn[0] = 0; A.bt_av = A.bt_playing = A.bt_stream = 0; } }
void bt_remove(const char *id) { (void)id; }

void bt_media(int op)
{
    if (!fake) return;
    A.bt_pos += A.bt_playing && A.bt_pos_t > 0 ? tnow - A.bt_pos_t : 0;   /* freeze the position at the current point */
    A.bt_pos_t = tnow;
    switch (op) {
    case 1: A.bt_playing = 1; break;
    case 0: case 2: A.bt_playing = 0; break;
    case 3: fake_track(trk + 1, tnow); break;
    case 4: fake_track(trk - 1, tnow); break;
    }
}

void bt_media_mode(int shuffle, int repeat) { (void)shuffle; (void)repeat; }
void bt_dial(const char *number) { (void)number; }
void bt_answer(void) {}
void bt_reject(void) {}
void bt_hangup(void) {}
void bt_mic_mute(int on) { (void)on; }
void bt_audio_car(int car) { (void)car; }
void bt_a2dp_mute(int on) { (void)on; }
void bt_dtmf(char c) { (void)c; }
int  bt_linked(const unsigned char addr[6]) { (void)addr; return 0; }
int  bt_rfcomm_listen(const unsigned char uuid[16], const char *name) { (void)uuid; (void)name; return -1; }
