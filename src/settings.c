/* Settings kept across HU reboots: /mnt/data1/hu (NAND data partition - the stock shell also keeps params.dat and
 * Bluetooth pairings there; the root is read-only). On the PC: $HU_DATA (if unset, nothing is saved).
 *  - settings: key=value; snapshot once a second, written if changed (temp file + fsync + rename: if power is
 *    cut mid-write, the previous file remains);
 *  - track, position, whether it was playing - not in NAND but in the MCU (owner: "like in the MCU, don't write to NAND needlessly"): fast-save block
 *    0x09, like ShellApp FastSaving - once a second while the USB stick plays; the MCU keeps it during sleep and on
 *    power loss, and returns it at startup. The track is identified by a path hash (numbers change on scanning) and is applied
 *    after the USB stick is scanned. The file (track/pos/playing, old versions) is used only if there is no block from the MCU;
 *  - "From USB" wallpaper - wallpaper.jpg (media.c), the USB stick is not needed after selection.
 * Radio, volumes, equalizer, Vehicle settings (except language) are remembered by the MCU - not here. */
#include "ui_int.h"
#include "theme.h"
#include "mcu.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char trk_path[1024];              /* track in the file (from the previous shutdown) */
static double trk_pos;
static int trk_play, trk_pending;        /* pending - not yet found on the USB stick */
static int shut_done;
static int fs_tried;                     /* MCU block already checked */
static unsigned char fs_sent[12];        /* what was last sent to the MCU */

/* block 0x09 (12 bytes, as in ShellApp: 2 x u32, seconds u16 LE, flags, 0): path hash, "TGHU" tag (our block, not the
 * stock one), seconds, flags (bit 0 - was playing) */
static uint32_t path_hash(const char *s) { uint32_t h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }
static const unsigned char FS_MARK[4] = { 'T', 'G', 'H', 'U' };
static char last[2048];
static double tick_t;

void data_path(char *out, size_t n, const char *name)
{
    const char *d = getenv("HU_DATA");
    snprintf(out, n, "%s/%s", d ? d : "/mnt/data1/hu", name);
}

int data_dir(void)
{
    char d[512];
    struct stat st;
    data_path(d, sizeof d, "");
    if (!stat(d, &st)) return S_ISDIR(st.st_mode) ? 0 : -1;
    return mkdir(d, 0755);
}

/* tmp written - flush to disk and move into place as dst */
int data_commit(const char *tmp, const char *dst)
{
    int fd = open(tmp, O_RDONLY);
    if (fd < 0) return -1;
    fsync(fd); close(fd);
    if (rename(tmp, dst)) { unlink(tmp); return -1; }
    return 0;
}

void settings_load(void)
{
    char p[512], l[1100], *v;
    FILE *f;
    int n;
    data_path(p, sizeof p, "settings");
    if (!(f = fopen(p, "r"))) return;
    while (fgets(l, sizeof l, f)) {
        if (!(v = strchr(l, '='))) continue;
        *v++ = 0; v[strcspn(v, "\n")] = 0; n = atoi(v);
        if (!strcmp(l, "light")) A.light = n != 0;
        else if (!strcmp(l, "theme_auto")) A.theme_auto = n != 0;
        else if (!strcmp(l, "scene") && n >= 0 && n <= SCENE_CUSTOM) A.scene = n;
        else if (!strcmp(l, "accent") && n >= 0 && n < N_ACCENT) theme_accent = n;
        else if (!strcmp(l, "bright") && n >= 1 && n <= 10) A.bright = n;
        else if (!strcmp(l, "lang") && n >= 0 && n <= 2) A.lang = n;
        else if (!strcmp(l, "src_usb")) A.src_usb = n != 0;
        else if (!strcmp(l, "shuffle")) A.shuffle = n != 0;
        else if (!strcmp(l, "repeat")) A.repeat = n != 0;
        else if (!strcmp(l, "pl_date")) A.pl_date = n != 0;
        else if (!strcmp(l, "radio_on")) A.radio_on = n != 0;
        else if (!strcmp(l, "wide")) A.wide = n != 0;
        else if (!strcmp(l, "guides")) A.guides = n != 0;
        else if (!strcmp(l, "widget") && n >= 0) A.widget = n % 2;   /* dashboard card: Connections / Photos */
        else if (!strcmp(l, "key_tone")) A.key_tone = n != 0;
        else if (!strcmp(l, "playing")) trk_play = n != 0;
        else if (!strcmp(l, "pos")) trk_pos = atof(v);
        else if (!strcmp(l, "track")) snprintf(trk_path, sizeof trk_path, "%s", v);
    }
    fclose(f);
    trk_pending = trk_path[0] != 0;
    fprintf(stderr, "hu: settings: loaded%s%s\n", trk_pending ? ", track " : "", trk_path);
}

static void save(const char *s)
{
    char p[512], tmp[520];
    FILE *f;
    if (data_dir() < 0) return;
    data_path(p, sizeof p, "settings");
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    if (!(f = fopen(tmp, "w"))) return;
    fputs(s, f);
    if (fclose(f) || data_commit(tmp, p)) { unlink(tmp); fprintf(stderr, "hu: settings: write failed\n"); }
}

/* playing - whether music from the USB stick was playing */
static void snap(char *b, size_t n, int playing)
{
    (void)playing;                               /* track and position go to the MCU (0x09), not to the file */
    snprintf(b, n, "light=%d\ntheme_auto=%d\nscene=%d\naccent=%d\nbright=%d\nlang=%d\nsrc_usb=%d\nshuffle=%d\nrepeat=%d\n"
             "pl_date=%d\nradio_on=%d\nwide=%d\nguides=%d\nwidget=%d\nkey_tone=%d\n",
             A.light, A.theme_auto, A.scene, theme_accent, A.bright, A.lang, A.src_usb, A.shuffle, A.repeat,
             A.pl_date, A.radio_on, A.wide, A.guides, A.widget % 2, A.key_tone);
}

/* track from the USB stick: wait until it is found during scanning; if the user already started something, it is no longer needed */
static void track_sync(int playing)
{
    int i;
    if (!fs_tried && mcu_st()->fs_ok) {                  /* block from the MCU (at startup) takes priority over the file */
        const unsigned char *b = mcu_st()->fs;
        fs_tried = 1;
        if (!memcmp(b + 4, FS_MARK, 4)) {
            trk_path[0] = 0; trk_pending = 2;
            trk_pos = b[8] | b[9] << 8; trk_play = b[10] & 1;
            memcpy(fs_sent, b, 12);
            fprintf(stderr, "hu: settings: MCU track hash %02x%02x%02x%02x at %.0f s%s\n", b[0], b[1], b[2], b[3], trk_pos, trk_play ? ", play" : "");
        }
    }
    if (trk_pending) {
        const unsigned char *b = mcu_st()->fs;
        uint32_t h = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
        if (A.m_loaded) { trk_pending = 0; return; }
        for (i = 0; i < NTRACKS; i++)
            if (trk_pending == 2 ? path_hash(TRACKS[i].path) == h : !strcmp(TRACKS[i].path, trk_path)) break;
        if (i >= NTRACKS) return;
        trk_pending = 0;
        A.trk = i; A.m_loaded = 1;
        if (trk_pos > 1) { A.pos = trk_pos; np_seek(trk_pos); }     /* before opening - deferred seek */
        if (trk_play && A.src_usb && A.usb_conn) A.playing = 1;
        base_dirty(); screen_dirty();
        fprintf(stderr, "hu: settings: track %d at %.0f s%s\n", i, trk_pos, A.playing ? ", play" : "");
        return;
    }
    /* what is playing goes to the MCU; after "screen off" (0x04 [01]) we do not touch it: hu pauses, but the MCU must keep "was playing" */
    if (!shut_done && A.m_loaded && A.src_usb && A.usb_conn && A.trk >= 0 && A.trk < NTRACKS && mcu_ok()) {
        unsigned char b[12];
        uint32_t h = path_hash(TRACKS[A.trk].path);
        int s = A.pos < 0 ? 0 : A.pos > 65535 ? 65535 : (int)A.pos;
        b[0] = (unsigned char)(h >> 24); b[1] = (unsigned char)(h >> 16); b[2] = (unsigned char)(h >> 8); b[3] = (unsigned char)h;
        memcpy(b + 4, FS_MARK, 4);
        b[8] = (unsigned char)s; b[9] = (unsigned char)(s >> 8); b[10] = playing ? 1 : 0; b[11] = 0;
        if (memcmp(b, fs_sent, 12)) { memcpy(fs_sent, b, 12); mcu_fastsave(b); }   /* changes once a second while playing */
    }
}

static void save_changed(void)
{
    char s[sizeof last];
    snap(s, sizeof s, trk_play);
    if (!strcmp(s, last)) return;
    save(s);
    memcpy(last, s, sizeof last);
}

void settings_tick(double now, int playing)
{
    track_sync(playing);
    if (now - tick_t < 1) return;
    tick_t = now;
    save_changed();
}

/* now (going to sleep) - settings, without the track */
void settings_flush(int playing)
{
    track_sync(playing);
    save_changed();
}

/* MCU turned off the screen and sound (ignition off / door) - the last track state goes to the MCU (was playing -> "was playing": hu already
 * paused), settings - if changed (once) */
void settings_shutdown(int playing)
{
    if (shut_done) return;
    shut_done = 1;
    track_sync(playing);
    save_changed();
    fprintf(stderr, "hu: settings: shutdown%s\n", playing ? " (playing - resume)" : "");
}
int settings_resume_usb(void) { return trk_play && A.src_usb; }   /* the USB stick was playing at shutdown - resume it */
void settings_shutdown_cancel(void) { shut_done = 0; }   /* MCU changed its mind (ACC again) - the next shutdown is handled too */
