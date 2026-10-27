/* Android Auto: USB part (usbfs, no libusb). Phone -> accessory mode (AOA), then bulk IN/OUT.
 * On the HU - bus 1 only (CarPlay port, OTG ci_hdrc.0): bus 2 permanently hosts the phone for network/SSH, we do not touch it.
 * HU_AA_BUS=N - another bus (on PC: 0 - any). */
#ifndef AA_USB_H
#define AA_USB_H

#include <stddef.h>

typedef struct {
    int fd;                          /* /dev/bus/usb/BBB/DDD of the accessory; -1 - none */
    unsigned ep_in, ep_out;          /* bulk */
    int bus, dev;
} aa_usb;

/* Find the phone on our bus: already an accessory (18d1:2d00..2d05) - open it; otherwise Android - switch to AOA
 * (1 - switched, wait for the new device), 0 - accessory opened, -1 - no phone/error. */
int  aa_usb_find(aa_usb *u);
int  aa_usb_read(aa_usb *u, void *buf, size_t n, int timeout_ms);   /* bytes or -1 (timeout: 0) */
int  aa_usb_write(aa_usb *u, const void *buf, size_t n, int timeout_ms);
void aa_usb_close(aa_usb *u);
/* there is a phone in the CarPlay port (without opening, sysfs only): apple 1 - iPhone, 0 - any other (Android) */
int  aa_usb_phone(int apple);

#endif
