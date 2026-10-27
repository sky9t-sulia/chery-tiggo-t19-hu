/* Android Auto: USB part. AOA protocol (Android Open Accessory 2.0):
 *  GET_PROTOCOL  - 0xC0, bRequest 51, 2 bytes (version, >= 1);
 *  SEND_STRING   - 0x40, bRequest 52, wIndex 0..5 (manufacturer, model, description, version, URI, serial);
 *  START         - 0x40, bRequest 53 -> the phone reconnects as 18d1:2d00..2d05, interface 0 - bulk IN/OUT.
 * Strings are as in aasdk/openauto ("Android" / "Android Auto"): the phone launches Android Auto based on them. */
#include "aa_usb.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/usbdevice_fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define AOA_GET_PROTOCOL 51
#define AOA_SEND_STRING  52
#define AOA_START        53
#define USBFS_MAX        16384             /* kernel 3.10: more than this per USBDEVFS_BULK is not allowed */

static const char *AOA_STR[6] = { "Android", "Android Auto", "Android Auto", "2.0.1", "https://f1xstudio.com", "HU-AAAAAA001" };

static int sys_int(const char *dir, const char *name, int hex)
{
    char p[300], b[32];
    FILE *f;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (!(f = fopen(p, "r"))) return -1;
    if (!fgets(b, sizeof b, f)) b[0] = 0;
    fclose(f);
    return (int)strtol(b, NULL, hex ? 16 : 10);
}

static int want_bus(void)
{
    const char *e = getenv("HU_AA_BUS");
#ifdef __arm__
    return e ? atoi(e) : 1;                /* HU: CarPlay port */
#else
    return e ? atoi(e) : 0;
#endif
}

static int ctrl(int fd, int type, int req, int val, int idx, void *data, int len)
{
    struct usbdevfs_ctrltransfer c;
    memset(&c, 0, sizeof c);
    c.bRequestType = type; c.bRequest = req; c.wValue = val; c.wIndex = idx; c.wLength = len;
    c.timeout = 1000; c.data = data;
    return ioctl(fd, USBDEVFS_CONTROL, &c);
}

/* bulk endpoints of interface 0 from the descriptors (read() of the device file: device + active configuration) */
static int endpoints(int fd, unsigned *in, unsigned *out)
{
    uint8_t d[4096];
    int n, i, ifc = -1;
    lseek(fd, 0, SEEK_SET);
    if ((n = read(fd, d, sizeof d)) < 18) return -1;
    *in = *out = 0;
    for (i = 18; i + 2 <= n && d[i] >= 2; i += d[i]) {
        if (d[i + 1] == 4 && i + 3 < n) ifc = d[i + 2];                     /* interface: bInterfaceNumber */
        else if (d[i + 1] == 5 && ifc == 0 && i + 3 < n && (d[i + 3] & 3) == 2) {   /* bulk endpoint */
            if (d[i + 2] & 0x80) { if (!*in) *in = d[i + 2]; }
            else if (!*out) *out = d[i + 2];
        }
    }
    return *in && *out ? 0 : -1;
}

static int open_dev(int bus, int dev)
{
    char p[64];
    snprintf(p, sizeof p, "/dev/bus/usb/%03d/%03d", bus, dev);
    return open(p, O_RDWR | O_CLOEXEC);
}

/* Wi-Fi adapter (wireless-AA dongle, including in the CarPlay port): AOA requests (vendor 51) hang its control channel -
 * after that the driver does not write registers (timeouts, in the car 2026-10-25). Do not touch: VID Realtek / TP-Link or the device
 * has a wireless network interface */
static int is_wifi(const char *d, const char *name, int vid)
{
    char p[400];
    DIR *n;
    struct dirent *e;
    int w = 0;
    if (vid == 0x0bda || vid == 0x2357) return 1;
    snprintf(p, sizeof p, "%s/%s:1.0/net", d, name);
    if (!(n = opendir(p))) return 0;
    while (!w && (e = readdir(n))) {
        char q[700];
        if (e->d_name[0] == '.') continue;
        snprintf(q, sizeof q, "%s/%s/wireless", p, e->d_name);
        w = access(q, F_OK) == 0;
    }
    closedir(n);
    return w;
}

int aa_usb_phone(int apple)
{
    DIR *dir = opendir("/sys/bus/usb/devices");
    struct dirent *e;
    int wb = want_bus(), found = 0;
    if (!dir) return 0;
    while (!found && (e = readdir(dir))) {
        char d[300];
        int vid, bus;
        if (e->d_name[0] == '.' || strchr(e->d_name, ':') || !strncmp(e->d_name, "usb", 3)) continue;
        snprintf(d, sizeof d, "/sys/bus/usb/devices/%s", e->d_name);
        vid = sys_int(d, "idVendor", 1); bus = sys_int(d, "busnum", 0);
        if (vid < 0 || (wb && bus != wb) || sys_int(d, "bDeviceClass", 1) == 9 || is_wifi(d, e->d_name, vid)) continue;
        found = apple ? vid == 0x05ac : vid != 0x05ac;
    }
    closedir(dir);
    return found;
}

int aa_usb_find(aa_usb *u)
{
    DIR *dir = opendir("/sys/bus/usb/devices");
    struct dirent *e;
    int wb = want_bus(), r = -1;
    u->fd = -1;
    if (!dir) return -1;
    while ((e = readdir(dir)) && r < 0) {
        char d[300];
        int vid, pid, bus, dev, fd, cls;
        uint8_t ver[2] = { 0, 0 };
        if (e->d_name[0] == '.' || strchr(e->d_name, ':') || !strncmp(e->d_name, "usb", 3)) continue;   /* interfaces, root hubs */
        snprintf(d, sizeof d, "/sys/bus/usb/devices/%s", e->d_name);
        vid = sys_int(d, "idVendor", 1); pid = sys_int(d, "idProduct", 1);
        bus = sys_int(d, "busnum", 0); dev = sys_int(d, "devnum", 0); cls = sys_int(d, "bDeviceClass", 1);
        if (vid < 0 || bus < 0 || dev < 0 || (wb && bus != wb) || cls == 9) continue;   /* foreign bus, hub */
        if (vid == 0x05ac) continue;                                       /* iPhone is CarPlay */
        if (is_wifi(d, e->d_name, vid)) continue;                          /* Wi-Fi dongle is not a phone, AOA hangs it */
        if ((fd = open_dev(bus, dev)) < 0) continue;
        if (vid == 0x18d1 && pid >= 0x2d00 && pid <= 0x2d05) {                /* already an accessory */
            unsigned ifc = 0;
            struct usbdevfs_ioctl dc = { 0, USBDEVFS_DISCONNECT, NULL };
            if (endpoints(fd, &u->ep_in, &u->ep_out) < 0) { close(fd); continue; }
            ioctl(fd, USBDEVFS_IOCTL, &dc);                                   /* detach the kernel driver, if any */
            if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifc) < 0) {
                fprintf(stderr, "hu: aa: claim %d-%d: %s\n", bus, dev, strerror(errno)); close(fd); continue;
            }
            u->fd = fd; u->bus = bus; u->dev = dev; r = 0;
            fprintf(stderr, "hu: aa: accessory %04x:%04x on %d-%d, ep in %02x out %02x\n", vid, pid, bus, dev, u->ep_in, u->ep_out);
            break;
        }
        if (ctrl(fd, 0xC0, AOA_GET_PROTOCOL, 0, 0, ver, 2) == 2 && (ver[0] | ver[1] << 8) >= 1) {
            int i, ok = 1;
            for (i = 0; i < 6 && ok; i++)
                ok = ctrl(fd, 0x40, AOA_SEND_STRING, 0, i, (void *)AOA_STR[i], (int)strlen(AOA_STR[i]) + 1) >= 0;
            if (ok && ctrl(fd, 0x40, AOA_START, 0, 0, NULL, 0) >= 0) {
                fprintf(stderr, "hu: aa: %04x:%04x on %d-%d -> accessory mode (AOA %d)\n", vid, pid, bus, dev, ver[0] | ver[1] << 8);
                r = 1;
            }
        }
        close(fd);
    }
    closedir(dir);
    return r;
}

/* reading is an asynchronous URB: always one in the queue (the phone must not stall), a poll timeout does not lose data
 * (with synchronous USBDEVFS_BULK a timeout cancels the transfer together with bytes that already arrived) */
static struct usbdevfs_urb rurb;
static uint8_t rbuf[USBFS_MAX];
static int rpending;

int aa_usb_read(aa_usb *u, void *buf, size_t n, int timeout_ms)
{
    struct pollfd pf;
    void *got = NULL;
    int len;
    if (!rpending) {
        memset(&rurb, 0, sizeof rurb);
        rurb.type = USBDEVFS_URB_TYPE_BULK; rurb.endpoint = u->ep_in; rurb.buffer = rbuf; rurb.buffer_length = sizeof rbuf;
        if (ioctl(u->fd, USBDEVFS_SUBMITURB, &rurb) < 0) return -1;
        rpending = 1;
    }
    pf.fd = u->fd; pf.events = POLLOUT; pf.revents = 0;
    if (poll(&pf, 1, timeout_ms) <= 0) return 0;                   /* completed URBs - POLLOUT */
    if (pf.revents & (POLLERR | POLLHUP)) return -1;
    if (ioctl(u->fd, USBDEVFS_REAPURBNDELAY, &got) < 0) return errno == EAGAIN ? 0 : -1;
    rpending = 0;
    if (got != &rurb || rurb.status) return -1;
    len = rurb.actual_length < (int)n ? rurb.actual_length : (int)n;
    memcpy(buf, rbuf, (size_t)len);
    return len;
}

int aa_usb_write(aa_usb *u, const void *buf, size_t n, int timeout_ms)
{
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < n) {
        struct usbdevfs_bulktransfer b;
        int r;
        b.ep = u->ep_out; b.len = n - done > USBFS_MAX ? USBFS_MAX : (unsigned)(n - done); b.timeout = timeout_ms;
        b.data = (void *)(p + done);
        if ((r = ioctl(u->fd, USBDEVFS_BULK, &b)) < 0) return -1;
        done += (size_t)r;
    }
    return (int)done;
}

void aa_usb_close(aa_usb *u)
{
    if (u->fd >= 0) {
        unsigned ifc = 0;
        void *got;
        if (rpending) { ioctl(u->fd, USBDEVFS_DISCARDURB, &rurb); ioctl(u->fd, USBDEVFS_REAPURB, &got); rpending = 0; }
        ioctl(u->fd, USBDEVFS_RELEASEINTERFACE, &ifc);
        close(u->fd);
    }
    u->fd = -1;
}
