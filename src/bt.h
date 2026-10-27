/* Bluetooth: on the HU - the stock libbluelink stack (nFore, NOTES_bt.md) in bt_nfore.c; on PC - bt_none.c (no BT).
 * Library events (from its threads) are queued, bt_poll() in the main loop moves them into the interface state.
 * A device is identified by id - the address "AA:BB:CC:DD:EE:FF". */
#ifndef BT_H
#define BT_H

int  bt_init(void);                      /* start the stack and switch the module on (in its own thread); -1 - no BT */
void bt_poll(double now);
void bt_power(int on);
void bt_discoverable(int on);            /* visibility for pairing */
void bt_search(int on);                  /* device search */
void bt_pair(const char *id);
void bt_connect(const char *id);
void bt_disconnect(const char *id);
void bt_remove(const char *id);          /* remove pairing */
/* music from the phone (AVRCP): 0 stop, 1 play, 2 pause, 3 next, 4 prev */
void bt_media(int op);
void bt_media_mode(int shuffle, int repeat);   /* phone player modes (AVRCP player settings) */
/* calls (HFP) */
void bt_dial(const char *number);
void bt_answer(void);
void bt_reject(void);
void bt_hangup(void);
void bt_mic_mute(int on);
void bt_audio_car(int car);             /* call audio: 1 - to the car, 0 - to the phone */
void bt_a2dp_mute(int on);              /* mute the phone audio (A2DP) on the HU: Bluetooth not selected */
void bt_dtmf(char c);
/* RFCOMM server with its own 128-bit UUID (wireless Android Auto): SDP record + accepting connections. Socket: first
 * int channel (4 bytes), then for each connection a 16-byte recvmsg with the connection fd in SCM_RIGHTS (bt_rfcomm_accept).
 * Only after the module is switched on (A.bt_on). -1 - failed */
int  bt_rfcomm_listen(const unsigned char uuid[16], const char *name);
int  bt_linked(const unsigned char addr[6]);   /* device is connected over Bluetooth (at least one profile) */

#endif
