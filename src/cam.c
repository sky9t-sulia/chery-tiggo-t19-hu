/* Rear-view camera, see cam.h. */
#include "cam.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEV "/dev/video0"                 /* HU_CAM_DEV - another device (PC testing) */
#define REVERSE "/sys/class/gpio/gpio84/value"   /* reverse; HU_REVERSE - another file (PC testing) */
#define NBUF 3

static int fd = -1, cur = -1, fw, fh, blank;
static double last_t;
static struct { void *va; size_t len; unsigned pa; gfx_tex t; } buf[NBUF];

/* reset of the ADV7182 video decoder (I2C 0, 0x21) - the same list of writes as in cameradetect (reset7182) */
static void reset_adv7182(void)
{
    if (system("i2cset -f -y 0 0x21 0x0f 0x80; sleep 0.1; i2cset -f -y 0 0x21 0x0f 0x00;"
               " for r in 52:cd 00:00 0e:80 9c:00 9c:ff 0e:00 0e:80 d9:44 0e:40 e0:01 0e:00 17:41 03:0c 04:07 13:00"
               " 1d:40 0e:40 84:00 80:80 0e:00 0c:20 0a:c0; do i2cset -f -y 0 0x21 0x${r%:*} 0x${r#*:}; done") != 0)
        fprintf(stderr, "hu: cam: adv7182 reset failed\n");
}

static void stock_stop(void) { if (system("killall cameradetect.sh cameradetect 2>/dev/null; sleep 0.2") != 0) {} }

int cam_active(void) { return fd >= 0; }

/* the shell started with R already on: the stock cameradetect is showing the camera at that moment - killing it in the middle
 * of showing = the camera hangs until the HU is rebooted (owner, 2026-10-24). We take our own camera only after the first R release. */
static int rev_seen_off;

int cam_reverse(double now)
{
    static double t = -1;
    static int v;
    if (now - t >= 0.1) {                               /* once per 0.1 s: sysfs file, cheap to read */
        const char *p = getenv("HU_REVERSE") ? getenv("HU_REVERSE") : REVERSE;
        char c = '0';
        int g = open(p, O_RDONLY);
        t = now;
        if (g >= 0) { if (read(g, &c, 1) != 1) c = '0'; close(g); }
        v = c == '1';
        if (!v) rev_seen_off = 1;
    }
    return v;
}
double cam_last_frame(void) { return last_t; }

/* frame is empty (the decoder has not locked onto the signal yet: zeros, green in YUYV): luma Y at 16 points across the frame is almost 0 */
static int frame_blank(const unsigned char *p)
{
    int i;
    for (i = 0; i < 16; i++) {
        int x = 40 + (i % 4) * 200, y = 40 + (i / 4) * 130;
        if (y < fh && x < fw && p[(size_t)y * fw * 2 + (size_t)x * 2] > 8) return 0;
    }
    return 1;
}

void cam_close(void) { cam_stop(1); }

void cam_stop(int stock)
{
    int i;
    if (fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd, VIDIOC_STREAMOFF, &type);
        for (i = 0; i < NBUF; i++) {
            gfx_tex_free(&buf[i].t);
            if (buf[i].va && buf[i].va != MAP_FAILED) munmap(buf[i].va, buf[i].len);
            buf[i].va = NULL;
        }
        close(fd);
        fd = -1;
        fprintf(stderr, "hu: cam: closed\n");
    }
    cur = -1;
    (void)stock;   /* we do NOT restart the stock cameradetect: with our own boot it does not exist, and a running one holds the memory
                    * of the video decoder - the AA decoder did not allocate frames, black screen after reverse (in the car 2026-10-25) */
}

static int fail(const char *what)
{
    fprintf(stderr, "hu: cam: %s: %s\n", what, strerror(errno));
    cam_stop(1);
    return -1;
}

int cam_open(void)
{
    struct v4l2_format f;
    struct v4l2_requestbuffers req;
    struct v4l2_streamparm parm;
    v4l2_std_id std;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int i, in = 1;
    const char *dev = getenv("HU_CAM_DEV") ? getenv("HU_CAM_DEV") : DEV;
    if (fd >= 0) return 0;
    if (access(dev, F_OK)) return -1;                   /* PC without a camera */
    /* R since the very start of the shell - the camera is held by the stock cameradetect (see above). With our own boot (S08tiggo) it is absent:
     * the rule applies only if it is really running - otherwise the screen was black until R was toggled (owner 2026-10-25) */
    if (!rev_seen_off && !getenv("HU_REVERSE") && system("pidof cameradetect > /dev/null 2>&1") == 0) {
        static int said;
        if (!said++) fprintf(stderr, "hu: cam: reverse since start - stock camera keeps it\n");
        return -1;
    }
    stock_stop();
    reset_adv7182();
    fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) return fail("open");
    if (ioctl(fd, VIDIOC_S_INPUT, &in) < 0) return fail("S_INPUT");
    if (ioctl(fd, VIDIOC_G_STD, &std) == 0) ioctl(fd, VIDIOC_S_STD, &std);
    memset(&parm, 0, sizeof parm);
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0) return fail("S_PARM");
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    f.fmt.pix.field = V4L2_FIELD_INTERLACED;
    if (ioctl(fd, VIDIOC_S_FMT, &f) < 0) return fail("S_FMT");
    if (ioctl(fd, VIDIOC_G_FMT, &f) < 0) return fail("G_FMT");
    fw = (int)f.fmt.pix.width; fh = (int)f.fmt.pix.height;
    memset(&req, 0, sizeof req);
    req.count = NBUF; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < NBUF) return fail("REQBUFS");
    for (i = 0; i < NBUF; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP; b.index = (unsigned)i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) return fail("QUERYBUF");
        buf[i].len = b.length;
        buf[i].pa = b.m.offset;                         /* mxc_v4l2: mmap offset = physical address of the buffer */
        buf[i].va = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (buf[i].va == MAP_FAILED) return fail("mmap");
        if (ioctl(fd, VIDIOC_QBUF, &b) < 0) return fail("QBUF");
    }
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) return fail("STREAMON");
    fprintf(stderr, "hu: cam: %dx%d, stream on\n", fw, fh);
    last_t = 0; blank = 1;
    return 0;
}

/* take new frames: show the last one, give the previous one back to the driver */
gfx_tex *cam_frame(int *w, int *h)
{
    struct v4l2_buffer b;
    if (fd < 0) return NULL;
    for (;;) {
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; b.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &b) < 0) break;
        if (b.index >= NBUF) break;
        if (cur >= 0) {
            struct v4l2_buffer q;
            memset(&q, 0, sizeof q);
            q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; q.memory = V4L2_MEMORY_MMAP; q.index = (unsigned)cur;
            ioctl(fd, VIDIOC_QBUF, &q);
        }
        cur = (int)b.index;
        blank = frame_blank(buf[cur].va);
        if (!blank) last_t = pf_now();
        if (buf[cur].t.id) gfx_tex_yuv_dirty(&buf[cur].t);
        else if (gfx_tex_yuyv(&buf[cur].t, fw, fh, buf[cur].va, buf[cur].pa) < 0) fprintf(stderr, "hu: cam: texture failed\n");
    }
    if (cur < 0 || !buf[cur].t.id || blank) return NULL;
    *w = fw; *h = fh;
    return &buf[cur].t;
}
