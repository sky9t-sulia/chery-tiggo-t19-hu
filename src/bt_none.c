/* Bluetooth on PC: no module - the interface shows "Bluetooth is off". */
#include "bt.h"

int  bt_init(void) { return -1; }
void bt_poll(double now) { (void)now; }
void bt_power(int on) { (void)on; }
void bt_discoverable(int on) { (void)on; }
void bt_search(int on) { (void)on; }
void bt_pair(const char *id) { (void)id; }
void bt_connect(const char *id) { (void)id; }
void bt_disconnect(const char *id) { (void)id; }
void bt_remove(const char *id) { (void)id; }
void bt_media(int op) { (void)op; }
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
