/* Internal UI API: shared state, dirty-area tracking, touches, fonts, components, screens. */
#ifndef UI_INT_H
#define UI_INT_H

#include "anim.h"
#include "gfx.h"
#include "icons.h"
#include "text.h"
#include "theme.h"

#include <stddef.h>

/* ---------------------------------------------------------------- dirty areas (design px) */
void damage_d(float x, float y, float w, float h);
void damage_full(void);
void screen_dirty(void);           /* redraw the cache layer of the current screen */
void base_dirty(void);             /* redraw the bottom cache layer */
/* clip (design px) inside the current redraw area - for scrollable lists */
void clip_push(float x, float y, float w, float h);
void clip_pop(void);

/* settings kept across reboots (settings.c): files in /mnt/data1/hu ($HU_DATA) */
void data_path(char *out, size_t n, const char *name);
int  data_dir(void);                                   /* create the directory; -1 - not possible */
int  data_commit(const char *tmp, const char *dst);   /* fsync + rename */
void settings_load(void);                              /* in ui_init after the defaults */
void settings_tick(double now, int playing);           /* every frame; playing - music from the USB stick is playing */
void settings_flush(int playing);                      /* write now (going to sleep) */
void settings_shutdown(int playing);                   /* MCU turned everything off (0x04 [01]): + track and position (once) */
void settings_shutdown_cancel(void);
int  settings_resume_usb(void);                         /* the USB stick was playing at shutdown */

/* ---------------------------------------------------------------- touches */
#define HIT_MAX 256
#define HIT_SCREEN 40
#define HIT_FS 28                  /* 28..37 - fullscreen video (on top of everything) */              /* id >= HIT_SCREEN - buttons of the current screen (cleared on change) */
void  set_hit(int id, float x, float y, float w, float h);     /* button (design px) */
void  set_drag(int id, float x, float y, float w, float h);    /* slider: the screen receives drag() */
void  set_scroll(int id, float x, float y, float w, float h);  /* scrollable area: drag() when moved > 8px */
void  clear_screen_hits(void);
float press_k(int id, double now);                             /* 1 .. PRESS_SCALE (scale 0.93) */
void  hw_press(int id, int down, double now);                  /* press of a screen button from the steering wheel: animated like a finger */
float press_dim(float k);                                      /* 0 .. PRESS_DIM (brightness 0.88) */
int   press_active(double now);
int   held_id(void);                                           /* which button is currently held (-1 none) */
double held_since(void);
void  cancel_press(double now);                                /* cancel the current press (long press fired) */

/* ---------------------------------------------------------------- fonts (weight, size in design px) */
font *F(int weight, float size);

/* ---------------------------------------------------------------- drawing helpers (design px) */
void  t_left(int w, float size, float x, float cy, const char *s, col c);    /* vertically centered on cy */
void  t_right(int w, float size, float xr, float cy, const char *s, col c);
void  t_center(int w, float size, float cx, float cy, const char *s, col c);
void  t_top(int w, float size, float x, float y, const char *s, col c, float ls);   /* y - top of the line */
void  t_fit(int w, float size, float x, float cy, float maxw, const char *s, col c); /* with an ellipsis */
float t_width(int w, float size, const char *s);
int   marquee(int w, float size, float x, float y, float maxw, const char *s, col c, float ls, double t0, double now, int dyn);   /* marquee */
float t_lh(int w, float size);                                               /* line-height: normal */
float t_wrap_center(int w, float size, float cx, float y, float maxw, const char *s, col c);   /* word wrap */
void  fmt_time(char *b, size_t n, double sec);
void  panel(float x, float y, float w, float h, float r);                     /* glass panel + theme shadow */
void  ico(const char *name, float cx, float cy, float size, col c);
void  ico_rot(const char *name, float cx, float cy, float size, col c, float deg);
int   tile_sq(float w, float h, float r, float *rr);   /* design's square tile (v35) -> squircle and its radius */
void  rrect(float x, float y, float w, float h, float r, col c);
void  rrect_b(float x, float y, float w, float h, float r, col fill, col border);
void  shadow(float x, float y, float w, float h, float r, float blur, float ox, float oy, col c);
float press_box(int id, float *x, float *y, float *w, float *h, double now);  /* scale about the center */
void  art_draw(const gfx_tex *t, float x, float y, float s, float r);   /* cover into square s: fills it, the excess is cropped about the center */
void  live_row(float x, float cy, int on);
/* background from a blurred cover (dashboard music card, Music screen): update - outside drawing (reads the GPU), 1 - changed;
 * key - which cover it is (track + texture), 0 - no cover (plain glass) */
typedef struct { gfx_tex t; long key; } art_bg;
int   art_bg_update(art_bg *b, const gfx_tex *art, long key);
void  art_bg_draw(const art_bg *b, float x, float y, float w, float h, float r);   /* live (design dLive): LIVE badge + Streaming / Paused, height 44, cy - middle */

/* ---------------------------------------------------------------- design components */
/* segmented switch: pad - inner padding (4 normal, 3 small), border - c.border frame */
void seg_static(float x, float y, int n, float iw, float ih);
void seg_static2(float x, float y, int n, float iw, float ih, float pad, int border);
void  scpy(char *d, size_t n, const char *s);
void *grow(void *p, int *cap, int need, size_t sz);   /* realloc with headroom (x2); NULL - out of memory */   /* truncated copy (whole UTF-8 characters) */
extern const float *seg_item_op;         /* for the next seg_dyn: item opacity */
void seg_dyn(float x, float y, int n, float iw, float ih, float thumb, const char *const *labels,
             const char *const *icons, int weight, float size, float icon_size, int base_id, double now);
void seg_dyn2(float x, float y, int n, float iw, float ih, float pad, int border, float thumb,
              const char *const *labels, int weight, float size, int base_id, double now);
void toggle_draw(float x, float y, float on);                  /* toggle 76x44, on 0..1 */
void toggle_draw_c(float x, float y, float on, col on_col);    /* color when on (mute - red) */
/* design slider: track h40 r20 c.track, fill c.text up to frac, icon on the left (difference) */
void slider_draw(float x, float y, float w, float frac, const char *icon, col fill);
float slider_value(float x, float w, float px);                /* 0..1 from the touch x */
/* header buttons: "close" 52 circle c.inner, "Reset to defaults" (c.track frame) */
void close_btn(int id, float x, float y, double now);
float reset_w(void);
void reset_btn(int id, float x, float y, double now);

void head_static(float x, float y, uint32_t color, const char *icon, float isz, const char *title);
/* v26 screen header: tile 42 (r12, icon 22) + 28/800, centered on cy; returns x after the header */
float screen_head(float x, float cy, uint32_t color, const char *icon, const char *title, col fg);
void dash_rect(float x, float y, float w, float h, float r, col c);          /* 2px dashed */
void row_text(float x, float cy, float maxw, const char *title, const char *sub);
float slider_row(float x, float y, float w, const char *label, const char *val, float frac, const char *icon, col fill);
float slider_row_m(float x, float y, float w, float gap, const char *label, const char *val, float frac, const char *icon, col fill);
void seg_plain(float x, float y, int n, float iw, float ih, float pad, float thumb, const char *const *labels,
               int size, int base_id, double now);

/* scrolling with inertia */
typedef struct { float off, max, v; int drag; float last_y; double last_t; } scroller;
void sc_drag(scroller *s, float y, int phase, double now);    /* phase 0 down, 1 move, 2 up */
int  sc_update(scroller *s, double now);                       /* 1 - offset changed */

/* ---------------------------------------------------------------- shared application state */
enum { S_DASH, S_APPS, S_MUSIC, S_VIDEO, S_RADIO, S_CAMERA, S_DISPLAY, S_PHONE,
       S_USB, S_BT, S_SOUND, S_VEHICLE, S_LUA, S_COUNT };

/* media from the USB stick (media.c): filled by scanning, empty without a stick */
/* path - full path (on the heap, any length), name - file name inside path */
typedef struct { char t[160], a[96], al[64]; int len; uint32_t hue; double added; char *path; long art_off, art_len; } track_t;   /* added - unix time; art - cover (APIC) in the file */
typedef struct { const char *name; int len; long long size; char *path; int w, h; } video_t;   /* w/h - from the mp4 (0 - unknown) */
typedef struct { const char *name; long long size, mtime; char *path; } image_t;
extern track_t *TRACKS;                  /* arrays grow as scanning proceeds - no limits */
extern video_t *VIDEOS;
extern image_t *IMAGES;
extern int NTRACKS, NVIDEOS, NIMAGES;
/* safe access: outside the list (stick is being scanned / empty) gives an empty entry, not NULL */
static inline track_t *trk_at(int i) { static track_t z = { .path = (char *)"" }; return i >= 0 && i < NTRACKS ? &TRACKS[i] : &z; }
static inline video_t *vid_at(int i) { static video_t z = { .name = "", .path = (char *)"" }; return i >= 0 && i < NVIDEOS ? &VIDEOS[i] : &z; }
int  music_usb_av(void);                 /* USB stick with music (v15 usbAv: connected and not empty) */
int next_track(void);                    /* next (honoring shuffle) - in playlist order */
int prev_track(void);
const int *playlist(void);               /* TRACKS indices in display order (A–Z or Date, A.pl_date) */
int playlist_pos(int trk);               /* position of the track in this order */
/* "now playing" of the selected source: USB - HU player (TRACKS), Bluetooth - phone (AVRCP) */
typedef struct { const char *t, *a, *al; int len, playing, id, av, idle, live; double pos; uint32_t hue; } np_t;   /* av - source connected; idle - phone connected but no track (v15); live - live stream, no duration */
int  music_dash_play(void);              /* v15 dashPlay: something was started and a source exists - otherwise the "Nothing playing" card */
const char *bt_phone_name(void);
int  music_bt_av(void);                  /* phone connected over BT */
int  music_any(void);                    /* there is at least one source (otherwise the "No music source" screen) */
void usb_eject(double now);              /* unmount the USB stick (Eject button in USB Files) */
np_t np_now(double now);
gfx_tex *np_aa_art(void);                /* track cover from Android Auto (or NULL) */
void np_play(double now);                /* play/pause */
void np_next(double now);
void np_prev(double now);
void np_seek(double sec);                /* USB: seek */
void np_mode(void);                      /* shuffle/repeat changed - for Bluetooth, send to the phone */
void np_source(int usb, double now);

typedef struct { char id[18]; char name[64]; int kind; } btdev;
typedef struct { char name[64]; char num[3][24]; char kind[3]; int nnum; } pb_contact;   /* kind: 'm' mobile, 'h' home, 'w' work, 'o' other */
typedef struct { char name[64]; char num[24]; int dir; long long t; } pb_call;          /* dir: 0 missed, 1 dialed, 2 received; t - YYYYMMDDhhmm */
extern pb_contact *PB;                   /* grow while parsing vCards */
extern pb_call *REC;
const char *pb_name_of(const char *num);   /* name by number (or NULL) */
const char *pb_kind_of(const char *num);   /* "Mobile" / "Home" / "Work" by number from contacts */
uint32_t person_color(const char *name);   /* avatar color from the design palette (by name) */
void person_initial(char *out, size_t n, const char *name);   /* first letter (UTF-8) or "#" for a number */   /* id - address AA:BB:..; kind: 0 phone, 1 tablet, 2 speaker, 3 laptop */

typedef struct {
    int light, scene;                     /* theme; scene 0..N_SCENE-1, SCENE_CUSTOM - photo from USB */
    int theme_auto;                       /* v30 Auto theme: night (19..7) or headlights -> dark (theme_auto_sync) */
    int custom_img;                       /* which demo photo is set as wallpaper (-1 none) */
    int bright;                           /* brightness 1..10 */
    /* music */
    int playing, trk, shuffle, repeat, src_usb;
    int m_loaded;
    int pl_date;                          /* USB playlist: 0 - A–Z, 1 - Date */                         /* v15 mLoaded: music has already been started (USB play or the phone started playing) */
    double pos;
    tween eq[4], art_sc;
    /* video */
    int vsel, vplaying, vfull;
    volatile int aa_media, aa_mic, aa_voice, aa_vs, aa_cm, aa_navi, aa_conn;   /* aa_cm - hands-free: microphone open at 16 kHz */           /* aa_mic - microphone for the AA assistant (DSP in recognition mode) */                   /* Android Auto is playing music (media channel opened by the phone) */
    double vpos, vctl_t;
    int vdrag;                            /* video slider is being dragged: seek happens on release */               /* vctl_t - when the fullscreen panel was last touched */
    /* radio (design v7; data from the MCU, NOTES_mcu.md) */
    int am;
    float fm_f, am_f;
    int radio_on;
    int region;                  /* radio region: MCU code 0x36 (0 Asia, 1 Europe, 2 USA, 3 Latin America) */
    float fav[2][20]; int nfav[2];              /* favorites [0 FM / 1 AM], in MHz / kHz */
    float found[2][40]; int nfound[2];          /* found by scanning */
    int found_known[2];                         /* a scan has been done (otherwise "Not scanned yet") */
    int scanning, seeking, seek_dir, scan_n;    /* scan_n - found so far during the scan */
    float scan_f;                               /* frequency the scan/seek is currently at */
    /* camera */
    int wide, guides;
    /* phone */
    char dial[20];
    int ph_contacts, mute, speaker;
    int call_on; double call_t; char call_name[24]; uint32_t call_col;
    /* sound */
    int vol, muted, phone_vol, def_vol, speed_vol, key_tone;
    int eqv[3];
    /* bluetooth */
    int bt_on, disc, searching, found_n, pairing_i;
    double disc_t, search_t, pair_t, pend_t;
    btdev paired[8]; int n_paired;
    int usb_conn;                /* USB stick is mounted (/media/usb*) */
    char usb_mnt[256];
    float usb_free_gb, usb_total_gb;   /* free / total on the USB stick */
    int net;                     /* internet: phone in USB tethering mode, the interface has IPv4 (tools/net-usb.sh) */
    char net_dev[32];            /* phone name from USB (product) */
    btdev extra[8]; int n_extra;
    char bt_conn[18], bt_pending[18], pairing[18];
    int ph_batt, ph_sig;                  /* phone battery and signal via HFP: 0..5, -1 - no data */
    /* music from the phone (AVRCP) */
    int bt_av;                            /* AVRCP connected */
    char bt_title[160], bt_artist[96], bt_album[96];
    int bt_len, bt_playing;               /* seconds; 1 - playing */
    int bt_stream;                        /* A2DP stream open: the phone is playing something (navigation, notification, music) */
    double bt_pos, bt_pos_t;              /* position (s) and when it arrived - between events the position advances by itself */
    int bt_seq;                           /* changes when the track changes */
    int bt_call;                          /* HFP call: 0 none, 1 incoming ringing, 2 outgoing, 3 in conversation */
    char bt_call_num[32];
    double bt_call_t;                     /* when the conversation started (state 3) */
    int bt_sco;                           /* voice goes through the car (SCO) */
    /* contacts and call log from the phone (PBAP -> vCard, bt_nfore.c) */
    int pb_state;                         /* 0 none, 1 loading, 2 ready */
    int n_cont, n_rec;
    /* car */
    int auto_light, drl, lock_lh, speed_alarm, speed_limit, lang, backlight, vt_h, vt_m;
    int car_live;                         /* ignition (MCU 0x50 power 2/3) or the MCU doesn't know yet: climate and Vehicle work */
    double veh_touch;                     /* time of the last Vehicle setting edit (do not overwrite with the MCU reply) */
    /* USB */
    int usb_path;                         /* 0 root, 1 music, 2 video, 3 images, 4 other */
    int other_path[4], other_depth;
    int usb_img, usb_last;
    int app_run; double app_t; char app_name[64];
    /* dashboard widget */
    int widget;
    /* what was before the current screen (for "close") */
    int prev_screen;
} app_t;
extern app_t A;

/* screen */
typedef struct {
    void (*layout)(void);                 /* register the buttons */
    void (*draw_static)(void);            /* static -> screen cache layer */
    void (*draw_dyn)(double now);         /* moving and pressable (over the layer) */
    void (*update)(double now);           /* animations/time -> damage_d */
    void (*click)(int id, double now);
    void (*enter)(double now);            /* screen opened */
    void (*drag)(int id, float x, float y, int phase, double now);   /* sliders/scrolling (design px) */
    /* steering-wheel previous/next button (d -1/+1; held - being held) -> screen button (press animation and click), -1 - none */
    int (*hw_btn)(int d, int held);
} screen_def;

extern const screen_def SCR_DASH, SCR_APPS, SCR_MUSIC, SCR_VIDEO, SCR_RADIO, SCR_CAMERA, SCR_DISPLAY,
                        SCR_PHONE, SCR_SETTINGS, SCR_USB, SCR_VEHICLE, SCR_LUA;
void standby_palette(void);              /* blob colors - from the current wallpaper (standby.c) */
void standby_draw(double at, float op, float glow, float clk);   /* at - seconds since appearance */
extern float sb_btn[2];                    /* "Wake screen" button on Screen off: top-left, 72x72 */
const char *ui_assets(void);              /* resources folder (HU_ASSETS) */
void go_screen(int screen, double now);
void close_screen(double now);           /* "close" button: back to the previous screen or to Apps */
int  cur_screen(void);
int  any_active(const tween *t, int n, double now);
void set_theme(int light, double now);
void theme_auto_apply(double now);                                 /* Auto: set the theme right away from the clock/headlights */
void set_scene(int scene, double now);
void set_accent(int k);
void set_custom_wallpaper(int img, double now);
void set_bright(int b);
void set_volume(int v, double now);      /* media volume 0..31 (clears mute), HUD */
void toggle_muted(double now);
void layout_again(void);                 /* rebuild the touch zones of the current screen */
/* HUD: kind 0 volume, 1 temperature, 2 air, 3 "Sound: Defaults restored", 4 Display, 5 Vehicle,
 * 6 "Wallpaper: Updated", 7 "Can't open: <text>" */
void hud_show(int kind, const char *text, double now);
void app_run(int lua_app, double now);   /* "Running app" window: Lua program lua_apps_get(i) */
void display_shutdown(void);
void usb_shutdown(void);
const char *station_name(char *b, size_t n);
void  radio_fmt(char *b, size_t n, float v);
int   radio_fm_div(void);                /* MCU FM units per 1 MHz: 20 (Europe) / 10 */
void  radio_set_region(int code);        /* region (MCU code) without a command: recompute frequencies */
void  radio_region_sent(void);           /* core: the region was changed - don't retune the frequency */   /* frequency of the current band: 1008 / 87.55 / 101.5 */
float radio_frac(void);                  /* scale needle position 0..1 */
int   radio_is_fav(void);
void  radio_fav_toggle(double now);
void  radio_jump(int d);                 /* -1/+1: through favorites / found / seek */   /* radio caption on the Dashboard (scr_radio.c): RDS name or "Radio off" */
/* fullscreen video (scr_video.c), called by the core */
void video_fs_layout(double now);
void video_fs_update(double now);
void video_fs_draw(double now);
int  video_fs_covers(double now);
/* Android Auto (aa_view.c) */
void aa_view_open(double now);           /* Apps tile: start a session / give the screen back to the phone */
int  aa_view_covers(void);               /* AA fullscreen */
void aa_view_update(double now);
void aa_view_draw(double now);
void aa_view_touch(int type, float x, float y);   /* PF_DOWN/MOVE/UP, design coordinates */
void aa_view_key(int d);                 /* steering wheel: +1 next, -1 previous */
int  aa_view_voice(void);
void aa_view_exit(void);
void aa_view_resume(void);                      /* screen back to the phone (after reverse), if the session is alive */
void aa_view_phone(void);                /* steering wheel, phone button - to AA */                 /* Home button: from AA to the shell */                /* steering wheel, voice button: AA assistant (0 - no session) */
void aa_view_shutdown(void);
int  video_loaded(void);
int  audio_loaded(void);                 /* track from the USB stick is open (BT - always 1) */                 /* video is open in the player (otherwise time shows -:--) */        /* fullscreen video is fully open (opaque) - don't draw beneath it */
void video_fs_click(int id, double now);
void video_fs_drag(int id, float x, float y, int phase, double now);              /* demo photo (loaded on first access) */

/* screen area (design px) */
/* wallpapers (v26: 11 scenes + photo from USB) */
#define N_SCENE 11
#define SCENE_CUSTOM N_SCENE
extern const char *const SCENE_FILE[N_SCENE], *const SCENE_NAME[N_SCENE];

#define ST_X 122
#define ST_Y 18
#define ST_W 1140
#define ST_H 636
/* v26: all panels have padding 24 - content (border 1 + 24) */
#define PX0 147.0f
#define PY0 43.0f
#define PCW 1090.0f
#define PBOT 629.0f

#endif
