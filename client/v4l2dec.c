/*
 * V4L2 stateful decoder backend (see v4l2dec.h). The dance, per the
 * kernel's stateful-decoder interface:
 *
 *   open -> subscribe SOURCE_CHANGE/EOS -> S_FMT(OUTPUT)=H264 ->
 *   REQBUFS/mmap OUTPUT -> STREAMON(OUTPUT) -> queue the first access
 *   unit (an IDR with SPS/PPS, the server guarantees it) -> the driver
 *   fires SOURCE_CHANGE once it knows the coded size -> S_FMT(CAPTURE)
 *   (we try RGB565 first on RGB565 framebuffers — the Pi firmware can
 *   convert — then NV12, then YU12) -> REQBUFS/mmap/queue CAPTURE ->
 *   STREAMON(CAPTURE) -> steady state: OUTPUT buffers cycle access
 *   units in, CAPTURE buffers cycle decoded frames out.
 *
 * One access unit per OUTPUT buffer, never split (the server's VBV caps
 * an AU well under our 1 MiB buffers). No B-frames on the wire, so
 * decode order == display order and frames are emitted as dequeued.
 *
 * End of an H.264 episode (SREMFB_ENC_H264_EOS): V4L2_DEC_CMD_STOP,
 * dequeue-and-display until the buffer flagged LAST (or -EPIPE), then
 * V4L2_DEC_CMD_START resumes with the queues intact for the next
 * episode. A second SOURCE_CHANGE (mid-connection resolution change,
 * which sRemFB never does) is treated as fatal; the caller reconnects.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "v4l2dec.h"

#define OUT_BUFS   4
#define CAP_BUFS_MAX 32
#define AU_MAX     (1u << 20)      /* 1 MiB per access unit */

extern volatile sig_atomic_t g_stop;   /* sremfb-client.c */

static void declog(const char *fmt, ...);

static struct {
    char node[32];                 /* probed decoder node */
    int fd;
    unsigned stream_w, stream_h;
    int want_rgb565;

    void *out_map[OUT_BUFS];
    size_t out_len[OUT_BUFS];
    int out_free[OUT_BUFS];

    int cap_ready;
    uint32_t cap_fourcc;
    unsigned cap_nbufs;
    unsigned coded_w, coded_h;
    unsigned vis_w, vis_h;
    unsigned cap_stride;
    void *cap_map[CAP_BUFS_MAX];
    size_t cap_len[CAP_BUFS_MAX];

    int saw_source_change;
    int saw_last;                  /* drain finished */
} D = { .fd = -1 };

static void declog(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "sremfb-client: v4l2dec: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR && !g_stop);
    return r;
}

/* ------------------------------------------------------------- probe */

int v4l2dec_probe(void)
{
    if (getenv("SREMFB_NO_H264"))
        return 0;

    for (int i = 0; i < 32; i++) {
        char node[32];
        snprintf(node, sizeof(node), "/dev/video%d", i);
        int fd = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        struct v4l2_capability cap = {0};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
            close(fd);
            continue;
        }
        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                            ? cap.device_caps : cap.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_M2M_MPLANE)) {
            close(fd);
            continue;
        }

        struct v4l2_fmtdesc f = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        };
        int has_h264 = 0;
        for (f.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &f) == 0; f.index++)
            if (f.pixelformat == V4L2_PIX_FMT_H264) {
                has_h264 = 1;
                break;
            }
        close(fd);
        if (has_h264) {
            snprintf(D.node, sizeof(D.node), "%s", node);
            declog("hardware H.264 decoder found: %s (%s)", node, cap.card);
            return 1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------- open */

int v4l2dec_fd(void)
{
    return D.fd;
}

static int setup_output(void)
{
    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    };
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    fmt.fmt.pix_mp.width = D.stream_w;
    fmt.fmt.pix_mp.height = D.stream_h;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = AU_MAX;
    if (xioctl(D.fd, VIDIOC_S_FMT, &fmt) < 0) {
        declog("S_FMT(OUTPUT H264) failed: %s", strerror(errno));
        return -1;
    }

    struct v4l2_requestbuffers req = {
        .count = OUT_BUFS,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (xioctl(D.fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        declog("REQBUFS(OUTPUT) failed: %s", strerror(errno));
        return -1;
    }

    for (unsigned i = 0; i < req.count && i < OUT_BUFS; i++) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
        struct v4l2_buffer buf = {
            .index = i,
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
            .m.planes = planes,
            .length = 1,
        };
        if (xioctl(D.fd, VIDIOC_QUERYBUF, &buf) < 0)
            return -1;
        D.out_len[i] = planes[0].length;
        D.out_map[i] = mmap(NULL, planes[0].length,
                            PROT_READ | PROT_WRITE, MAP_SHARED, D.fd,
                            planes[0].m.mem_offset);
        if (D.out_map[i] == MAP_FAILED) {
            D.out_map[i] = NULL;
            return -1;
        }
        D.out_free[i] = 1;
    }

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(D.fd, VIDIOC_STREAMON, &type) < 0) {
        declog("STREAMON(OUTPUT) failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int try_capture_fmt(uint32_t fourcc)
{
    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    };
    if (xioctl(D.fd, VIDIOC_G_FMT, &fmt) < 0)
        return -1;
    fmt.fmt.pix_mp.pixelformat = fourcc;
    if (xioctl(D.fd, VIDIOC_S_FMT, &fmt) < 0)
        return -1;
    if (xioctl(D.fd, VIDIOC_G_FMT, &fmt) < 0)
        return -1;
    return fmt.fmt.pix_mp.pixelformat == fourcc ? 0 : -1;
}

static int setup_capture(void)
{
    /* the coded size is known now (SOURCE_CHANGE fired) */
    static const uint32_t ladder565[] = {
        V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_YUV420, 0
    };
    static const uint32_t ladder32[] = {
        V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_YUV420, 0
    };
    const uint32_t *ladder = D.want_rgb565 ? ladder565 : ladder32;
    uint32_t got = 0;

    for (int i = 0; ladder[i]; i++)
        if (try_capture_fmt(ladder[i]) == 0) {
            got = ladder[i];
            break;
        }
    if (!got) {
        declog("no usable capture format (RGB565/NV12/YU12 all refused)");
        return -1;
    }

    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    };
    if (xioctl(D.fd, VIDIOC_G_FMT, &fmt) < 0)
        return -1;
    D.cap_fourcc = got;
    D.coded_w = fmt.fmt.pix_mp.width;
    D.coded_h = fmt.fmt.pix_mp.height;
    D.cap_stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    if (D.cap_stride == 0)
        D.cap_stride = got == V4L2_PIX_FMT_RGB565 ? D.coded_w * 2
                                                  : D.coded_w;

    struct v4l2_selection sel = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .target = V4L2_SEL_TGT_COMPOSE,
    };
    if (xioctl(D.fd, VIDIOC_G_SELECTION, &sel) == 0 && sel.r.width) {
        D.vis_w = sel.r.width;
        D.vis_h = sel.r.height;
    } else {
        D.vis_w = D.coded_w < D.stream_w ? D.coded_w : D.stream_w;
        D.vis_h = D.coded_h < D.stream_h ? D.coded_h : D.stream_h;
    }

    struct v4l2_control min = { .id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE };
    unsigned want = 4;
    if (xioctl(D.fd, VIDIOC_G_CTRL, &min) == 0 && min.value > 0)
        want = (unsigned)min.value + 2;
    if (want > CAP_BUFS_MAX)
        want = CAP_BUFS_MAX;

    struct v4l2_requestbuffers req = {
        .count = want,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (xioctl(D.fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        declog("REQBUFS(CAPTURE) failed: %s", strerror(errno));
        return -1;
    }
    D.cap_nbufs = req.count;

    for (unsigned i = 0; i < D.cap_nbufs; i++) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
        struct v4l2_buffer buf = {
            .index = i,
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
            .m.planes = planes,
            .length = 1,
        };
        if (xioctl(D.fd, VIDIOC_QUERYBUF, &buf) < 0)
            return -1;
        D.cap_len[i] = planes[0].length;
        D.cap_map[i] = mmap(NULL, planes[0].length,
                            PROT_READ | PROT_WRITE, MAP_SHARED, D.fd,
                            planes[0].m.mem_offset);
        if (D.cap_map[i] == MAP_FAILED) {
            D.cap_map[i] = NULL;
            return -1;
        }
        if (xioctl(D.fd, VIDIOC_QBUF, &buf) < 0)
            return -1;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(D.fd, VIDIOC_STREAMON, &type) < 0) {
        declog("STREAMON(CAPTURE) failed: %s", strerror(errno));
        return -1;
    }
    D.cap_ready = 1;

    declog("decoding %ux%u (coded %ux%u) -> %.4s, %u buffers",
           D.vis_w, D.vis_h, D.coded_w, D.coded_h,
           (const char *)&D.cap_fourcc, D.cap_nbufs);
    return 0;
}

int v4l2dec_open(unsigned w, unsigned h, int fb_rgb565)
{
    if (D.fd >= 0)
        return 0;
    if (!D.node[0])
        return -1;

    char node[sizeof(D.node)];
    memcpy(node, D.node, sizeof(node));
    memset(&D, 0, sizeof(D));
    memcpy(D.node, node, sizeof(node));
    D.fd = open(D.node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (D.fd < 0) {
        declog("cannot open %s: %s", D.node, strerror(errno));
        return -1;
    }
    D.stream_w = w;
    D.stream_h = h;
    D.want_rgb565 = fb_rgb565;

    struct v4l2_event_subscription sub = { .type = V4L2_EVENT_SOURCE_CHANGE };
    if (xioctl(D.fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
        declog("cannot subscribe SOURCE_CHANGE: %s", strerror(errno));
        v4l2dec_close();
        return -1;
    }
    sub.type = V4L2_EVENT_EOS;
    xioctl(D.fd, VIDIOC_SUBSCRIBE_EVENT, &sub);   /* best effort */

    if (setup_output() < 0) {
        v4l2dec_close();
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------- steady state */

static int handle_events(void)
{
    struct v4l2_event ev;

    while (xioctl(D.fd, VIDIOC_DQEVENT, &ev) == 0) {
        if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
            if (!D.cap_ready) {
                D.saw_source_change = 1;
                if (setup_capture() < 0)
                    return -1;
            } else {
                declog("mid-stream resolution change: unsupported");
                return -1;
            }
        }
    }
    return 0;
}

static void recycle_output(void)
{
    for (;;) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
            .m.planes = planes,
            .length = 1,
        };
        if (xioctl(D.fd, VIDIOC_DQBUF, &buf) < 0)
            return;
        if (buf.index < OUT_BUFS)
            D.out_free[buf.index] = 1;
    }
}

/* Dequeues decoded frames, hands them to the display, requeues. */
static int drain_capture(void)
{
    if (!D.cap_ready)
        return 0;
    for (;;) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory = V4L2_MEMORY_MMAP,
            .m.planes = planes,
            .length = 1,
        };
        if (xioctl(D.fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EPIPE)
                D.saw_last = 1;    /* drained past the STOP point */
            return 0;
        }
        if (buf.index >= D.cap_nbufs)
            return -1;

        if (planes[0].bytesused > 0) {
            struct v4l2dec_frame f = {
                .data = (const uint8_t *)D.cap_map[buf.index],
                .fourcc = D.cap_fourcc,
                .w = D.vis_w,
                .h = D.vis_h,
                .stride = D.cap_stride,
                .coded_h = D.coded_h,
            };
            v4l2dec_emit(&f);
        }
        if (buf.flags & V4L2_BUF_FLAG_LAST)
            D.saw_last = 1;

        if (xioctl(D.fd, VIDIOC_QBUF, &buf) < 0)
            return -1;
    }
}

int v4l2dec_pump(void)
{
    if (D.fd < 0)
        return 0;
    if (handle_events() < 0)
        return -1;
    recycle_output();
    return drain_capture();
}

int v4l2dec_feed(const uint8_t *au, size_t len)
{
    if (D.fd < 0 || len == 0)
        return -1;
    if (len > D.out_len[0]) {
        declog("access unit too large (%zu bytes)", len);
        return -1;
    }

    /* find a free OUTPUT buffer, pumping the decoder while we wait —
     * this is the natural back-pressure when the decoder lags */
    int idx = -1;
    for (int tries = 0; tries < 500 && !g_stop; tries++) {
        recycle_output();
        for (int i = 0; i < OUT_BUFS; i++)
            if (D.out_free[i]) {
                idx = i;
                break;
            }
        if (idx >= 0)
            break;
        struct pollfd p = { .fd = D.fd, .events = POLLIN | POLLOUT | POLLPRI };
        if (poll(&p, 1, 10) < 0 && errno != EINTR)
            return -1;
        if (v4l2dec_pump() < 0)
            return -1;
    }
    if (idx < 0) {
        declog("no free input buffer (decoder wedged?)");
        return -1;
    }

    memcpy(D.out_map[idx], au, len);
    D.out_free[idx] = 0;

    struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
    planes[0].bytesused = len;
    struct v4l2_buffer buf = {
        .index = (unsigned)idx,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
        .m.planes = planes,
        .length = 1,
    };
    if (xioctl(D.fd, VIDIOC_QBUF, &buf) < 0) {
        declog("QBUF(OUTPUT) failed: %s", strerror(errno));
        D.out_free[idx] = 1;
        return -1;
    }

    /* the first AU triggers SOURCE_CHANGE: wait for the capture side so
     * decoded frames start flowing (and fail fast if they never do) */
    if (!D.cap_ready) {
        for (int tries = 0; tries < 400 && !D.cap_ready && !g_stop; tries++) {
            struct pollfd p = { .fd = D.fd, .events = POLLIN | POLLPRI };
            if (poll(&p, 1, 10) < 0 && errno != EINTR)
                return -1;
            if (handle_events() < 0)
                return -1;
        }
        if (!D.cap_ready) {
            declog("decoder never reported the stream format");
            return -1;
        }
    }
    return v4l2dec_pump();
}

/* --------------------------------------------------------------- EOS */

int v4l2dec_drain(void)
{
    if (D.fd < 0 || !D.cap_ready)
        return 0;

    struct v4l2_decoder_cmd cmd = { .cmd = V4L2_DEC_CMD_STOP };
    if (xioctl(D.fd, VIDIOC_DECODER_CMD, &cmd) < 0) {
        declog("DECODER_CMD STOP failed: %s", strerror(errno));
        return -1;
    }

    D.saw_last = 0;
    for (int tries = 0; tries < 500 && !D.saw_last && !g_stop; tries++) {
        struct pollfd p = { .fd = D.fd, .events = POLLIN | POLLPRI };
        if (poll(&p, 1, 10) < 0 && errno != EINTR)
            return -1;
        if (v4l2dec_pump() < 0)
            return -1;
    }
    if (!D.saw_last) {
        declog("drain timed out");
        return -1;
    }
    D.saw_last = 0;

    cmd.cmd = V4L2_DEC_CMD_START;
    if (xioctl(D.fd, VIDIOC_DECODER_CMD, &cmd) < 0) {
        declog("DECODER_CMD START failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------- close */

void v4l2dec_close(void)
{
    if (D.fd < 0)
        return;
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(D.fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(D.fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < OUT_BUFS; i++)
        if (D.out_map[i]) {
            munmap(D.out_map[i], D.out_len[i]);
            D.out_map[i] = NULL;
        }
    for (unsigned i = 0; i < D.cap_nbufs && i < CAP_BUFS_MAX; i++)
        if (D.cap_map[i]) {
            munmap(D.cap_map[i], D.cap_len[i]);
            D.cap_map[i] = NULL;
        }
    close(D.fd);
    D.fd = -1;
    D.cap_ready = 0;
    D.cap_nbufs = 0;
}
