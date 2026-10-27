/* Players on the HU's GStreamer 1.0 (1.2.3, dlopen: no headers in the sysroot, what is needed is declared here).
 *  Audio (music from the stick): playbin with audio only.
 *  Video: playbin, video - vpudec (VPU) -> appsink (NV12 in VPU memory) -> GPU texture
 *  without copying (gfx_tex_yuv, GL_VIV_tex_direct); movie audio - alsasink.
 * On PC there is no GStreamer 1.0 - nothing plays (-1). We no longer touch GStreamer 0.10: two versions in one process
 * conflict (identical GTypes). */
#include "media.h"

#include <ctype.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ST_NULL = 1, ST_PAUSED = 3, ST_PLAYING = 4, FMT_TIME = 3, SEEK_FLUSH = 1, SEEK_ACCURATE = 2, SEEK_KEY = 4, SEEKT_SET = 1,   /* SEEKT_SET - GstSeekType SET (SEEK_SET is taken by stdio) */
       MSG_EOS = 1, MSG_ERROR = 2, PB_VIDEO = 0x1, PB_AUDIO = 0x2, PB_SOFTVOL = 0x10 };

typedef struct { void *domain_code; char *message; } gerror_t;            /* GError: GQuark + gint (2 x 32 bit), message */
typedef struct { uint8_t *vaddr, *paddr, *caddr; size_t size; void *user; } phymem_t;   /* PhyMemBlock (imx-gst1.0-plugin) */
typedef struct { uint32_t flags; void *info; void *buffer; uint32_t vflags, format, id, width, height, n_planes;
                 size_t offset[4]; int stride[4]; } vmeta_t;             /* GstVideoMeta (1.2) */
typedef struct {                                                         /* GstAppSinkCallbacks */
    void (*eos)(void *sink, void *user);
    int (*new_preroll)(void *sink, void *user);
    int (*new_sample)(void *sink, void *user);
    void *reserved[4];
} appcb_t;

static struct {
    int tried, ok, video, soft;              /* soft - video without VPU (PC): Android Auto stream only, RGBA */
    void (*init)(int *, char ***);
    void *(*make)(const char *, const char *);
    void (*set)(void *, const char *, ...);
    int (*state)(void *, int);
    int (*qpos)(void *, int, int64_t *);
    int (*qdur)(void *, int, int64_t *);
    int (*seek)(void *, int, int, int64_t);
    int (*seek_full)(void *, double, int, int, int, int64_t, int, int64_t);   /* gst_element_seek: rate, fmt, flags, start/stop type+pos */
    void *(*bus)(void *);
    void *(*pop)(void *, int);
    void (*unref)(void *);
    void *(*caps)(const char *);
    void (*parse_err)(void *, gerror_t **, char **);
    void (*err_free)(gerror_t *);
    void (*gfree)(void *);
    /* video */
    void (*callbacks)(void *, appcb_t *, void *, void *);
    void *(*pull_sample)(void *);
    void *(*pull_preroll)(void *);
    void *(*sample_buf)(void *);
    void *(*get_meta)(void *, unsigned long);
    unsigned long (*vmeta_type)(void);
    phymem_t *(*phymem)(void *);
    void *(*sample_caps)(void *);
    void *(*caps_struct)(void *, unsigned);
    int (*struct_int)(void *, const char *, int *);
    int (*map)(void *, void *, int);
    void (*unmap)(void *, void *);
    unsigned long (*connect)(void *, const char *, void *, void *, void *, int);
    int (*is_a)(void *, unsigned long);
    unsigned long (*bin_type)(void);
    char *(*obj_name)(void *);
    /* Android Auto streams (appsrc) */
    void *(*parse)(const char *, gerror_t **);
    void *(*by_name)(void *, const char *);
    void (*obj_unref)(void *);
    void *(*buf_new)(void *, size_t, void *);
    size_t (*buf_fill)(void *, size_t, const void *, size_t);
    int (*push)(void *, void *);
} G;

/* GstBuffer 1.x (beginning): GstMiniObject + pool, then timestamps - we set pts ourselves */
typedef struct { size_t type; int refcount, lockstate; unsigned flags; void *copy, *dispose, *free; unsigned n_qdata;
                 void *qdata; void *pool; uint64_t pts, dts, duration, offset, offset_end; } gbuf_t;

typedef struct { void *sample; unsigned char info[128]; int mapped; } vslot;  /* info - GstMapInfo (52 bytes on the HU, 104 on PC) */

static int gst_load(void)
{
    void *g, *o, *a, *v, *f, *gl;
    if (G.tried) return G.ok;
    G.tried = 1;
    if (!(g = dlopen("libgstreamer-1.0.so.0", RTLD_NOW | RTLD_GLOBAL)) || !(o = dlopen("libgobject-2.0.so.0", RTLD_NOW | RTLD_GLOBAL))
        || !(gl = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_GLOBAL))) {
        fprintf(stderr, "hu: player: no GStreamer 1.0\n");
        return 0;
    }
    G.init = (void (*)(int *, char ***))dlsym(g, "gst_init");
    G.make = (void *(*)(const char *, const char *))dlsym(g, "gst_element_factory_make");
    G.set = (void (*)(void *, const char *, ...))dlsym(o, "g_object_set");
    G.state = (int (*)(void *, int))dlsym(g, "gst_element_set_state");
    G.qpos = (int (*)(void *, int, int64_t *))dlsym(g, "gst_element_query_position");
    G.qdur = (int (*)(void *, int, int64_t *))dlsym(g, "gst_element_query_duration");
    G.seek = (int (*)(void *, int, int, int64_t))dlsym(g, "gst_element_seek_simple");
    G.seek_full = (int (*)(void *, double, int, int, int, int64_t, int, int64_t))dlsym(g, "gst_element_seek");
    G.bus = (void *(*)(void *))dlsym(g, "gst_element_get_bus");
    G.pop = (void *(*)(void *, int))dlsym(g, "gst_bus_pop_filtered");
    G.unref = (void (*)(void *))dlsym(g, "gst_mini_object_unref");
    G.caps = (void *(*)(const char *))dlsym(g, "gst_caps_from_string");
    G.parse_err = (void (*)(void *, gerror_t **, char **))dlsym(g, "gst_message_parse_error");
    G.err_free = (void (*)(gerror_t *))dlsym(gl, "g_error_free");
    G.gfree = (void (*)(void *))dlsym(gl, "g_free");
    G.get_meta = (void *(*)(void *, unsigned long))dlsym(g, "gst_buffer_get_meta");
    G.sample_buf = (void *(*)(void *))dlsym(g, "gst_sample_get_buffer");
    G.sample_caps = (void *(*)(void *))dlsym(g, "gst_sample_get_caps");
    G.caps_struct = (void *(*)(void *, unsigned))dlsym(g, "gst_caps_get_structure");
    G.struct_int = (int (*)(void *, const char *, int *))dlsym(g, "gst_structure_get_int");
    G.map = (int (*)(void *, void *, int))dlsym(g, "gst_buffer_map");
    G.connect = (unsigned long (*)(void *, const char *, void *, void *, void *, int))dlsym(o, "g_signal_connect_data");
    G.is_a = (int (*)(void *, unsigned long))dlsym(o, "g_type_check_instance_is_a");
    G.bin_type = (unsigned long (*)(void))dlsym(g, "gst_bin_get_type");
    G.obj_name = (char *(*)(void *))dlsym(g, "gst_object_get_name");
    G.unmap = (void (*)(void *, void *))dlsym(g, "gst_buffer_unmap");
    G.parse = (void *(*)(const char *, gerror_t **))dlsym(g, "gst_parse_launch");
    G.by_name = (void *(*)(void *, const char *))dlsym(g, "gst_bin_get_by_name");
    G.obj_unref = (void (*)(void *))dlsym(g, "gst_object_unref");
    G.buf_new = (void *(*)(void *, size_t, void *))dlsym(g, "gst_buffer_new_allocate");
    G.buf_fill = (size_t (*)(void *, size_t, const void *, size_t))dlsym(g, "gst_buffer_fill");
    G.ok = G.init && G.make && G.set && G.state && G.qpos && G.qdur && G.seek && G.bus && G.pop && G.unref && G.caps
           && G.parse_err && G.err_free && G.gfree;
    if (!G.ok) { fprintf(stderr, "hu: player: GStreamer 1.0 incomplete\n"); return 0; }
    G.init(NULL, NULL);
    a = dlopen("libgstapp-1.0.so.0", RTLD_NOW);
    v = dlopen("libgstvideo-1.0.so.0", RTLD_NOW);
    f = dlopen("libgstfsl-1.0.so.0", RTLD_NOW);                 /* physical address of VPU buffers (HU only) */
    if (a) {
        G.callbacks = (void (*)(void *, appcb_t *, void *, void *))dlsym(a, "gst_app_sink_set_callbacks");
        G.pull_sample = (void *(*)(void *))dlsym(a, "gst_app_sink_pull_sample");
        G.pull_preroll = (void *(*)(void *))dlsym(a, "gst_app_sink_pull_preroll");
        G.push = (int (*)(void *, void *))dlsym(a, "gst_app_src_push_buffer");
    }
    if (v) G.vmeta_type = (unsigned long (*)(void))dlsym(v, "gst_video_meta_api_get_type");
    if (f) G.phymem = (phymem_t *(*)(void *))dlsym(f, "gst_buffer_query_phymem_block");
    G.video = G.callbacks && G.pull_sample && G.pull_preroll && G.sample_buf && G.get_meta && G.vmeta_type && G.phymem
              && G.sample_caps && G.caps_struct && G.struct_int && G.map && G.unmap;
    G.soft = !G.video && G.callbacks && G.pull_sample && G.pull_preroll && G.sample_buf && G.sample_caps && G.caps_struct
             && G.struct_int && G.map && G.unmap;
    if (!G.video) fprintf(stderr, "hu: player: no video path (app %p video %p fsl %p)%s\n", a, v, f, G.soft ? ", software for AA" : "");
    return 1;
}

static void to_uri(char *uri, size_t n, const char *path)       /* path -> URI (spaces and non-ASCII to %XX) */
{
    size_t o;
    const unsigned char *p;
    snprintf(uri, n, "file://");
    for (o = strlen(uri), p = (const unsigned char *)path; *p && o + 4 < n; p++) {
        if (isalnum(*p) || strchr("/-_.~", *p)) uri[o++] = (char)*p;
        else o += (size_t)snprintf(uri + o, n - o, "%%%02X", *p);
    }
    uri[o] = 0;
}

/* end or error (the error goes to the log) */
static int bus_done(void *bus, const char *who)
{
    void *m;
    int done = 0;
    while (bus && (m = G.pop(bus, MSG_ERROR))) {               /* error - to the log */
        gerror_t *e = NULL;
        char *dbg = NULL;
        G.parse_err(m, &e, &dbg);
        if (e) { fprintf(stderr, "hu: %s: %s\n", who, e->message); G.err_free(e); }
        if (dbg) G.gfree(dbg);
        G.unref(m);
        done = 1;
    }
    while (bus && (m = G.pop(bus, MSG_EOS))) { G.unref(m); done = 1; fprintf(stderr, "hu: %s: EOS\n", who); }
    return done;
}

static double q_time(void *el, int dur)
{
    int64_t v = -1;
    if (!el) return -1;
    if (!(dur ? G.qdur(el, FMT_TIME, &v) : G.qpos(el, FMT_TIME, &v)) || v < 0) return -1;
    return v / 1e9;
}

/* ================================================================= audio */

static double pl_vol = 1;               /* volume of stick and video (software, playbin volume) */
static void *apb, *apb_bus;

int player_open(const char *path)
{
    char uri[4200];
    if (!gst_load()) return -1;
    if (!apb) {
        if (!(apb = G.make("playbin", "hu-audio"))) return -1;
        G.set(apb, "flags", PB_AUDIO | PB_SOFTVOL, "volume", pl_vol, NULL);       /* audio only */
        apb_bus = G.bus(apb);
    }
    G.state(apb, ST_NULL);
    to_uri(uri, sizeof uri, path);
    G.set(apb, "uri", uri, NULL);
    G.state(apb, ST_PLAYING);
    return 0;
}
void player_pause(int pause) { if (apb) G.state(apb, pause ? ST_PAUSED : ST_PLAYING); }
void player_stop(void) { if (apb) G.state(apb, ST_NULL); }
void player_seek(double sec)
{
    int ok;
    if (!apb) { fprintf(stderr, "hu: audio: seek %.1f - no pipeline\n", sec); return; }
    ok = G.seek(apb, FMT_TIME, SEEK_FLUSH | SEEK_KEY, (int64_t)(sec * 1e9));
    fprintf(stderr, "hu: audio: seek %.1f -> %s (pos %.1f dur %.1f)\n", sec, ok ? "ok" : "FAIL", q_time(apb, 0), q_time(apb, 1));
}
double player_pos(void) { return q_time(apb, 0); }
double player_dur(void) { return q_time(apb, 1); }
int player_eos(void) { return bus_done(apb_bus, "player"); }

/* ================================================================= video */

static void *vpb, *vpb_bus, *vsink;
static void *spb, *spb_bus, *ssrc;                             /* Android Auto video: appsrc -> vpudec -> appsink */

void player_volume(double v)
{
    if (v == pl_vol) return;
    pl_vol = v;
    if (apb) G.set(apb, "volume", v, NULL);
    if (vpb) G.set(vpb, "volume", v, NULL);
}
static pthread_mutex_t vmu = PTHREAD_MUTEX_INITIALIZER;
static volatile int v_soft;                                    /* frame not from the VPU (software decoder) - not shown */
static void *v_pending;                                        /* GstSample: new (from the GStreamer thread) */
#define VHOLD 3                                                /* we hold frames while the GPU (lagging 1-2 frames behind) might still read them */
static vslot v_hold[VHOLD];                                    /* [0] - on screen */
/* a separate texture for each VPU buffer (their pool is constant): bind once, as in Qt imx6vivantevideonode -
 * rebinding one texture while the GPU is still finishing the previous frame causes tearing */
#define VTEX 24
static struct { unsigned pa; gfx_tex t; } vtex[VTEX];
static int n_vtex;
static gfx_tex *v_curtex;
static gfx_tex v_snap;                                         /* copy of the last frame: until the VPU gives a new one (seek, end) */
static int v_snap_ok, v_snap_w, v_snap_h;
static volatile unsigned v_count;                              /* frames arrived (GStreamer thread) */
static unsigned v_seen;
static double v_last_t;
static float v_u1 = 1, v_v1 = 1;
static int v_rgba;                                             /* AA stream on PC: software decoder, RGBA frames */
static gfx_tex v_rgbtex;
static int v_w, v_h;

static pthread_cond_t vcond = PTHREAD_COND_INITIALIZER;
static volatile int v_flush;                                   /* seek/stop: the decoder thread does not wait */

/* as in the stock one (Qt QVideoSurfaceGstSink): the decoder thread waits until the frame is taken to the screen - the decoder does not run
 * ahead and does not churn VPU buffers. No longer than 50 ms, so as not to hang. */
static void put_sample(void *s)
{
    void *buf;
    if (!s) return;
    if (!v_rgba && (buf = G.sample_buf(s)) && (!G.phymem || !G.phymem(buf))) v_soft = 1;   /* not VPU - immediately, even if the video is not on screen right now */
    v_count++;
    pthread_mutex_lock(&vmu);
    if (v_pending) G.unref(v_pending);
    v_pending = s;
    if (!v_flush) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_nsec += 50000000;
        if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
        while (v_pending == s && !v_flush && pthread_cond_timedwait(&vcond, &vmu, &t) == 0) {}
    }
    pthread_mutex_unlock(&vmu);
}
static int on_sample(void *sink, void *user) { (void)user; put_sample(G.pull_sample(sink)); return 0; }
static int on_preroll(void *sink, void *user) { (void)user; put_sample(G.pull_preroll(sink)); return 0; }   /* frame on pause / after seek */

static void slot_free(vslot *v)
{
    if (v->mapped) G.unmap(G.sample_buf(v->sample), v->info);
    if (v->sample) G.unref(v->sample);
    memset(v, 0, sizeof *v);
}

static void drop_samples(void)
{
    pthread_mutex_lock(&vmu);
    pthread_cond_broadcast(&vcond);
    if (v_pending) G.unref(v_pending);
    v_pending = NULL;
    pthread_mutex_unlock(&vmu);
    {
        int i;
        for (i = 0; i < VHOLD; i++) slot_free(&v_hold[i]);
        for (i = 0; i < n_vtex; i++) gfx_tex_free(&vtex[i].t);
        n_vtex = 0; v_curtex = NULL;
    }
}

static void release_frames(void)
{
    v_flush = 1;                                               /* until the next open/seek: the thread does not wait for the screen */
    if (v_w && v_curtex) {                                     /* last frame - into its own texture, keep showing it */
        gfx_tex_snapshot(&v_snap, v_curtex, v_u1, v_v1);
        v_snap_ok = 1; v_snap_w = v_w; v_snap_h = v_h;
    }
    gfx_finish();
    drop_samples();
    v_w = v_h = 0;
}

/* main loop: 0.5 s without new frames (end of file, jam) - return the VPU frames, otherwise the decoder cannot finish
 * and the end (EOS) does not arrive */
void vplayer_poll(double now)
{
    if (!vpb && !spb) return;
    if (v_count != v_seen) { v_seen = v_count; v_last_t = now; return; }
    if (v_hold[0].sample && now - v_last_t > 0.5) { fprintf(stderr, "hu: video: no frames 0.5 s - frames back to VPU\n"); release_frames(); }
}

/* how many seconds there were no new frames (a fallback end indicator) */
double vplayer_idle(double now) { return (vpb || spb) && v_last_t > 0 ? now - v_last_t : 0; }

/* vpudec: frame reserve (frame-plus) 6 as in the stock one, HU_VPU_PLUS=N - for testing. playbin elements appear
 * along the way - we catch them in all nested bins. */
static void on_element(void *bin, void *el, void *user)
{
    char *n;
    (void)bin; (void)user;
    if (G.is_a(el, G.bin_type())) { G.connect(el, "element-added", (void *)on_element, NULL, NULL, 0); return; }
    if ((n = G.obj_name(el))) {
        if (!strncmp(n, "vpudec", 6)) {                         /* frame reserve as in the stock one (0.10 vpudec: 6, in 1.x - 3) */
            unsigned fp = getenv("HU_VPU_PLUS") ? (unsigned)atoi(getenv("HU_VPU_PLUS")) : 6;
            G.set(el, "frame-plus", fp, NULL);
            fprintf(stderr, "hu: video: %s frame-plus %u\n", n, fp);
        }
        G.gfree(n);
    }
}

int vplayer_open(const char *path)
{
    static appcb_t cb = { NULL, on_preroll, on_sample, { NULL } };
    char uri[4200];
    if (!gst_load() || !G.video) return -1;
    vstream_close();                                           /* stick video and Android Auto - not at the same time */
    if (!vpb) {
        void *caps;
        if (!(vpb = G.make("playbin", "hu-video")) || !(vsink = G.make("appsink", "hu-vsink"))) return -1;
        caps = G.caps("video/x-raw,format=NV12");
        G.set(vsink, "caps", caps, "sync", 1, "max-buffers", 1, "drop", 1, "emit-signals", 0, NULL);
        G.unref(caps);
        G.callbacks(vsink, &cb, NULL, NULL);
        G.set(vpb, "flags", PB_VIDEO | PB_AUDIO | PB_SOFTVOL, "video-sink", vsink, "volume", pl_vol, NULL);   /* without subtitles */
        if (G.connect && G.is_a && G.bin_type && G.obj_name) G.connect(vpb, "element-added", (void *)on_element, NULL, NULL, 0);
        vpb_bus = G.bus(vpb);
    }
    release_frames();
    G.state(vpb, ST_NULL);
    drop_samples();
    v_soft = 0; v_snap_ok = 0;                                 /* another video - do not show the old frame */
    to_uri(uri, sizeof uri, path);
    G.set(vpb, "uri", uri, NULL);
    v_flush = 0; v_last_t = 0;
    G.state(vpb, ST_PLAYING);
    return 0;
}
/* seek and stop: on flush the VPU waits for ALL its frames to be returned - they must not be held (otherwise the main thread
 * is stuck in the call, and the MCU without the "I'm alive" keep-alive reboots the HU). First the GPU finishes drawing, then we release the frames. */
void vplayer_pause(int pause) { if (vpb) G.state(vpb, pause ? ST_PAUSED : ST_PLAYING); }
void vplayer_stop(void) { if (vpb) { release_frames(); G.state(vpb, ST_NULL); drop_samples(); } }
void vplayer_close(void) { vplayer_stop(); v_snap_ok = 0; }   /* completely: do not show the last frame either */
/* to the nearest key frame (the user's decision). seek_simple does not reset the segment end: after the first seek
 * all further ones were rejected ("gst_segment_do_seek: start <= stop"). Hence a full seek: end = -1 (to the end of the file) */
void vplayer_seek(double sec)
{
    int ok;
    if (!vpb) return;
    release_frames();
    if (G.seek_full) ok = G.seek_full(vpb, 1.0, FMT_TIME, SEEK_FLUSH | SEEK_KEY, SEEKT_SET, (int64_t)(sec * 1e9), SEEKT_SET, -1);
    else ok = G.seek(vpb, FMT_TIME, SEEK_FLUSH | SEEK_KEY, (int64_t)(sec * 1e9));
    if (!ok) fprintf(stderr, "hu: video: seek %.1f FAIL\n", sec);
    v_flush = 0;
}
double vplayer_pos(void) { return q_time(vpb, 0); }
double vplayer_dur(void) { return q_time(vpb, 1); }
int vplayer_eos(void) { return bus_done(vpb_bus, "video"); }

int vplayer_new_frame(void)
{
    int n;
    pthread_mutex_lock(&vmu);
    n = v_pending != NULL;
    pthread_mutex_unlock(&vmu);
    return n;
}

/* current frame (main thread, while drawing): a new one goes into the texture; u1/v1 - visible part (without VPU alignment),
 * w/h - picture size. NULL - no frame. */
gfx_tex *vplayer_frame(float *u1, float *v1, int *w, int *h)
{
    void *s;
    pthread_mutex_lock(&vmu);
    s = v_pending; v_pending = NULL;
    pthread_cond_broadcast(&vcond);                            /* frame taken - the decoder goes on */
    pthread_mutex_unlock(&vmu);
    if (s) {
        vslot n;
        void *buf = G.sample_buf(s);
        vmeta_t *m = buf ? (vmeta_t *)G.get_meta(buf, G.vmeta_type()) : NULL;
        phymem_t *pm = buf && G.phymem ? G.phymem(buf) : NULL;
        int w0 = 0, h0 = 0, sw = 0, sh = 0;
        const void *va = NULL;
        unsigned pa = ~0u;                                      /* ~0 - the GPU will map the memory by virtual address itself */
        memset(&n, 0, sizeof n);
        n.sample = s;
        if (pm) { va = pm->vaddr; pa = (unsigned)(uintptr_t)pm->paddr; }
        else if (buf && G.map(buf, n.info, 1)) { n.mapped = 1; memcpy(&va, n.info + 2 * sizeof(void *), sizeof va); }   /* GstMapInfo.data */
        if (m && m->stride[0] > 0 && m->offset[1] > 0) {       /* Y plane with VPU alignment */
            int a16;
            w0 = (int)m->width; h0 = (int)m->height; sw = m->stride[0]; sh = (int)(m->offset[1] / (size_t)m->stride[0]);
            /* the VPU stores the frame with height aligned to 16 (1080 -> 1088), while the MPEG-4 meta reports 1080:
             * then the color (UV) shifts up by 8 lines - a colored stripe at the top. We take the aligned one if the buffer has it. */
            a16 = (h0 + 15) & ~15;
            if (pm && sh < a16 && pm->size >= (size_t)sw * a16 * 3 / 2) sh = a16;
        } else if (G.sample_caps(s)) {                         /* without meta: packed NV12 by caps */
            void *st = G.caps_struct(G.sample_caps(s), 0);
            if (st && G.struct_int(st, "width", &w0) && G.struct_int(st, "height", &h0)) { sw = w0; sh = h0; }
        }
        gfx_tex *t = NULL;
        if (pm) {                                              /* texture of this VPU buffer (a new one - bind) */
            int i;
            for (i = 0; i < n_vtex && vtex[i].pa != pa; i++) {}
            if (i < n_vtex) { t = &vtex[i].t; gfx_tex_yuv_dirty(t); }   /* same buffer, new frame */
            else if (n_vtex < VTEX && va && sw > 0 && h0 > 0 && sh >= h0) {
                memset(&vtex[n_vtex], 0, sizeof vtex[n_vtex]);
                if (gfx_tex_yuv(&vtex[n_vtex].t, sw, sh, va, pa) == 0) { vtex[n_vtex].pa = pa; t = &vtex[n_vtex++].t; }
                else gfx_tex_free(&vtex[n_vtex].t);
            }
        }
        if (!pm && v_rgba && va && w0 > 0 && h0 > 0) {         /* PC, AA stream: RGBA copy into the texture */
            gfx_tex_free(&v_rgbtex);
            if (gfx_tex_rgba(&v_rgbtex, w0, h0, va) == 0) { v_curtex = &v_rgbtex; v_w = w0; v_h = h0; v_u1 = v_v1 = 1; }
            { static int once; if (!once++) fprintf(stderr, "hu: aa video: first frame %dx%d (software)\n", w0, h0); }
        } else if (!pm) { v_soft = 1; v_w = v_h = 0; }        /* hardware decoder (VPU) only: otherwise "Can't open" */
        else if (t) {
            v_curtex = t; v_w = w0; v_h = h0;
            v_u1 = (w0 - 0.5f) / sw; v_v1 = (h0 - 0.5f) / sh;     /* half a pixel from the edge: filtering does not pick up the alignment lines */
        } else {
            static int warned;
            if (!warned++) fprintf(stderr, "hu: video: cannot map frame (meta %p phys %p va %p %dx%d stride %d/%d)\n",
                                   (void *)m, (void *)pm, va, w0, h0, sw, sh);
            v_w = v_h = 0;
        }
        slot_free(&v_hold[VHOLD - 1]);                          /* the oldest: the GPU has definitely finished drawing it */
        memmove(&v_hold[1], &v_hold[0], sizeof v_hold[0] * (VHOLD - 1));
        v_hold[0] = n;
    }
    if (!v_hold[0].sample || !v_w || !v_curtex) {
        if (!v_snap_ok) return NULL;
        *u1 = 1; *v1 = 1; *w = v_snap_w; *h = v_snap_h;      /* copy of the last frame */
        return &v_snap;
    }
    *u1 = v_u1; *v1 = v_v1; *w = v_w; *h = v_h;
    return v_curtex;
}

/* ================================================================= Android Auto streams
 * Video: H.264 (byte-stream, whole frame) from the phone -> h264parse -> vpudec -> the same appsink as the video from the
 * stick: VPU frames into a texture without copying (vplayer_frame). Frame 1280x720, the caller crops the center 1024x600.
 * Audio: PCM S16LE from the phone, its own pipeline per stream (0 media 48000/2, 1 navigation 16000/1, 2 system 16000/1),
 * ALSA default (dmix - the streams are mixed). */

static void set_frame_plus(void *pipe)
{
    void *el = G.by_name(pipe, "aadec");
    unsigned fp = getenv("HU_VPU_PLUS") ? (unsigned)atoi(getenv("HU_VPU_PLUS")) : 6;   /* as in vplayer_open */
    if (!el) return;
    G.set(el, "frame-plus", fp, NULL);
    G.obj_unref(el);
}

static void *launch(const char *desc, const char *who)
{
    gerror_t *e = NULL;
    void *p = G.parse(desc, &e);
    if (e) { fprintf(stderr, "hu: %s: %s\n", who, e->message ? e->message : "?"); G.err_free(e); }
    return p;
}

static int push_buf(void *src, const void *data, size_t n, int64_t pts_us)
{
    void *b;
    if (!src || !n || !(b = G.buf_new(NULL, n, NULL))) return -1;
    G.buf_fill(b, 0, data, n);
    if (pts_us >= 0) ((gbuf_t *)b)->pts = (uint64_t)pts_us * 1000;   /* otherwise GST_CLOCK_TIME_NONE (as allocated) */
    if (G.push(src, b) != 0) { static int warned; if (!warned++) fprintf(stderr, "hu: player: appsrc push failed\n"); return -1; }
    return 0;                                                  /* GST_FLOW_OK; the buffer is taken by appsrc */
}

int vstream_open(void)
{
    static appcb_t cb = { NULL, on_preroll, on_sample, { NULL } };
    void *sink, *caps;
    if (!gst_load() || !(G.video || G.soft) || !G.parse || !G.by_name || !G.obj_unref || !G.buf_new || !G.buf_fill || !G.push) return -1;
    if (spb) return 0;
    vplayer_close();
    v_rgba = !G.video;                                         /* PC: openh264dec -> RGBA (copy), HU: VPU without copying */
    spb = launch(v_rgba ? "appsrc name=aasrc is-live=true do-timestamp=false format=time "
                          "caps=video/x-h264,stream-format=byte-stream,alignment=au ! h264parse ! openh264dec ! videoconvert ! "
                          "appsink name=aasink"
                        : "appsrc name=aasrc is-live=true do-timestamp=false format=time "
                          "caps=video/x-h264,stream-format=byte-stream,alignment=au ! h264parse ! vpudec name=aadec ! "
                          "appsink name=aasink", "aa video");
    if (!spb) return -1;
    ssrc = G.by_name(spb, "aasrc");
    sink = G.by_name(spb, "aasink");
    if (!ssrc || !sink) { if (sink) G.obj_unref(sink); vstream_close(); return -1; }
    caps = G.caps(v_rgba ? "video/x-raw,format=RGBA" : "video/x-raw,format=NV12");
    G.set(sink, "caps", caps, "sync", 0, "max-buffers", 1, "drop", 1, "emit-signals", 0, NULL);   /* live stream - immediately */
    G.unref(caps);
    G.callbacks(sink, &cb, NULL, NULL);
    G.obj_unref(sink);
    set_frame_plus(spb);
    spb_bus = G.bus(spb);
    drop_samples();
    v_soft = 0; v_snap_ok = 0; v_flush = 0; v_last_t = 0;
    G.state(spb, ST_PLAYING);
    return 0;
}

int vstream_push(const void *h264, size_t n, int64_t pts_us) { return spb ? push_buf(ssrc, h264, n, pts_us) : -1; }

void vstream_close(void)
{
    if (!spb) return;
    release_frames();
    if (v_rgba) { if (v_curtex == &v_rgbtex) v_curtex = NULL; gfx_tex_free(&v_rgbtex); v_rgba = 0; }
    G.state(spb, ST_NULL);
    drop_samples();
    v_snap_ok = 0;
    if (ssrc) G.obj_unref(ssrc);
    if (spb_bus) G.obj_unref(spb_bus);
    G.obj_unref(spb);
    spb = spb_bus = ssrc = NULL;
}

#define NA 3
static struct { void *pipe, *src, *vol; } as[NA];

int astream_open_dev(int id, int rate, int channels, const char *dev)
{
    char d[440];
    if (id < 0 || id >= NA || !gst_load() || !G.parse || !G.by_name || !G.obj_unref || !G.buf_new || !G.buf_fill || !G.push) return -1;
    if (as[id].pipe) return 0;
    snprintf(d, sizeof d, "appsrc name=src is-live=true do-timestamp=true format=time "
             "caps=audio/x-raw,format=S16LE,layout=interleaved,rate=%d,channels=%d ! audioconvert ! audioresample ! "
             "volume name=vol ! alsasink sync=false%s%s", rate, channels, dev ? " device=" : "", dev ? dev : "");
    if (!(as[id].pipe = launch(d, "aa audio"))) return -1;
    as[id].src = G.by_name(as[id].pipe, "src");
    as[id].vol = G.by_name(as[id].pipe, "vol");
    if (!as[id].src) { astream_close(id); return -1; }
    if (G.state(as[id].pipe, ST_PLAYING) == 0 && dev) {      /* the device is busy at another rate - the normal output */
        fprintf(stderr, "hu: aa audio %d: %s failed, default\n", id, dev);
        astream_close(id);
        return astream_open_dev(id, rate, channels, NULL);
    }
    return 0;
}

int astream_open(int id, int rate, int channels) { return astream_open_dev(id, rate, channels, NULL); }

int astream_push(int id, const void *pcm, size_t n) { return id >= 0 && id < NA && as[id].pipe ? push_buf(as[id].src, pcm, n, -1) : -1; }

void astream_volume(int id, double v) { if (id >= 0 && id < NA && as[id].vol) G.set(as[id].vol, "volume", v, NULL); }

void astream_close(int id)
{
    if (id < 0 || id >= NA || !as[id].pipe) return;
    G.state(as[id].pipe, ST_NULL);
    if (as[id].src) G.obj_unref(as[id].src);
    if (as[id].vol) G.obj_unref(as[id].vol);
    G.obj_unref(as[id].pipe);
    memset(&as[id], 0, sizeof as[id]);
}

void player_shutdown(void)
{
    int i;
    vstream_close();
    for (i = 0; i < NA; i++) astream_close(i);
    gfx_tex_free(&v_snap);
    if (vpb) { release_frames(); G.state(vpb, ST_NULL); drop_samples(); }
    if (apb) G.state(apb, ST_NULL);
}

int vplayer_unsupported(void) { return v_soft; }
