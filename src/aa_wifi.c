/* Wireless Android Auto, see aa_wifi.h. RFCOMM protocol (as in wireless AA adapters, e.g. WirelessAndroidAutoDongle):
 * frame [protobuf length 2][id 2][protobuf], big-endian.
 *   HU -> phone 1 WifiStartRequest  { 1 ip, 2 port }
 *   phone -> HU 2 WifiInfoRequest   {}
 *   HU -> phone 3 WifiInfoResponse  { 1 ssid, 2 password, 3 bssid, 4 security (8 - WPA2 personal), 5 AP type (1 - dynamic) }
 *   phone -> HU 7 WifiStartResponse / 6 WifiConnectStatus - connected to the network; then TCP to ip:port.
 * The network comes from the hostapd config (ssid, wpa_passphrase), BSSID and IP from the wlan0 interface.
 * As in cars: the phone came over RFCOMM - WifiStartRequest right away; the AA session runs in the background, the screen - via the tile (aa_view.c).
 * RFCOMM cannot be silently held: the phone hangs on "Connecting" and reconnects.
 * The AP is like WirelessAndroidAutoDongle (aawgd, owner 2026-10-26 "1-to-1"): up since boot (wifi.sh) and not taken down
 * between sessions, constant password; "wifi.sh up" here - only if it is not up yet (up to the 25th s of boot).
 * Before, it was taken down after every session and the password changed - after the car was switched off/on the phone connected to the AP twice, left
 * the air, AA stalled and reconnected (in the car 2026-10-26). */
#define _GNU_SOURCE                               /* accept4 */
#include "aa_wifi.h"
#include "aa_pb.h"
#include "bt.h"
#include "ui_int.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define PORT 5288
#define IFACE "wlan0"
static const unsigned char UUID_AAW[16] = { 0x4d, 0xe1, 0x7a, 0x00, 0x52, 0xcb, 0x11, 0xe6, 0xbd, 0xf4, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66 };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int pending = -1;                                   /* accepted TCP - waiting for aa.c */
static volatile int active;                                /* AA session over Wi-Fi is running (aa.c took the socket) */
static volatile int want;                                  /* tell the phone where to go (AP + WifiStartRequest) */
static volatile int paused;                                /* car is off (left): do not start a new session */
static volatile int lost;                                  /* phone is gone - close the session, do not wait for the ping (~10 s) */
static uint8_t phone[6];                                   /* Bluetooth address of the wireless-session phone */
static volatile int phone_ok;
static volatile int lfd_cur = -1;                          /* listening RFCOMM (AA service in Bluetooth) */
static volatile int rf_on;                                 /* phone on RFCOMM (can be called again - AA tile) */

static int ap(const char *op)
{
    char c[96];
    int r;
    snprintf(c, sizeof c, "sh /dev/wifi/wifi.sh %s", op);
    r = system(c);
    fprintf(stderr, "hu: aaw: access point %s: %d\n", op, r);
    return r;
}

/* ---------------------------------------------------------------- HU network */

static int conf_value(const char *key, char *out, size_t n)   /* key=value from the hostapd config */
{
    const char *p = getenv("HU_WIFI_CONF") ? getenv("HU_WIFI_CONF") : "/dev/wifi/hostapd.conf";
    char l[256];
    size_t k = strlen(key);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    while (fgets(l, sizeof l, f)) {
        if (strncmp(l, key, k) || l[k] != '=') continue;
        l[strcspn(l, "\r\n")] = 0;
        snprintf(out, n, "%s", l + k + 1);
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

static int iface_ip(char *out, size_t n)
{
    struct ifreq r;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), ok;
    if (s < 0) return -1;
    memset(&r, 0, sizeof r);
    snprintf(r.ifr_name, sizeof r.ifr_name, IFACE);
    ok = ioctl(s, SIOCGIFADDR, &r) == 0;
    close(s);
    if (!ok) return -1;
    snprintf(out, n, "%s", inet_ntoa(((struct sockaddr_in *)&r.ifr_addr)->sin_addr));
    return 0;
}

static int iface_mac(char *out, size_t n)
{
    FILE *f = fopen("/sys/class/net/" IFACE "/address", "r");
    if (!f) return -1;
    if (!fgets(out, (int)n, f)) { fclose(f); return -1; }
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
    return 0;
}

/* ---------------------------------------------------------------- RFCOMM */

static int write_all(int fd, const void *b, size_t n)
{
    const uint8_t *p = b;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int read_all(int fd, void *b, size_t n, int ms)   /* 0 - read, -1 - end/error/timeout */
{
    uint8_t *p = b;
    while (n) {
        struct pollfd q = { fd, POLLIN, 0 };
        ssize_t r;
        if (poll(&q, 1, ms) <= 0) return -1;
        r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int rf_send(int fd, int id, const pb_w *w)
{
    uint8_t h[4] = { (uint8_t)(w->n >> 8), (uint8_t)w->n, (uint8_t)(id >> 8), (uint8_t)id };
    if (w->err || write_all(fd, h, 4) || write_all(fd, w->b, w->n)) return -1;
    fprintf(stderr, "hu: aaw: -> %d (%u B)\n", id, (unsigned)w->n);
    return 0;
}

/* start: the AP (already up since boot - wifi.sh does nothing) and tell the phone where to go (WifiStartRequest). 0 - told */
static int rf_start(int fd, char *ssid, size_t ns, char *key, size_t nk, char *mac, size_t nm)
{
    char ip[32];
    uint8_t b[64];
    pb_w w;
    if (ap("up")) return -1;
    if (iface_ip(ip, sizeof ip) || conf_value("ssid", ssid, ns) || conf_value("wpa_passphrase", key, nk) || iface_mac(mac, nm)) {
        fprintf(stderr, "hu: aaw: no Wi-Fi access point (" IFACE " / hostapd.conf)\n");
        return -1;
    }
    pb_init(&w, b, sizeof b);
    pb_str(&w, 1, ip); pb_varint(&w, 2, PORT);
    return rf_send(fd, 1, &w);
}

/* one phone connection: start right away, then keep answering until the phone closes */
static void rf_session(int fd)
{
    char ssid[64] = "", key[80] = "", mac[32] = "";
    uint8_t b[256];
    pb_w w;
    time_t started = 0;
    want = 1;                                                    /* as in cars: immediately, we do not wait (owner 2026-10-25) */
    for (;;) {
        struct pollfd q = { fd, POLLIN, 0 };
        uint8_t h[4];
        int n, id, r = poll(&q, 1, 300);
        if (want && !paused) {
            want = 0;
            if (rf_start(fd, ssid, sizeof ssid, key, sizeof key, mac, sizeof mac) == 0) started = time(NULL);
        }
        if (started && (active || pending >= 0)) started = 0;   /* arrived over TCP */
        if (started && time(NULL) - started > 60) { fprintf(stderr, "hu: aaw: phone did not come over Wi-Fi\n"); started = 0; }
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) continue;
        if (read_all(fd, h, 4, 5000)) break;
        n = h[0] << 8 | h[1]; id = h[2] << 8 | h[3];
        if (n > (int)sizeof b || (n && read_all(fd, b, (size_t)n, 5000))) break;
        fprintf(stderr, "hu: aaw: <- %d (%d B)\n", id, n);
        if (id == 2 && ssid[0]) {                                /* WifiInfoRequest - network */
            uint8_t o[256];
            pb_init(&w, o, sizeof o);
            pb_str(&w, 1, ssid); pb_str(&w, 2, key); pb_str(&w, 3, mac);
            pb_varint(&w, 4, 8); pb_varint(&w, 5, 1);
            if (rf_send(fd, 3, &w)) break;
        } else if (id == 6 || id == 7) {                         /* connected to the network / reply to start */
            pb_r pr = { b, b + n };
            int f, wt;
            uint64_t v;
            const uint8_t *p;
            size_t len;
            while (pb_next(&pr, &f, &wt, &v, &p, &len) == 1) if (wt == 0) fprintf(stderr, "hu: aaw:    field %d = %lld\n", f, (long long)v);
        }
    }
    fprintf(stderr, "hu: aaw: rfcomm closed\n");
}

static int accept_fd(int lfd, uint8_t addr[6])               /* next connection: fd from SCM_RIGHTS */
{
    uint8_t sig[16];
    char cb[CMSG_SPACE(sizeof(int))];
    struct iovec io = { sig, sizeof sig };
    struct msghdr m;
    struct cmsghdr *c;
    ssize_t r;
    memset(&m, 0, sizeof m);
    m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = cb; m.msg_controllen = sizeof cb;
    do r = recvmsg(lfd, &m, MSG_CMSG_CLOEXEC); while (r < 0 && errno == EINTR);
    if (r <= 0) return -2;                                   /* listening socket closed - the stack was restarted */
    memcpy(addr, sig + 2, 6);
    for (c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) { int fd; memcpy(&fd, CMSG_DATA(c), sizeof fd); return fd; }
    return -1;
}

static void *rf_thread(void *x)
{
    (void)x;
    for (;;) {
        int lfd, ch = -1;
        while (!A.bt_on) sleep(2);
        if ((lfd = bt_rfcomm_listen(UUID_AAW, "Android Auto Wireless")) < 0) { sleep(30); continue; }
        fcntl(lfd, F_SETFD, FD_CLOEXEC);
        lfd_cur = lfd;
        if (read_all(lfd, &ch, 4, 10000)) { fprintf(stderr, "hu: aaw: no rfcomm channel\n"); close(lfd); sleep(10); continue; }
        fprintf(stderr, "hu: aaw: rfcomm channel %d\n", ch);
        for (;;) {
            uint8_t a[6];
            int fd = accept_fd(lfd, a);
            if (fd == -2) break;
            if (fd < 0) continue;
            fprintf(stderr, "hu: aaw: phone %02X:%02X:%02X:%02X:%02X:%02X on rfcomm\n", a[0], a[1], a[2], a[3], a[4], a[5]);
            memcpy(phone, a, 6); phone_ok = 1;
            rf_on = 1;
            rf_session(fd);
            rf_on = 0;
            close(fd);
        }
        fprintf(stderr, "hu: aaw: rfcomm listener gone\n");
        lfd_cur = -1;
        close(lfd);
        sleep(3);
    }
    return NULL;
}

/* ---------------------------------------------------------------- phone gone */

/* hostapd events (control socket): AP-STA-DISCONNECTED - the phone disconnected from the AP (switched Wi-Fi off, left) */
static void *ev_thread(void *x)
{
    const char *loc = "/dev/wifi/hu_ev";
    (void)x;
    for (;;) {
        struct sockaddr_un l, r;
        char b[256];
        int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        ssize_t n;
        if (s < 0) { sleep(5); continue; }
        memset(&l, 0, sizeof l); l.sun_family = AF_UNIX; snprintf(l.sun_path, sizeof l.sun_path, "%s", loc);
        memset(&r, 0, sizeof r); r.sun_family = AF_UNIX; snprintf(r.sun_path, sizeof r.sun_path, "/dev/wifi/ctrl/" IFACE);
        unlink(loc);
        if (bind(s, (struct sockaddr *)&l, sizeof l) || connect(s, (struct sockaddr *)&r, sizeof r) || send(s, "ATTACH", 6, 0) != 6) {
            close(s); sleep(1); continue;                      /* no AP - wait */
        }
        for (;;) {
            struct pollfd q = { s, POLLIN, 0 };
            int pr = poll(&q, 1, 2000);
            if (pr < 0 && errno == EINTR) continue;
            if (pr == 0) { if (send(s, "PING", 4, 0) != 4) break; continue; }   /* hostapd alive? */
            if ((n = recv(s, b, sizeof b - 1, 0)) <= 0) break;
            b[n] = 0;
            if (strstr(b, "AP-STA-DISCONNECTED") && active) { fprintf(stderr, "hu: aaw: phone left Wi-Fi\n"); lost = 1; }
        }
        close(s); unlink(loc);
        sleep(1);
    }
    return NULL;
}

int aa_wifi_lost(void) { return lost; }

void aa_wifi_pause(int on)
{
    if (on == paused) return;
    paused = on;
    fprintf(stderr, "hu: aaw: %s\n", on ? "car off - no new sessions" : "car on");
    if (!on) want = 1;                                      /* phone is still on RFCOMM - call again */
}

void aa_wifi_poll(double now)
{
    static double gone_t;
    if (!active || !phone_ok || bt_linked(phone)) { gone_t = 0; return; }
    if (gone_t == 0) gone_t = now;
    else if (now - gone_t > 2 && !lost) { fprintf(stderr, "hu: aaw: phone left Bluetooth\n"); lost = 1; }   /* 2 s - not bounce */
}

/* ---------------------------------------------------------------- TCP 5288 */

static void *tcp_thread(void *x)
{
    struct sockaddr_in a;
    int s, one = 1;
    (void)x;
    /* all sockets are CLOEXEC: the AP (hostapd, udhcpd) is started by hu via system() - without it they inherited port 5288 and
     * the session sockets (after hu restarted the port was busy, the phone had nobody to connect to; in the car 2026-10-25). Busy - wait */
    for (;;) {
        if ((s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) return NULL;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (!bind(s, (struct sockaddr *)&a, sizeof a) && !listen(s, 2)) break;
        fprintf(stderr, "hu: aaw: tcp %d: %s - retry\n", PORT, strerror(errno));
        close(s);
        sleep(2);
    }
    for (;;) {
        socklen_t l = sizeof a;
        int c = accept4(s, (struct sockaddr *)&a, &l, SOCK_CLOEXEC), old;
        if (c < 0) { if (errno != EINTR) sleep(1); continue; }
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        fprintf(stderr, "hu: aaw: tcp from %s\n", inet_ntoa(a.sin_addr));
        pthread_mutex_lock(&mu);
        old = pending; pending = c;                          /* a new connection matters more than a failed one */
        pthread_mutex_unlock(&mu);
        if (old >= 0) close(old);
    }
    return NULL;
}

void aa_wifi_init(void)
{
    pthread_t t;
    if (!pthread_create(&t, NULL, rf_thread, NULL)) pthread_detach(t);
    if (!pthread_create(&t, NULL, tcp_thread, NULL)) pthread_detach(t);
    if (getenv("HU_AAW_EV") && !pthread_create(&t, NULL, ev_thread, NULL)) pthread_detach(t);   /* hostapd events: with them sessions
                                                                     * died after ~30 s by ping timeout (2026-10-25) - off for now */
}

int aa_wifi_pending(void) { return pending >= 0; }

/* the phone disconnected AA itself ("Disconnect" on the phone), RFCOMM remained: the phone waits for AA to be chosen on the car screen
 * ("To continue, select Android Auto on your vehicle screen", 2026-10-26) - the tile is available, and via it - WifiStartRequest
 * again (as on connection) */
int aa_wifi_phone(void) { return rf_on && !active && pending < 0 && !paused; }
void aa_wifi_call(void) { if (rf_on && !paused) { want = 1; fprintf(stderr, "hu: aaw: tile - call phone again\n"); } }

void aa_wifi_done(void) { active = 0; }   /* we do not take the AP down (aawgd) */


int aa_wifi_take(void)
{
    int fd;
    pthread_mutex_lock(&mu);
    fd = pending; pending = -1;
    if (fd >= 0) { active = 1; lost = 0; }
    pthread_mutex_unlock(&mu);
    return fd;
}
