/* Bluetooth on the HU: the stock libbluelink stack (nFore, bluedroid, CSR module on ttymxc4). API and call order -
 * NOTES_bt.md (verified with hu/tools/bttest.c). ShellApp must be stopped: the module is owned by a single process.
 * Callbacks arrive in the library's threads: we only copy data into a queue; everything else is in bt_poll (main loop).
 * The library keeps pairings in /mnt/data1/bluetooth (NAND - allowed, as with the stock one). */
#define _GNU_SOURCE                               /* dladdr */
#include "text.h"
#include "version.h"
#include "bt.h"
#include "ui_int.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

typedef struct { uint8_t b[6]; } BT_ADDR;
typedef struct { const char *name; BT_ADDR addr; uint8_t pad[2]; } DEV_INFO;

extern int nfmCmdSetCallback(void *h, void *x);
extern int nfmCmdStartup(void **h, void *p);
extern int nfmCmdEnd(void **h);
extern int nfmCmdSetPower(void *h, int p);
extern int nfmCmdSetAutoConnectionEnable(void *h, int on);
extern int nfmCmdSetLocalDeviceName(void *h, const char *name);
extern int nfmCmdSetScanEnable(void *h, int on);
extern int nfmCmdInquiryStart(void *h);
extern int nfmCmdInquiryStop(void *h);
extern int nfmCmdGetPairedList(void *h);
extern int nfmCmdPairRequest(void *h, const uint8_t a[6]);
extern int nfmCmdAnswerPair(void *h, const uint8_t a[6], uint8_t len, const char *pin);
extern int nfmCmdRefusePair(void *h, const uint8_t a[6]);
extern int nfmCmdDeletePairDevice(void *h, const uint8_t a[6]);
extern int nfmCmdConnectRemoteDevice(void *h, const uint8_t a[6], uint32_t prof);
extern int nfmCmdDisconnectRemoteDevice(void *h, const uint8_t a[6], uint32_t prof);
extern int nfmCmdSetCbk_PowerStatus(void *h, int (*f)(void *, int));
extern int nfmCmdSetCbk_GetPairedList(void *h, int (*f)(void *, const DEV_INFO *, int));
extern int nfmCmdSetCbk_InquiryStatus(void *h, int (*f)(void *, int, DEV_INFO));
extern int nfmCmdSetCbk_ConnectionStatus(void *h, int (*f)(void *, BT_ADDR, uint32_t, int));
extern int nfmCmdSetCbk_RemotePairRequest(void *h, int (*f)(void *, int, int, BT_ADDR, int));
extern int nfmCmdSetCbk_HandsfreeBattStatusChanged(void *h, int (*f)(void *, BT_ADDR, int));
extern int nfmCmdSetCbk_HandsfreeSignalStrength(void *h, int (*f)(void *, BT_ADDR, int));
typedef struct { const char *name; uint16_t charset, pad; const char *value; } NOW_ATTR;
extern int nfmCmdGetCapability(void *h, const uint8_t a[6]);
extern int nfmCmdRegisterEventWatcher(void *h, const uint8_t a[6], uint8_t ev, int x);
extern int nfmCmdGetNowPlayingAttributes(void *h, const uint8_t a[6]);
extern int nfmCmdGetPlayStatus(void *h, const uint8_t a[6]);
extern int nfmCmdAVRCPControlCommand(void *h, const uint8_t a[6], int op);
extern int nfmCmdSetCurrentPlayerAttributeValues(void *h, const uint8_t a[6], const uint8_t *attrs);
extern int nfmCmdSetCbk_AVRCPGetCapability(void *h, int (*f)(void *, BT_ADDR, uint8_t, const uint8_t *));
extern int nfmCmdSetCbk_AVRCPNowPlaying(void *h, int (*f)(void *, BT_ADDR, uint8_t, const NOW_ATTR *));
extern int nfmCmdSetCbk_AVRCPPlayStatus(void *h, int (*f)(void *, BT_ADDR, const char *, const char *, int));
extern int nfmCmdSetCbk_Event_TrackChanged(void *h, int (*f)(void *, BT_ADDR, uint64_t));
extern int nfmCmdSetCbk_Event_PlaybackStatusChanged(void *h, int (*f)(void *, BT_ADDR, int));
extern int nfmCmdSetCbk_Event_PlaybackPosChanged(void *h, int (*f)(void *, BT_ADDR, const char *));
extern int nfmCmdSetCbk_HandsfreeRing(void *h, int (*f)(void *, BT_ADDR));
extern int nfmCmdSetCbk_HandsfreeCurrentCall(void *h, int (*f)(void *, BT_ADDR, const char *, uint8_t));
extern int nfmCmdSetCbk_HandsfreeOutgoingCall(void *h, int (*f)(void *, BT_ADDR, const char *, uint8_t));
extern int nfmCmdSetCbk_HandsfreeCallActive(void *h, int (*f)(void *, BT_ADDR, const char *, uint8_t));
extern int nfmCmdSetCbk_HandsfreeCallTerminated(void *h, int (*f)(void *, BT_ADDR, const char *, uint8_t));
extern int nfmCmdSetCbk_HandsfreeScoStatus(void *h, int (*f)(void *, BT_ADDR, int));
extern int nfmCmdSetCbk_A2DPStreamStatus(void *h, int (*f)(void *, BT_ADDR, int));   /* like a2dp_streamstatus(void*, BT_ADDR, bool) in demo_UI */
extern int nfmCmdSetCbk_PhonebookDownloadStatus(void *h, int (*f)(void *, BT_ADDR, int));
extern int nfmCmdDialCall(void *h, const uint8_t a[6], const char *number);
extern int nfmCmdAnswerCall(void *h, const uint8_t a[6]);
extern int nfmCmdRejectCall(void *h, const uint8_t a[6]);
extern int nfmCmdTerminateCall(void *h, const uint8_t a[6]);
extern int nfmCmdSendDTMF(void *h, const uint8_t a[6], const char *dtmf);
extern int nfmCmdAudioTransfer(void *h, const uint8_t a[6], int x);
extern int nfmCmdA2dpMute(void *h, const uint8_t a[6], int mute);   /* address is not used (libbluelink disassembly) */
extern int nfmCmdMicrophoneMute(void *h, const uint8_t a[6], int mute);
typedef struct { int storage; int type; } PB_TARGET;
extern int nfmCmdDownloadNoPhotoPhonebookStart(void *h, const uint8_t a[6], PB_TARGET t, uint32_t max);

enum { E_POWER, E_PAIRED, E_FOUND, E_SEARCH, E_CONN, E_PAIRREQ, E_BATT, E_SIGNAL, E_CAP, E_TRACK, E_PSTAT, E_PBS, E_POS, E_TRKCHG, E_CALL, E_SCO, E_PBDL, E_STREAM };
typedef struct { int type, a, b; uint32_t prof; uint8_t addr[6]; char name[64]; char t[160], ar[96], al[96]; } bt_ev;
typedef struct { uint8_t addr[6]; char name[64]; } bt_dev;

#define QN 128
static bt_ev q[QN];
static int qh, qt;
static bt_dev plist[16];                      /* list of paired devices from the callback */
static int plist_n;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void *H;
static int ready;                             /* stack is running */
static struct { uint8_t addr[6]; int prof, conn, seq; } links[8];   /* seq - connection order */
static int link_seq;
static uint8_t want_addr[6];                  /* connect when the others disconnect */
static double want_t;
static uint8_t av_addr[6];                    /* phone with AVRCP */
static uint8_t hf_addr[6];                    /* phone with HFP (calls, contacts) */
static double pb_t;
static int pb_cap, rec_cap;                   /* capacity of PB / REC (growing) */                           /* contact loading: last event (3 s of silence - done) */
static int pb_step;                           /* 0 none, 1 phonebook, 2 call log */
static double kind_t;                         /* when to re-read the types of paired devices (the file is not written immediately) */   /* profiles (2 HFP, 4 A2DP, 8 AVRCP) per device */

static void push(const bt_ev *e)
{
    pthread_mutex_lock(&mu);
    if ((qt + 1) % QN != qh) { q[qt] = *e; qt = (qt + 1) % QN; }
    pthread_mutex_unlock(&mu);
}
static void ev_addr(bt_ev *e, int type, const uint8_t *a) { memset(e, 0, sizeof *e); e->type = type; if (a) memcpy(e->addr, a, 6); }

/* ---- callbacks (library threads) */
static int cb_power(void *c, int p) { bt_ev e; (void)c; ev_addr(&e, E_POWER, NULL); e.a = p; push(&e); return 1; }
static int cb_paired(void *c, const DEV_INFO *l, int n)
{
    bt_ev e;
    int i;
    (void)c;
    pthread_mutex_lock(&mu);
    for (plist_n = 0, i = 0; i < n && i < 16; i++, plist_n++) {
        memcpy(plist[i].addr, l[i].addr.b, 6);
        snprintf(plist[i].name, sizeof plist[i].name, "%s", l[i].name ? l[i].name : "");
    }
    pthread_mutex_unlock(&mu);
    ev_addr(&e, E_PAIRED, NULL); push(&e);
    return 1;
}
static int cb_inquiry(void *c, int active, DEV_INFO d)
{
    bt_ev e;
    (void)c;
    if (d.name) { ev_addr(&e, E_FOUND, d.addr.b); snprintf(e.name, sizeof e.name, "%s", d.name); }
    else { ev_addr(&e, E_SEARCH, NULL); e.a = active; }
    push(&e);
    return 1;
}
static int cb_conn(void *c, BT_ADDR a, uint32_t prof, int st) { bt_ev e; (void)c; ev_addr(&e, E_CONN, a.b); e.prof = prof; e.a = st; push(&e); return 1; }
static int cb_pair(void *c, int pk, int st, BT_ADDR a, int ok) { bt_ev e; (void)c; ev_addr(&e, E_PAIRREQ, a.b); e.a = st; e.b = ok; (void)pk; push(&e); return 1; }
static int cb_batt(void *c, BT_ADDR a, int l) { bt_ev e; (void)c; ev_addr(&e, E_BATT, a.b); e.a = l; push(&e); return 1; }
static int cb_sig(void *c, BT_ADDR a, int l) { bt_ev e; (void)c; ev_addr(&e, E_SIGNAL, a.b); e.a = l; push(&e); return 1; }

/* "m:ss" / "h:mm:ss" -> seconds */
static int secs_of(const char *s)
{
    int v = 0, x = 0;
    if (!s) return 0;
    for (; *s; s++) { if (*s >= '0' && *s <= '9') x = x * 10 + *s - '0'; else if (*s == ':') { v = v * 60 + x; x = 0; } }
    return v * 60 + x;
}
static int cb_cap(void *c, BT_ADDR a, uint8_t n, const uint8_t *ev) { bt_ev e; (void)c; (void)n; (void)ev; ev_addr(&e, E_CAP, a.b); push(&e); return 1; }
static int cb_now(void *c, BT_ADDR a, uint8_t n, const NOW_ATTR *at)
{
    bt_ev e;
    int i;
    (void)c;
    ev_addr(&e, E_TRACK, a.b);
    for (i = 0; i < n; i++) {
        const char *k = at[i].name ? at[i].name : "", *v = at[i].value ? at[i].value : "";
        if (!strcmp(k, "Title")) snprintf(e.t, sizeof e.t, "%s", v);
        else if (!strcmp(k, "Artist")) snprintf(e.ar, sizeof e.ar, "%s", v);
        else if (!strcmp(k, "Album")) snprintf(e.al, sizeof e.al, "%s", v);
        else if (!strcmp(k, "Play Time")) e.b = secs_of(v);
    }
    text_unescape(e.t); text_unescape(e.ar); text_unescape(e.al);
    push(&e);
    return 1;
}
static int cb_pstat(void *c, BT_ADDR a, const char *len, const char *pos, int st)
{ bt_ev e; (void)c; ev_addr(&e, E_PSTAT, a.b); e.b = secs_of(len); e.prof = (uint32_t)secs_of(pos); e.a = st; push(&e); return 1; }
static int cb_trkchg(void *c, BT_ADDR a, uint64_t uid) { bt_ev e; (void)c; (void)uid; ev_addr(&e, E_TRKCHG, a.b); push(&e); return 1; }
static int cb_pbs(void *c, BT_ADDR a, int st) { bt_ev e; (void)c; ev_addr(&e, E_PBS, a.b); e.a = st; push(&e); return 1; }
static int cb_pos(void *c, BT_ADDR a, const char *pos) { bt_ev e; (void)c; ev_addr(&e, E_POS, a.b); e.a = secs_of(pos); push(&e); return 1; }

/* calls (HFP): e.a - 1 incoming ringing, 2 outgoing, 3 in call, 0 ended */
static void call_ev(BT_ADDR a, const char *num, int st) { bt_ev e; ev_addr(&e, E_CALL, a.b); e.a = st; if (num) snprintf(e.name, sizeof e.name, "%s", num); push(&e); }
static int cb_ring(void *c, BT_ADDR a) { (void)c; call_ev(a, NULL, 1); return 1; }
static int cb_curcall(void *c, BT_ADDR a, const char *n, uint8_t l) { (void)c; (void)l; call_ev(a, n, 1); return 1; }
static int cb_outcall(void *c, BT_ADDR a, const char *n, uint8_t l) { (void)c; (void)l; call_ev(a, n, 2); return 1; }
static int cb_active(void *c, BT_ADDR a, const char *n, uint8_t l) { (void)c; (void)l; call_ev(a, n, 3); return 1; }
static int cb_ended(void *c, BT_ADDR a, const char *n, uint8_t l) { (void)c; (void)l; call_ev(a, n, 0); return 1; }
static int cb_sco(void *c, BT_ADDR a, int up) { bt_ev e; (void)c; ev_addr(&e, E_SCO, a.b); e.a = up; push(&e); return 1; }
static int cb_stream(void *c, BT_ADDR a, int on) { bt_ev e; (void)c; ev_addr(&e, E_STREAM, a.b); e.a = on & 0xFF; push(&e); return 1; }   /* bool - low byte */
static int cb_pbdl(void *c, BT_ADDR a, int st) { bt_ev e; (void)c; ev_addr(&e, E_PBDL, a.b); e.a = st; push(&e); return 1; }

/* ---- addresses <-> id */
static void id_of(char *id, const uint8_t *a) { snprintf(id, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]); }
static int addr_of(uint8_t *a, const char *id)
{
    unsigned v[6];
    int i;
    if (sscanf(id, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -1;
    for (i = 0; i < 6; i++) a[i] = (uint8_t)v[i];
    return 0;
}

int bt_linked(const unsigned char addr[6])
{
    int i;
    for (i = 0; i < 8; i++) if ((links[i].conn || links[i].prof) && !memcmp(links[i].addr, addr, 6)) return 1;
    return 0;
}

/* RFCOMM with its own UUID: the nFore API (nfmCmdSetRfcommUUID / SPP) is a stub, the build has no SPP plugin. The bluedroid socket
 * layer (btif_sock, AOSP) works - it is in the library but not exported: btif_sock_get_interface at offset
 * 0x66d1c (Thumb), with a byte check of this libbluelink build (NOTES_bt.md "RFCOMM / custom UUID"). listen: type 1 -
 * RFCOMM, channel 0 - any, flags 0 (the phone is paired - the link is encrypted anyway) */
typedef struct {
    size_t size;
    int (*listen)(int type, const char *name, const uint8_t *uuid, int channel, int *fd, int flags);
    int (*connect)(const uint8_t *bd, int type, const uint8_t *uuid, int channel, int *fd, int flags);
} btsock_if;
int bt_rfcomm_listen(const unsigned char uuid[16], const char *name)
{
    static const uint8_t GUARD[6] = { 0x01, 0x48, 0x78, 0x44, 0x70, 0x47 };   /* ldr r0,[pc,#4]; add r0,pc; bx lr */
    Dl_info di;
    const uint8_t *b;
    const btsock_if *si;
    int fd = -1, r;
    if (!ready || !A.bt_on) return -1;
    void *sym = dlsym(RTLD_DEFAULT, "nfmCmdStartup");          /* not &nfmCmdStartup: in hu (non-PIE) that is its own PLT */
    if (!sym || !dladdr(sym, &di) || !di.dli_fbase) { fprintf(stderr, "hu: bt: rfcomm: no lib base\n"); return -1; }
    b = di.dli_fbase;
    if (memcmp(b + 0x66d1c, GUARD, 6)) { fprintf(stderr, "hu: bt: rfcomm: libbluelink differs - no btif_sock\n"); return -1; }
    si = ((const btsock_if *(*)(void))(b + 0x66d1d))();
    if (!si || si->size != sizeof *si) { fprintf(stderr, "hu: bt: rfcomm: bad sock_if\n"); return -1; }
    r = si->listen(1, name, uuid, 0, &fd, 0);
    fprintf(stderr, "hu: bt: rfcomm listen \"%s\": %d fd %d\n", name, r, fd);
    return r == 0 ? fd : -1;
}

/* power on: module reset (GPIO, ~1 s) and SetPower - in a separate thread, so as not to hold up the frame */
static void *power_on_thread(void *x)
{
    (void)x;
    if (system("/usr/bin/bluelink.sh") != 0) fprintf(stderr, "hu: bt: bluelink.sh failed\n");
    usleep(100000);
    nfmCmdSetPower(H, 1);
    return NULL;
}
static void *start_thread(void *x)
{
    void *p = malloc(4);
    (void)x;
    nfmCmdSetCallback(NULL, NULL);
    nfmCmdSetCbk_PowerStatus(NULL, cb_power);
    nfmCmdSetCbk_GetPairedList(NULL, cb_paired);
    nfmCmdSetCbk_InquiryStatus(NULL, cb_inquiry);
    nfmCmdSetCbk_ConnectionStatus(NULL, cb_conn);
    nfmCmdSetCbk_RemotePairRequest(NULL, cb_pair);
    nfmCmdSetCbk_HandsfreeBattStatusChanged(NULL, cb_batt);
    nfmCmdSetCbk_HandsfreeSignalStrength(NULL, cb_sig);
    nfmCmdSetCbk_AVRCPGetCapability(NULL, cb_cap);
    nfmCmdSetCbk_AVRCPNowPlaying(NULL, cb_now);
    nfmCmdSetCbk_AVRCPPlayStatus(NULL, cb_pstat);
    nfmCmdSetCbk_Event_TrackChanged(NULL, cb_trkchg);
    nfmCmdSetCbk_Event_PlaybackStatusChanged(NULL, cb_pbs);
    nfmCmdSetCbk_Event_PlaybackPosChanged(NULL, cb_pos);
    nfmCmdSetCbk_HandsfreeRing(NULL, cb_ring);
    nfmCmdSetCbk_HandsfreeCurrentCall(NULL, cb_curcall);
    nfmCmdSetCbk_HandsfreeOutgoingCall(NULL, cb_outcall);
    nfmCmdSetCbk_HandsfreeCallActive(NULL, cb_active);
    nfmCmdSetCbk_HandsfreeCallTerminated(NULL, cb_ended);
    nfmCmdSetCbk_HandsfreeScoStatus(NULL, cb_sco);
    nfmCmdSetCbk_A2DPStreamStatus(NULL, cb_stream);
    nfmCmdSetCbk_PhonebookDownloadStatus(NULL, cb_pbdl);
    if (nfmCmdStartup(&H, p) != 1) { fprintf(stderr, "hu: bt: startup failed\n"); nfmCmdEnd(&H); return NULL; }
    nfmCmdSetAutoConnectionEnable(H, 1);                       /* the stack itself reconnects the last phone */
    ready = 1;
    power_on_thread(NULL);
    return NULL;
}

int bt_init(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, start_thread, NULL)) return -1;
    pthread_detach(t);
    return 0;
}

/* power off is soft: we do not shut the stack down (the stock HU never does; power on after SetPower(0) in
 * libbluelink does not work - verified 2026-10-24: the stack stops, SetPower(1) never reaches the module).
 * "Off": disconnect everything, invisible, no auto-connect, drop incoming connections; "on" - the reverse. */
static int soft_off;
static int drop_others(const uint8_t *keep);
static void connect_after_drop(const uint8_t *a, double now);
void bt_power(int on)
{
    static const uint8_t none[6];
    pthread_t t;
    if (!ready) return;
    if (!on) {
        soft_off = 1;
        nfmCmdSetAutoConnectionEnable(H, 0);
        nfmCmdSetScanEnable(H, 0);
        nfmCmdInquiryStop(H);
        drop_others(none);
    } else if (soft_off) {
        soft_off = 0;
        nfmCmdSetAutoConnectionEnable(H, 1);
        A.bt_on = 1;
        if (A.n_paired > 0) { uint8_t a[6]; if (!addr_of(a, A.paired[0].id)) connect_after_drop(a, A.pend_t); }   /* last phone */
    } else if (!pthread_create(&t, NULL, power_on_thread, NULL)) pthread_detach(t);   /* the stack switched itself off - switch it on */
}
void bt_discoverable(int on) { if (ready) nfmCmdSetScanEnable(H, on ? 1 : 0); }
void bt_search(int on) { if (ready) { if (on) nfmCmdInquiryStart(H); else nfmCmdInquiryStop(H); } }
/* pairing during a search often fails in bluedroid (verified: state 2 ok 0) - stop the search first */
void bt_pair(const char *id)
{
    uint8_t a[6];
    fprintf(stderr, "hu: bt: %s %s\n", __func__, id);
    if (!ready || addr_of(a, id)) return;
    if (A.searching) { nfmCmdInquiryStop(H); A.searching = 0; usleep(200000); }
    nfmCmdPairRequest(H, a);
}
/* design: one device is connected - we disconnect the others */
static int drop_others(const uint8_t *keep)
{
    int i, n = 0;
    for (i = 0; i < 8; i++)
        if ((links[i].conn || links[i].prof) && memcmp(links[i].addr, keep, 6)) { nfmCmdDisconnectRemoteDevice(H, links[i].addr, 0xFFFFFFFFu); n++; }
    return n;
}
/* connect: first disconnect the others, the connection itself is in bt_poll once they disconnect (otherwise the stack loses it) */
static void connect_after_drop(const uint8_t *a, double now)
{
    memcpy(want_addr, a, 6); want_t = now;
    if (!drop_others(a)) { nfmCmdConnectRemoteDevice(H, a, 0xFFFFFFFFu); want_t = 0; }
}

void bt_connect(const char *id) { fprintf(stderr, "hu: bt: %s %s\n", __func__, id); uint8_t a[6]; if (ready && !addr_of(a, id)) connect_after_drop(a, A.pend_t); }
void bt_disconnect(const char *id) { fprintf(stderr, "hu: bt: %s %s\n", __func__, id); uint8_t a[6]; if (ready && !addr_of(a, id)) nfmCmdDisconnectRemoteDevice(H, a, 0xFFFFFFFFu); }
/* removal: first disconnect, then remove - removing a connected phone corrupts bt_config.xml (2026-10-24:
 * an entry with a truncated address, the stack spins in BTIF at 100% on every start and does not enable BT) */
static uint8_t rm_addr[6];
static char rm_id[18];                                         /* being removed: we drop it from the list immediately (owner: from the first press) */
static volatile int rm_done;
static void *remove_thread(void *x)
{
    (void)x;
    nfmCmdDisconnectRemoteDevice(H, rm_addr, 0xFFFFFFFFu);
    sleep(3);                                                  /* profile disconnection - up to 2 s */
    nfmCmdDeletePairDevice(H, rm_addr);
    rm_done = 1;
    nfmCmdGetPairedList(H);
    return NULL;
}
void bt_remove(const char *id)
{
    pthread_t t;
    int i;
    if (!ready || rm_id[0] || addr_of(rm_addr, id)) return;   /* one removal at a time */
    fprintf(stderr, "hu: bt: %s %s\n", __func__, id);
    snprintf(rm_id, sizeof rm_id, "%s", id); rm_done = 0;
    for (i = 0; i < A.n_paired; i++)
        if (!strcmp(A.paired[i].id, id)) { memmove(&A.paired[i], &A.paired[i + 1], sizeof(btdev) * (size_t)(A.n_paired - i - 1)); A.n_paired--; break; }
    if (!strcmp(A.bt_conn, id)) A.bt_conn[0] = 0;
    if (!strcmp(A.bt_pending, id)) A.bt_pending[0] = 0;
    if (!pthread_create(&t, NULL, remove_thread, NULL)) pthread_detach(t);
    else rm_id[0] = 0;
}

/* device type for the icon (design: 0 phone, 1 tablet, 2 speaker, 3 laptop) - from DevClass in bt_config.xml
 * (Class of Device: major = bits 8..12, minor = bits 2..7); no entry - phone */
static int dev_kind(const char *id)
{
    FILE *f = fopen("/mnt/data1/bluetooth/bt_config.xml", "r");
    char line[256], tag[32];
    int in = 0, kind = 0, i;
    long cod;
    if (!f) return 0;
    snprintf(tag, sizeof tag, "Tag=\"%s\"", id);
    for (i = 0; tag[i]; i++) if (tag[i] >= 'A' && tag[i] <= 'F') tag[i] = (char)(tag[i] - 'A' + 'a');   /* the address there is in lowercase */
    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "Tag=\"");
        if (strstr(line, tag)) { in = 1; continue; }
        if (in && p && strlen(p) > 22 && p[7] == ':' && p[10] == ':' && p[22] == '"') break;   /* entry of the next device */
        if (in && (p = strstr(line, "\"DevClass\""))) {
            p = strchr(p, '>');
            cod = p ? strtol(p + 1, NULL, 10) : 0;
            switch ((cod >> 8) & 0x1F) {
            case 1: kind = ((cod >> 2) & 0x3F) == 7 ? 1 : 3; break;    /* computer; minor 7 - tablet */
            case 4: kind = 2; break;                                   /* audio/video */
            default: kind = 0;
            }
            break;
        }
    }
    fclose(f);
    return kind;
}

void bt_media(int op) { if (ready && A.bt_av) nfmCmdAVRCPControlCommand(H, av_addr, op); }
/* ---- calls: address - phone with HFP */
void bt_dial(const char *number) { if (ready && number && number[0]) fprintf(stderr, "hu: bt: dial rc %d\n", nfmCmdDialCall(H, hf_addr, number)); }
void bt_answer(void) { if (ready) nfmCmdAnswerCall(H, hf_addr); }
void bt_reject(void) { if (ready) nfmCmdRejectCall(H, hf_addr); }
void bt_hangup(void) { if (ready) nfmCmdTerminateCall(H, hf_addr); }
void bt_mic_mute(int on) { if (ready) nfmCmdMicrophoneMute(H, hf_addr, on ? 1 : 0); }
void bt_audio_car(int car) { if (ready) nfmCmdAudioTransfer(H, hf_addr, car ? 0 : 1); }   /* x: 0 - to the car, 1 - to the phone [NOTES_bt] */
void bt_a2dp_mute(int on) { if (ready) fprintf(stderr, "hu: bt: a2dp mute %d rc %d\n", on, nfmCmdA2dpMute(H, av_addr, on ? 1 : 0)); }
void bt_dtmf(char c) { char d[2] = { c, 0 }; if (ready) nfmCmdSendDTMF(H, hf_addr, d); }

/* ---- contacts and call log: the library writes vCards to /mnt/data1/bluetooth/<ADDRESS>/PBAP/telecom/ (pb.vcf, cch.vcf) */
static void pb_start(int step)
{
    PB_TARGET t = { 0, step == 1 ? 0 : 4 };                 /* 0 - phonebook, 4 - all calls */
    pb_step = step;
    A.pb_state = 1;
    fprintf(stderr, "hu: bt: pbap %d rc %d\n", step, nfmCmdDownloadNoPhotoPhonebookStart(H, hf_addr, t, step == 1 ? 2000 : 100));
}

/* QUOTED-PRINTABLE (=D0=98...) -> bytes */
static void qp_decode(char *s)
{
    char *w = s;
    for (; *s; s++) {
        if (*s == '=' && s[1] && s[2]) { unsigned v; if (sscanf(s + 1, "%2x", &v) == 1) { *w++ = (char)v; s += 2; continue; } }
        if (*s == '=' && !s[1]) break;                       /* soft line break */
        *w++ = *s;
    }
    *w = 0;
}
static void pb_dir(char *p, size_t n)
{
    snprintf(p, n, "/mnt/data1/bluetooth/%02X%02X%02X%02X%02X%02X/PBAP/telecom", hf_addr[0], hf_addr[1], hf_addr[2], hf_addr[3], hf_addr[4], hf_addr[5]);
}
/* one vCard line: "KEY;PARAMS:value"; *params - parameters, returns the value */
static char *vc_split(char *line, char **params)
{
    char *c = strchr(line, ':'), *sc;
    if (!c) return NULL;
    *c = 0;
    sc = strchr(line, ';');
    *params = sc ? sc + 1 : (char *)"";
    if (sc) *sc = 0;
    if (strstr(*params, "QUOTED-PRINTABLE")) qp_decode(c + 1);
    return c + 1;
}
static void pb_parse(int step)
{
    char path[160], line[512], *v, *pr;
    FILE *f;
    pb_dir(path, sizeof path);
    strncat(path, step == 1 ? "/pb.vcf" : "/cch.vcf", sizeof path - strlen(path) - 1);
    if (!(f = fopen(path, "r"))) { fprintf(stderr, "hu: bt: no %s\n", path); return; }
    if (step == 1) A.n_cont = 0; else A.n_rec = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "BEGIN:VCARD")) {
            void *g;
            if (step == 1 && (g = grow(PB, &pb_cap, A.n_cont + 1, sizeof *PB))) { PB = g; memset(&PB[A.n_cont], 0, sizeof PB[0]); }
            if (step == 2 && (g = grow(REC, &rec_cap, A.n_rec + 1, sizeof *REC))) { REC = g; memset(&REC[A.n_rec], 0, sizeof REC[0]); }
        } else if (!strcmp(line, "END:VCARD")) {
            if (step == 1 && A.n_cont < pb_cap && PB[A.n_cont].nnum) A.n_cont++;   /* without a number - not needed */
            if (step == 2 && A.n_rec < rec_cap && REC[A.n_rec].num[0]) A.n_rec++;
        } else if (!(v = vc_split(line, &pr))) continue;
        else if ((step == 1 && A.n_cont >= pb_cap) || (step == 2 && A.n_rec >= rec_cap)) continue;   /* no room for the entry */
        else if (!strcmp(line, "FN") && v[0]) snprintf(step == 1 ? PB[A.n_cont].name : REC[A.n_rec].name, 64, "%s", v);
        else if (!strcmp(line, "TEL")) {
            if (step == 2) snprintf(REC[A.n_rec].num, sizeof REC[0].num, "%s", v);
            else if (A.n_cont < pb_cap && PB[A.n_cont].nnum < 3) {
                pb_contact *c = &PB[A.n_cont];
                snprintf(c->num[c->nnum], sizeof c->num[0], "%s", v);
                c->kind[c->nnum++] = strstr(pr, "HOME") ? 'h' : strstr(pr, "WORK") ? 'w' : strstr(pr, "CELL") ? 'm' : 'o';
            }
        } else if (step == 2 && !strcmp(line, "X-IRMC-CALL-DATETIME")) {
            int Y, M, D, h, m;
            REC[A.n_rec].dir = strstr(pr, "MISSED") ? 0 : strstr(pr, "DIALED") ? 1 : 2;
            if (sscanf(v, "%4d%2d%2dT%2d%2d", &Y, &M, &D, &h, &m) == 5) REC[A.n_rec].t = (((Y * 100LL + M) * 100 + D) * 100 + h) * 100 + m;
        }
    }
    fclose(f);
    fprintf(stderr, "hu: bt: pbap parsed %d: %d contacts, %d calls\n", step, A.n_cont, A.n_rec);
}

/* AVRCP player settings: array by attribute id (2 repeat: 1 off, 3 all; 3 shuffle: 1 off, 2 on), 0 - do not change [like ShellApp] */
void bt_media_mode(int shuffle, int repeat)
{
    uint8_t at[5] = { 0, 0, (uint8_t)(repeat ? 3 : 1), (uint8_t)(shuffle ? 2 : 1), 0 };
    if (ready && A.bt_av) nfmCmdSetCurrentPlayerAttributeValues(H, av_addr, at);
}

/* ---- main loop */
static int link_i(const uint8_t *a)
{
    int i, free_i = -1;
    for (i = 0; i < 8; i++) {
        if (links[i].prof && !memcmp(links[i].addr, a, 6)) return i;
        if (!links[i].prof && free_i < 0) free_i = i;
    }
    if (free_i >= 0) { memcpy(links[free_i].addr, a, 6); links[free_i].conn = 0; }
    return free_i;
}

/* connected - HFP or A2DP present; connecting - something in state 3 */
static void apply_links(void)
{
    int i;
    char id[18];
    int best = -1;
    A.bt_conn[0] = 0;
    for (i = 0; i < 8; i++) if ((links[i].conn & (2 | 4)) && (best < 0 || links[i].seq > links[best].seq)) best = i;
    if (best >= 0) id_of(A.bt_conn, links[best].addr);
    if (A.bt_conn[0] && !strcmp(A.bt_conn, A.bt_pending)) A.bt_pending[0] = 0;
    for (i = 0; i < 8 && A.bt_pending[0]; i++) {
        id_of(id, links[i].addr);
        if (!strcmp(id, A.bt_pending) && !links[i].prof && !links[i].conn) A.bt_pending[0] = 0;
    }
}

void bt_poll(double now)
{
    bt_ev e;
    int changed = 0, i;
    char id[18];
    for (;;) {
        pthread_mutex_lock(&mu);
        if (qh == qt) { pthread_mutex_unlock(&mu); break; }
        e = q[qh]; qh = (qh + 1) % QN;
        pthread_mutex_unlock(&mu);
        changed = 1;
        id_of(id, e.addr);
        fprintf(stderr, "hu: bt: ev %d %s a=%d b=%d prof=0x%x %s\n", e.type, id, e.a, e.b, (unsigned)e.prof, e.name);
        switch (e.type) {
        case E_POWER:
            A.bt_on = e.a == 1 && !soft_off;
            if (A.bt_on) {
                nfmCmdSetLocalDeviceName(H, HU_NAME);             /* name - as "This device" (version.h) */
                nfmCmdSetScanEnable(H, 0);
                nfmCmdGetPairedList(H);
            } else {
                memset(links, 0, sizeof links);
                A.bt_conn[0] = A.bt_pending[0] = A.pairing[0] = 0; A.disc = A.searching = 0; A.n_extra = 0;
                A.ph_batt = A.ph_sig = -1;
            }
            fprintf(stderr, "hu: bt: power %d\n", e.a);
            break;
        case E_PAIRED:
            pthread_mutex_lock(&mu);
            {
                int k;
                for (A.n_paired = 0, k = 0; k < plist_n && A.n_paired < 8; k++) {
                    i = A.n_paired;
                    id_of(A.paired[i].id, plist[k].addr);
                    if (rm_id[0] && !strcmp(A.paired[i].id, rm_id)) continue;   /* being removed - not shown anymore */
                    snprintf(A.paired[i].name, sizeof A.paired[i].name, "%s", plist[k].name);
                    A.paired[i].kind = dev_kind(A.paired[i].id);
                    A.n_paired++;
                }
                if (rm_done) rm_id[0] = 0;                             /* list after removal - the filter is no longer needed */
            }
            pthread_mutex_unlock(&mu);
            kind_t = now + 3;
            for (i = 0; i < A.n_paired; i++)                       /* paired ones - not in the list of available */
                for (int j = 0; j < A.n_extra; j++)
                    if (!strcmp(A.extra[j].id, A.paired[i].id)) { memmove(&A.extra[j], &A.extra[j + 1], sizeof(btdev) * (size_t)(A.n_extra - j - 1)); A.n_extra--; j--; }
            break;
        case E_FOUND:
            for (i = 0; i < A.n_extra; i++) if (!strcmp(A.extra[i].id, id)) break;
            if (i == A.n_extra && A.n_extra < 8) {
                snprintf(A.extra[i].id, sizeof A.extra[i].id, "%s", id);
                snprintf(A.extra[i].name, sizeof A.extra[i].name, "%s", e.name[0] ? e.name : id);
                A.extra[i].kind = dev_kind(id);
                A.n_extra++;
            }
            break;
        case E_SEARCH:
            A.searching = e.a;
            break;
        case E_CONN:
            if ((i = link_i(e.addr)) < 0) break;
            if (e.a == 0 && soft_off) { nfmCmdDisconnectRemoteDevice(H, e.addr, 0xFFFFFFFFu); break; }   /* BT "off" */
            if (e.a == 0) {
                if (!links[i].conn) { links[i].seq = ++link_seq; drop_others(e.addr); }   /* another one connected (also from the phone) - disconnect the old one */
                links[i].conn |= (int)e.prof;
            } else links[i].conn &= ~(int)e.prof;
            if (e.a == 3) { links[i].prof |= (int)e.prof; if (!A.bt_conn[0]) snprintf(A.bt_pending, sizeof A.bt_pending, "%s", id); }
            else links[i].prof = links[i].conn;
            if (!links[i].conn && !links[i].prof) A.ph_batt = A.ph_sig = -1;
            if (e.prof == 8 && e.a == 0) { memcpy(av_addr, e.addr, 6); A.bt_av = 1; nfmCmdGetCapability(H, av_addr); }   /* AVRCP connected */
            if (e.prof == 8 && e.a == 1 && !memcmp(av_addr, e.addr, 6)) { A.bt_av = 0; A.bt_playing = 0; A.bt_stream = 0; }
            if (e.prof == 2 && e.a == 1) A.bt_call = 0;          /* HFP disconnected - no call */
            if (e.prof == 2 && e.a == 0) { memcpy(hf_addr, e.addr, 6); pb_step = -1; pb_t = now; }   /* HFP: contacts after 2 s */
            if (e.prof == 2 && e.a == 1 && !memcmp(hf_addr, e.addr, 6)) { A.n_cont = A.n_rec = 0; A.pb_state = 0; pb_step = 0; }
            apply_links();
            break;
        case E_PAIRREQ:
            /* we confirm only what we started ourselves or when the HU is visible (design: "Pairing..." without a code) */
            if (e.a == 0 || e.a == 1) {
                if (!strcmp(A.pairing, id) || A.disc) nfmCmdAnswerPair(H, e.addr, 4, "0000");
                else nfmCmdRefusePair(H, e.addr);
            } else if (e.a == 2) {
                if (!strcmp(A.pairing, id)) A.pairing[0] = 0;
                nfmCmdGetPairedList(H);
                if (e.b) { snprintf(A.bt_pending, sizeof A.bt_pending, "%s", id); A.pend_t = now; connect_after_drop(e.addr, now); }
            }
            break;
        case E_CAP:                                               /* AVRCP: player events and the current track */
            memcpy(av_addr, e.addr, 6);
            nfmCmdRegisterEventWatcher(H, av_addr, 1, 0);         /* playback status */
            nfmCmdRegisterEventWatcher(H, av_addr, 2, 0);         /* track change */
            nfmCmdRegisterEventWatcher(H, av_addr, 5, 0);         /* position */
            nfmCmdRegisterEventWatcher(H, av_addr, 8, 0);         /* player settings */
            /* fallthrough */
        case E_TRKCHG:
            nfmCmdGetNowPlayingAttributes(H, av_addr);
            nfmCmdGetPlayStatus(H, av_addr);
            break;
        case E_TRACK:
            if (strcmp(A.bt_title, e.t) || strcmp(A.bt_artist, e.ar)) A.bt_seq++;
            snprintf(A.bt_title, sizeof A.bt_title, "%s", e.t);
            snprintf(A.bt_artist, sizeof A.bt_artist, "%s", e.ar);
            snprintf(A.bt_album, sizeof A.bt_album, "%s", e.al);
            if (e.b > 0) A.bt_len = e.b;
            break;
        case E_PSTAT:
            if (e.b > 0) A.bt_len = e.b;
            A.bt_pos = e.prof; A.bt_pos_t = now; A.bt_playing = e.a == 1;
            break;
        case E_PBS: A.bt_playing = e.a == 1; A.bt_pos_t = now; break;
        case E_POS: A.bt_pos = e.a; A.bt_pos_t = now; break;
        case E_CALL:
            if (e.a == 1 && A.bt_call == 3) break;               /* "current call" during a conversation - the same one */
            if (e.a == 3 && A.bt_call != 3) A.bt_call_t = now;
            if (e.a == 0) { A.bt_sco = 0; if (pb_step == 0 && A.pb_state == 2) { pb_start(2); pb_t = now; } }   /* call ended - update the call log */
            A.bt_call = e.a;
            if (e.name[0]) snprintf(A.bt_call_num, sizeof A.bt_call_num, "%s", e.name);
            break;
        case E_SCO: A.bt_sco = e.a; break;
        case E_STREAM: A.bt_stream = e.a != 0; break;           /* audio from the phone is playing (A2DP) - stick quieter */
        case E_PBDL: fprintf(stderr, "hu: bt: pbap status %d\n", e.a); break;   /* values not decoded - readiness is judged by files */
        case E_BATT: A.ph_batt = e.a; break;
        case E_SIGNAL: A.ph_sig = e.a; break;
        }
    }
    if (want_t) {                                                 /* waiting for the others to disconnect (up to 5 s) */
        int busy = 0;
        for (i = 0; i < 8; i++) if ((links[i].conn || links[i].prof) && memcmp(links[i].addr, want_addr, 6)) busy = 1;
        if (!busy || now - want_t > 5) { want_t = 0; nfmCmdConnectRemoteDevice(H, want_addr, 0xFFFFFFFFu); }
    }
    if (pb_step == -1 && now - pb_t > 2) { pb_start(1); pb_t = now; }
    {   /* contacts and call log files: the library writes them when the phone provides them (after access is granted) - we watch them */
        static double chk;
        static time_t mt[2];
        if (hf_addr[0] | hf_addr[1] | hf_addr[2] && now - chk > 2) {
            int k;
            chk = now;
            for (k = 0; k < 2; k++) {
                char path[160];
                struct stat st;
                pb_dir(path, sizeof path);
                strncat(path, k ? "/cch.vcf" : "/pb.vcf", sizeof path - strlen(path) - 1);
                if (stat(path, &st) == 0 && st.st_mtime != mt[k]) {
                    mt[k] = st.st_mtime;
                    pb_parse(k + 1);
                    if (k == 0 && pb_step == 1) pb_start(2);   /* phonebook is there - now the call log */
                    if (k == 1) { pb_step = 0; A.pb_state = 2; }
                    changed = 1;
                }
            }
            if (pb_step == 1 && pb_t > 0 && now - pb_t > 60) pb_start(2);   /* the phonebook did not arrive within a minute - the call log anyway */
        }
    }
    if (A.bt_pending[0] && now - A.pend_t > 20) { A.bt_pending[0] = 0; changed = 1; }   /* did not connect */
    if (kind_t && now >= kind_t) {
        kind_t = 0;
        for (i = 0; i < A.n_paired; i++) A.paired[i].kind = dev_kind(A.paired[i].id);
        changed = 1;
    }
    if (changed) { base_dirty(); screen_dirty(); layout_again(); }
}
