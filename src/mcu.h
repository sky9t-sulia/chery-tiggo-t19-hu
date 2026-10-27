/* Link to the MCU (RH850) over /dev/ttymxc2 - what ShincoService does in the stock system.
 * Frame: 01 55 AA <seq> <cmd> 00 <len> <data...> <sum>  (sum = low byte of the sum of seq..data);
 * from the MCU - 01 AA 55 ...; receive acknowledgement: 06 55 AA <seq> 00 (ours) / 06 AA 55 <seq> 00 (MCU).
 * Port: 115200, 8 bits, odd parity. */
#ifndef MCU_H
#define MCU_H

int  mcu_open(void);                 /* 0 - port opened */
/* 1 before mcu_open - without start (0x00 / 0xFC / 0x08 / 0x33, 0xF1 / 0x0E): only the "I'm alive" keep-alive and brightness - the boot screen.
 * A full start early (huboot - at 6 s) - the MCU then did not send climate and cluster (0x51-0x53) until the end of boot,
 * and a start from hu no longer helped (in the car 2026-10-26); the stock boot screen also lights up without "give state" */
extern int mcu_light_only;
void mcu_poll(double now);           /* reading, acknowledgements, the "I'm alive" keep-alive every 5 s */
void mcu_close(void);
int  mcu_ok(void);                   /* port opened (on the HU) */
void mcu_set_brightness(int v);      /* screen backlight 1..10 (command 0x11 [04, v]) */

/* sound (NOTES_mcu.md) */
void mcu_set_volumes(int media, int phone);           /* 0x25 [media, phone, +3 bytes from the MCU state] */
void mcu_set_default_volume(int v);                   /* 0x14 [v] */
void mcu_set_eq(int bass, int treble, int middle, int mute);   /* 0x21, levels -7..+7 */
void mcu_set_speed_volume(int n);                     /* 0x23 [0..3, 00]: Off/Low/Middle/High */
void mcu_beep(void);                                  /* 0x24 [02 00]: key click (the sound is made by the MCU) */

/* radio */
/* audio source 0x15 [00, code] - audio processor input. The code is not the ShellApp SRC_Type number but its translation
 * (SourceControler::sendAudioSrcCmd, table in ShellApp; Ghidra analysis 2026-10-22). Call - 0x15: only with it
 * does the MCU switch the microphone on (with 0x09 - CarPlay music - the other party is audible, we are not). */
enum { MCU_SRC_RADIO = 0x01, MCU_SRC_USB1_AUDIO = 0x02, MCU_SRC_USB2_AUDIO = 0x03, MCU_SRC_USB1_VIDEO = 0x04,
       MCU_SRC_USB2_VIDEO = 0x05, MCU_SRC_BT_AUDIO = 0x06, MCU_SRC_PHONE = 0x15, MCU_SRC_OFF = 0x0F,
       MCU_SRC_CP_PHONE = 0x0A, MCU_SRC_SIRI = 0x0B };   /* SRC_CARPLAY_PHONE / _SIRI (ShellApp SourceControler::sendAudioSrcCmd) */
void mcu_source(int src);
void mcu_fastsave(const unsigned char d[12]);         /* 0x09: fast-save block - the MCU keeps it without NAND */
void mcu_navi_mix(int on, int vol, int duck);         /* 0x20: navigation mix-in (sound-navi), vol 0..10, duck - attenuate the source */

/* climate 0x5b [cmd, left t, right t, left °F, right °F, level, 0] (ACControler, NOTES_mcu.md); t - current ones from 0x51 */
enum { MCU_CLIM_ON = 0x61, MCU_CLIM_OFF = 0x60, MCU_CLIM_FAN = 0x20, MCU_CLIM_AC_ON = 0x51, MCU_CLIM_AC_OFF = 0x50,
       MCU_CLIM_RECIRC = 0x10, MCU_CLIM_FRESH = 0x11,   /* ShellApp "AirCircule" on (0x11) = fresh air */ MCU_CLIM_TEMP = 0xFF };
void mcu_climate(int cmd);                            /* command (MCU_CLIM_FAN | n - fan n) */
void mcu_climate_mode(int air);                       /* mode 0 face .. 4 windshield */
void mcu_climate_temp(int level);                     /* manual climate: level 1..16 */
void mcu_user_off(int on);                            /* 0x03 [on]: screen off/on from the HU side (like ShellApp changeUserOff) */
void mcu_camera(int on);                              /* camera by button (without R): 0x18 [01] / [00], like ShellApp */
void mcu_vehicle(int item, int value);                /* vehicle setting: 0x50 [item, value, FF FF FF] (ShellApp RRM1) */
void mcu_cluster_backlight(int level);                /* 0x53: cluster backlight 1..10 (time/date untouched) */
void mcu_cluster_time(int h, int m, int bl);          /* 0x53: clock h:m, backlight bl kept, seconds/date untouched */
void mcu_band(int am);                                /* 0x30 [07] FM / [08] AM */
void mcu_tune(int am, int f);                         /* 0x32: FM - units of 50 kHz, AM - kHz */
void mcu_seek(int up);                                /* 0x30 [05] up / [06] down */
void mcu_scan(void);                                  /* 0x30 [02] - the whole band -> list 0x33 */
void mcu_pick_found(int n);                           /* 0x30 [10 + n], n from 1 */
void mcu_pick_fav(int n);                             /* 0x30 [40 + n], n from 1 */
void mcu_fav_toggle(void);                            /* 0x30 [0c] - add the current station to favorites */
void mcu_set_region(int code);                        /* 0x36 [code] + request for lists 0x33 */
void mcu_fav_remove(int am, int n);                   /* 0x38 [band, 80 + n] - remove preset n (from 1) */

/* what the MCU reported (updated in mcu_poll; flags are cleared by the reader) */
typedef struct {
    int audio_new;                                    /* audio state arrived (0x12/0x16) */
    int media, phone, def, bass, treble, middle, mute, speed;
    int region, region_new;                           /* radio region: 0x12/0x16 byte 21, bits 2-3 */
    int tuner_new;                                    /* tuner state arrived (0x30) */
    int am, freq, preset, seeking;                    /* freq: FM - units of 50 kHz, AM - kHz */
    int scanning, idx;                                /* scanning (flag 0x20); idx: while scanning 1 + found */
    int lists_new;                                    /* lists 0x33/0x34 arrived */
    int fav[2][20], nfav[2];                          /* favorites [0 FM / 1 AM], frequencies in MCU units */
    int found[2][40], nfound[2];                      /* found by scanning */
    int keys[8], nkeys;                               /* steering wheel buttons 0x13: MCU_KEY_* code (| MCU_KEY_UP - released), the reader zeroes nkeys */
    int knob;                                         /* volume knob: clicks (+ right), the reader zeroes it */
    int clk_h, clk_m, clk_s, clk_new;                 /* car clock 0x53 [hh, mm, backlight, ss] (flag - reader) */
    int bl;                                           /* 0x53 byte 2: cluster backlight 1..10; -1 unknown */
    int ill;                                          /* 0x16 byte 17 bit 2: headlights on (ILL); -1 unknown */
    int steer, steer_ok;                              /* steering angle 0x58: ((b0 << 8 | b1) >> 4) - 2048, ~+-550 (+ left), like cameradetect */
    int user_off, user_off_new;                       /* 0x02: screen "off" via the knob (power icon); flag - reader */
    int power;                                        /* 0x50 bits 0-1: 0 off, 1 ACC, 2 ignition, 3 starter; -1 unknown */
    int door;                                         /* 0x50 bit 3: door open (one, the driver's) */
    int veh_new;                                      /* vehicle settings arrived (0x50 bits / 0x52); flag - reader */
    int lock_fb;                                      /* 0x50 raw[0]: lock confirmation 1=lights, 2=sound, otherwise lights+sound; -1 none */
    int auto_light_c, drl_c;                          /* 0x50 raw[1]: bit0 auto light (headlight delay), bit1 DRL; -1 none */
    int spd_alarm_c, spd_idx_c;                       /* 0x52 raw[1]: 0 off, otherwise index (km/h = idx*5+25); -1 none */
    int shutdown;                                     /* 0x04 [00]: the MCU will cut power in 5-10 s */
    int off;                                          /* 0x04 [01]/[00]: the MCU turned off the screen and sound; [02] - on again */
    int clim_new;                                     /* climate 0x51 arrived (flag cleared by the reader) */
    int clim_on, fan, ac, air, temp, recirc;          /* on; fan 0..7; A/C; mode 0..4; level 1..16; recirculation */
    unsigned char clim[8];                            /* the 0x51 frame itself - for command 0x5b */
    int fs_ok;                                        /* the MCU returned the fast-save block 0x09 (once - at start) */
    unsigned char fs[12];                             /* the block itself (12 bytes, what 0x09 returned last time) */
    int vspeed;                                       /* 0x57: speed, 16 bit (LE) x 0.05625 km/h; -1 none. Verified in the car
                                                       * 2026-10-26: maximum 342 = 19.2 km/h at ~20 on the speedometer */
} mcu_state;
/* steering wheel button codes (0x13 [01 pressed / 00 released, code]) */
enum { MCU_KEY_MODE = 0x20, MCU_KEY_VOL_UP = 0x21, MCU_KEY_VOL_DOWN = 0x22, MCU_KEY_PREV = 0x23, MCU_KEY_NEXT = 0x24,
       MCU_KEY_VOICE = 0x27, MCU_KEY_VOL_UP_LONG = 0x2D, MCU_KEY_VOL_DOWN_LONG = 0x2E, MCU_KEY_MUTE = 0x31,
       MCU_KEY_PHONE = 0x25, MCU_KEY_HOME = 0x2A,          /* panel buttons */
       MCU_KEY_PREV_LONG = 0x33, MCU_KEY_NEXT_LONG = 0x34, MCU_KEY_UP = 0x100 };
/* held: volume - the *_LONG code after 0.5 s, previous/next - after 2 s; no repeats,
 * the release already arrives with the *_LONG code */
mcu_state *mcu_st(void);

#endif
