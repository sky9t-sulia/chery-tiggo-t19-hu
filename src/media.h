/* USB stick: music / video / photo lists (background scanning), thumbnails and full photos (background decoding),
 * audio player (GStreamer 0.10 via dlopen; absent on PC - then there is no sound). */
#ifndef MEDIA_H
#define MEDIA_H

#include "gfx.h"

#include <stddef.h>
#include <stdint.h>

void media_poll(double now);            /* main loop: stick inserted/removed, scan finished, photos loaded */
int  media_gen(void);                   /* changes when the lists are updated */
int  media_scanning(void);
int  media_kind(const char *name);      /* 1 audio, 2 video, 3 photo, 0 - other (by extension) */

/* photo i: thumbnail (up to 320, cached on the stick in .thumbnails) / full (up to 1280); NULL - still loading */
gfx_tex *media_thumb(int i);
gfx_tex *media_full(int i);
gfx_tex *media_art(int track);
int media_wallpaper(gfx_tex *t, int i, col overlay);   /* wallpaper from photo i (theme dimming on pixels) */
int media_wallpaper_file(gfx_tex *t, col overlay);     /* saved "From USB" wallpaper (data/wallpaper.jpg) */
int media_wallpaper_save(int i);                        /* photo i to screen size -> data/wallpaper.jpg; -1 - none */
gfx_tex *media_wallpaper_thumb(void);                   /* thumbnail of the saved wallpaper; NULL - none */         /* track cover art (APIC from MP3); NULL - none or loading */

/* audio player (one file at a time), player.c */
int    player_open(const char *path);   /* stop the previous one and play this; -1 - no GStreamer */
void   player_pause(int pause);
void   player_stop(void);
void   player_volume(double v);         /* 0..1 - stick and video (quieter under the phone audio) */
void   player_seek(double sec);
double player_pos(void);                /* seconds; -1 - unknown */
double player_dur(void);
int    player_eos(void);                /* track finished (cleared on read) */

/* video (player.c): VPU -> GPU texture without copying; the movie audio is separate */
int    vplayer_open(const char *path);
void   vplayer_pause(int pause);
void   vplayer_stop(void);
void   vplayer_close(void);             /* stop and without the last frame (finished watching) */
void   vplayer_seek(double sec);
double vplayer_pos(void);
double vplayer_dur(void);
int    vplayer_eos(void);               /* end or error (to the log) */
int    vplayer_new_frame(void);         /* a new frame arrived - redraw */
void   vplayer_poll(double now);
double vplayer_idle(double now);        /* seconds without new frames */        /* main loop: return the VPU frames if there are no new ones (end of file) */
gfx_tex *vplayer_frame(float *u1, float *v1, int *w, int *h);   /* current frame (to draw), NULL - none */
int    vplayer_unsupported(void);       /* frames not from the VPU (software decoder) - not shown, "Can't open" */

/* Android Auto (player.c): streams from the phone into appsrc; on PC without GStreamer 1.0 - -1 / nothing */
int    vstream_open(void);              /* H.264 -> VPU; frames - via vplayer_frame (1280x720), stick video stops */
int    vstream_push(const void *h264, size_t n, int64_t pts_us);   /* whole frame (byte-stream); pts < 0 - no timestamp */
void   vstream_close(void);
int    astream_open(int id, int rate, int channels);   /* 0 media 48000/2, 1 navigation 16000/1, 2 system 16000/1 */
int    astream_open_dev(int id, int rate, int channels, const char *dev);   /* dev - ALSA device (NULL - default) */
int    astream_push(int id, const void *pcm, size_t n);   /* S16LE */
void   astream_volume(int id, double v);  /* 0..1 (attenuation) */
void   astream_close(int id);
void   player_shutdown(void);

#endif
