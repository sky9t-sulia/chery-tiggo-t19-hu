/* Wireless Android Auto: the phone learns the HU network over Bluetooth (RFCOMM) and connects to it over TCP 5288 - then
 * the same AA protocol as over USB (aa.c). The dongle driver and the access point (hostapd) are brought up by wifi.sh at boot, the AP does not
 * go down (like WirelessAndroidAutoDongle). */
#ifndef AA_WIFI_H
#define AA_WIFI_H

void aa_wifi_init(void);     /* threads: RFCOMM server (after BT is switched on) and TCP 5288 */
int  aa_wifi_pending(void);  /* phone connected over TCP - session is waiting */
int  aa_wifi_phone(void);    /* phone on RFCOMM, no session (it disconnected AA itself) - AA tile is available */
void aa_wifi_call(void);     /* AA tile: call the phone again (WifiStartRequest) */
int  aa_wifi_take(void);     /* take the connection socket (aa.c); -1 - none */
void aa_wifi_done(void);     /* Wi-Fi session ended (aa.c) */
void aa_wifi_pause(int on);  /* car is off (ACC 0, MCU turned the screen off): do not start new sessions */
int  aa_wifi_lost(void);     /* phone is gone (disconnected from the AP or over Bluetooth) - close the session immediately (aa.c) */
void aa_wifi_poll(double now);   /* main loop: the wireless-session phone disappeared from Bluetooth - close */

#endif
