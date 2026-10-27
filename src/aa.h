/* Android Auto (wired, AOA): phone session in its own thread. Protocol - NOTES_aa.md.
 * Video: H.264 1280x720 30 fps, the phone draws in the center 1024x600 (margins 256x120); audio - 3 PCM streams. */
#ifndef AA_H
#define AA_H

#include <stddef.h>
#include <stdint.h>

enum { AA_IDLE, AA_SEARCH, AA_CONNECT, AA_RUN, AA_NATIVE };   /* AA_NATIVE - phone gave the screen back (exit to the shell) */
enum { AA_TOUCH_DOWN = 0, AA_TOUCH_UP = 1, AA_TOUCH_MOVE = 2 };

typedef struct {
    void (*video)(const uint8_t *h264, size_t n, int64_t ts_us);   /* frame (Annex-B), ts < 0 - no timestamp */
    void (*audio)(int id, const uint8_t *pcm, size_t n);          /* 0 music 48k/2, 1 navigation 16k/1, 2 system 16k/1 */
    void (*audio_ctl)(int id, int start);                         /* stream started/ended */
    void (*mic)(int open);                                        /* phone opened/closed the microphone (assistant) */
    void (*voice)(int on);                                        /* voice session (assistant, Gemini Live): start/end */
} aa_sinks;

int  aa_start(const aa_sinks *s);        /* thread: find the phone (CarPlay port), AOA, session; 0 - started */
void aa_stop(void);                      /* say bye-bye and close */
int  aa_state(void);
int  aa_wireless(void);                  /* session over Wi-Fi (the same phone as on Bluetooth) */
void aa_touch(int action, int x, int y); /* screen coordinates 1024x600 */
void aa_key(int code, int down);         /* Android KeyEvent: 0x54 voice (assistant), 0x55 play/pause, 0x57 next, 0x58 prev. */
void aa_night(int on);
typedef struct { int valid, seq, art_seq, state, pos, dur; double pos_t; char song[160], artist[96], album[96]; } aa_np_t;
int  aa_np(aa_np_t *out);                /* what is playing on the phone (AA); 1 - present and session running. state: 2 playing, 3 paused */
unsigned char *aa_np_art(int *w, int *h);    /* new RGBA cover art (take it and release with free) or NULL */
void aa_speed(int mm_s);                 /* car speed, mm/s (CAR_SPEED sensor) */
void aa_car(int steer_valid, int steer_deg, int lights);   /* steering angle (MCU, + left) and headlights (-1 unknown) - AA sensors */
void aa_focus(int projected);
void aa_audio_loss(void);
void aa_mic_send(const void *pcm, size_t n);   /* microphone audio 16 kHz / 16 bit / mono (microphone stream) */                /* own source (radio, USB stick) - to the phone: audio taken away, pause */            /* 1 - show AA, 0 - own screen (reverse, exit to the shell) */

#endif
