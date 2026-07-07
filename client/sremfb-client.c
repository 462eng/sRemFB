/*
 * sremfb-client — receives frames over TCP and writes them to /dev/fb0.
 *
 * Plain C, no dependencies beyond libc (LZ4 is statically linked in the
 * Debian packages). Builds and runs on ARMv6/v7 SBCs (Banana Pi M1+,
 * Raspberry Pi 1/2) as well as x86 for testing.
 *
 * The client sends its MAC address in the hello: the server derives the
 * monitor identity from it, so each physical client keeps its own
 * remembered position on the desktop. The panel is blanked (FBIOBLANK)
 * whenever there is no server — like a monitor with no signal — and when
 * the server says so (DPMS pass-through).
 *
 * Config (environment, overridable by argv):
 *   SREMFB_SERVER      server host/IP (required; or argv[1])
 *   SREMFB_PORT        TCP port, default 4629 (or argv[2])
 *   SREMFB_FBDEV       framebuffer device, default /dev/fb0
 *   SREMFB_TTY         VT to switch to and put in graphics mode; prefer one
 *                      with no getty (e.g. /dev/tty7), default /dev/tty1
 *   SREMFB_WRITE_MODE  "mmap" (default) or "pwrite" (deferred-io workaround)
 *   SREMFB_MAC         override the announced MAC (aa:bb:cc:dd:ee:ff)
 *   SREMFB_MODEL       override the announced panel model (13 chars max;
 *                      default: "vendor model" from the attached panel's
 *                      EDID via /sys/class/drm)
 *   SREMFB_NO_H264     don't advertise H.264 decoding even if a V4L2
 *                      hardware decoder is present
 *   SREMFB_NO_HOTPLUG  don't watch the panel's DRM connector (see below)
 *
 * The client always advertises FEEDBACK (it echoes the server's PING
 * messages, giving the server its congestion-delay signal) and, when a
 * V4L2 stateful M2M H.264 decoder is found (Raspberry Pi: /dev/video10),
 * the H264 capability — the server then switches the stream to H.264
 * under measured congestion. Any decoder failure disables the capability
 * and reconnects on the plain RAW/LZ4 path.
 *
 * Panel hotplug: if a connected DRM connector exists at startup, the
 * client watches it (~2 s). Unplugging the panel closes the connection —
 * the server unplugs the virtual monitor, exactly like pulling the cable
 * of a real screen — and replugging reconnects (a different panel means a
 * different EDID model, hence a new monitor identity).
 *
 * Test mode (runs anywhere, no framebuffer needed):
 *   sremfb-client --test WxH [server] [port]
 *   Announces WxH @ 32bpp XRGB8888 and dumps every 30th frame to
 *   sremfb-test-NNNN.ppm instead of writing to a framebuffer.
 *   With SREMFB_TEST_H264_SINK=1 it also advertises H264 without
 *   decoding: access units are appended to sremfb-test.h264 (pure
 *   Annex B, playable with ffplay) + sremfb-test.h264.len (u32 LE
 *   sizes, one per AU — the input of --decode-test).
 *
 * Decoder bring-up (hidden): sremfb-client --decode-test FILE.h264
 *   Replays a sink capture through the V4L2 decoder straight to the
 *   framebuffer, no network involved.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/videodev2.h>
#include <linux/vt.h>

#include <lz4.h>

#include "usbexport.h"

#include "protocol.h"
#include "v4l2dec.h"

volatile sig_atomic_t g_stop = 0;      /* shared with v4l2dec.c */

static struct {
    const char *server;
    const char *port;
    const char *fbdev;
    const char *tty;
    int use_pwrite;
    int no_lz4;
    int no_h264;
    int usb_active;            /* usbexport.c bound our USB devices */

    int test_mode;
    unsigned test_w, test_h;
    int test_sink;             /* --test + SREMFB_TEST_H264_SINK */

    /* adaptive H.264 */
    int dec_found;             /* v4l2dec_probe() succeeded */
    int dec_open;              /* decoder initialized this session */
    int h264_broken;           /* runtime failure: stop advertising */
    uint8_t srv_flags;         /* from the server hello */
    uint8_t *rowbuf;           /* one converted row (YUV -> fb format) */

    /* panel hotplug watcher */
    int hotplug_armed;         /* a connected DRM connector existed at start */

    /* framebuffer state */
    int fb_fd;
    uint8_t *fbmem;            /* NULL in pwrite/test mode */
    size_t fb_size;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    uint8_t pixfmt;
    unsigned bytespp;
    int blanked;               /* panel currently blanked (FBIOBLANK) */

    /* console state */
    int tty_fd;
    int tty_restore_needed;

    /* negotiated stream geometry and its placement in the fb (centered) */
    unsigned stream_w, stream_h;
    int off_x, off_y;

    /* test mode frame accumulator */
    uint8_t *testbuf;
    unsigned frame_count;
} C;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "sremfb-client: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---------------------------------------------------------------- io */

static int readn(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n == 0)
            return -1;                      /* peer closed */
        if (n < 0) {
            if (errno == EINTR) {
                if (g_stop)
                    return -1;
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int writen(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_stop)
                    return -1;
                continue;
            }
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ------------------------------------------------------ framebuffer */

static int fb_open(void)
{
    C.fb_fd = open(C.fbdev, O_RDWR);
    if (C.fb_fd < 0) {
        logmsg("cannot open %s: %s", C.fbdev, strerror(errno));
        return -1;
    }
    if (ioctl(C.fb_fd, FBIOGET_VSCREENINFO, &C.vinfo) < 0 ||
        ioctl(C.fb_fd, FBIOGET_FSCREENINFO, &C.finfo) < 0) {
        logmsg("FBIOGET_*SCREENINFO failed on %s: %s", C.fbdev, strerror(errno));
        return -1;
    }

    if (C.vinfo.bits_per_pixel == 32 && C.vinfo.red.offset == 16 &&
        C.vinfo.green.offset == 8 && C.vinfo.blue.offset == 0) {
        C.pixfmt = SREMFB_PIX_XRGB8888;
        C.bytespp = 4;
    } else if (C.vinfo.bits_per_pixel == 16 && C.vinfo.red.offset == 11 &&
               C.vinfo.green.offset == 5 && C.vinfo.blue.offset == 0) {
        C.pixfmt = SREMFB_PIX_RGB565;
        C.bytespp = 2;
    } else {
        logmsg("unsupported fb format: %ubpp r@%u/%u g@%u/%u b@%u/%u "
               "(need 32bpp XRGB8888 or 16bpp RGB565)",
               C.vinfo.bits_per_pixel,
               C.vinfo.red.offset, C.vinfo.red.length,
               C.vinfo.green.offset, C.vinfo.green.length,
               C.vinfo.blue.offset, C.vinfo.blue.length);
        return -1;
    }

    logmsg("%s: %ux%u %ubpp (%s), stride %u",
           C.fbdev, C.vinfo.xres, C.vinfo.yres, C.vinfo.bits_per_pixel,
           C.pixfmt == SREMFB_PIX_XRGB8888 ? "XRGB8888" : "RGB565",
           C.finfo.line_length);

    if (!C.use_pwrite) {
        C.fb_size = C.finfo.smem_len;
        C.fbmem = mmap(NULL, C.fb_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, C.fb_fd, 0);
        if (C.fbmem == MAP_FAILED) {
            logmsg("mmap failed (%s), falling back to pwrite mode",
                   strerror(errno));
            C.fbmem = NULL;
            C.use_pwrite = 1;
        }
    }
    return 0;
}

static void fb_clear(void)
{
    if (C.fbmem) {
        memset(C.fbmem, 0, C.fb_size);
    } else if (C.fb_fd >= 0) {
        uint8_t zeros[4096] = {0};
        off_t total = (off_t)C.finfo.line_length * C.vinfo.yres;
        for (off_t off = 0; off < total; off += (off_t)sizeof(zeros)) {
            size_t n = sizeof(zeros);
            if (off + (off_t)n > total)
                n = (size_t)(total - off);
            if (pwrite(C.fb_fd, zeros, n, off) < 0)
                break;
        }
    }
}

/* Panel power: blanked while there is no server (no signal) and when the
 * server forwards a DPMS off. Falls back to painting black if the fbdev
 * driver cannot power the panel down. */
static void fb_set_blank(int on)
{
    on = !!on;
    if (on == C.blanked)
        return;
    C.blanked = on;
    if (C.test_mode) {
        logmsg("panel %s", on ? "blanked" : "unblanked");
        return;
    }
    if (C.fb_fd >= 0 &&
        ioctl(C.fb_fd, FBIOBLANK,
              on ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK) == 0)
        return;
    if (on)
        fb_clear();
}

/* Blit one row of stream pixels into the fb, clipping to the visible area.
 * sx,sy are stream coordinates; row is w pixels of C.bytespp bytes. */
static void fb_blit_row(unsigned sx, unsigned sy, unsigned w, const uint8_t *row)
{
    int dy = (int)sy + C.off_y;
    if (dy < 0 || dy >= (int)C.vinfo.yres)
        return;

    int dx = (int)sx + C.off_x;
    unsigned skip = 0;
    if (dx < 0) {
        skip = (unsigned)(-dx);
        if (skip >= w)
            return;
        dx = 0;
        w -= skip;
    }
    if ((unsigned)dx >= C.vinfo.xres)
        return;
    if ((unsigned)dx + w > C.vinfo.xres)
        w = C.vinfo.xres - (unsigned)dx;

    off_t off = (off_t)dy * C.finfo.line_length + (off_t)dx * C.bytespp;
    const uint8_t *src = row + (size_t)skip * C.bytespp;
    size_t len = (size_t)w * C.bytespp;

    if (C.fbmem)
        memcpy(C.fbmem + off, src, len);
    else
        pwrite(C.fb_fd, src, len, off);
}

/* ---------------------------------------------------------- console */

/* Extract the VT number from a /dev/ttyN path (e.g. "/dev/tty7" -> 7). */
static int vt_of(const char *tty)
{
    const char *e = tty + strlen(tty);
    while (e > tty && e[-1] >= '0' && e[-1] <= '9')
        e--;
    return *e ? atoi(e) : -1;
}

static void console_restore(void)
{
    if (C.tty_restore_needed && C.tty_fd >= 0) {
        ioctl(C.tty_fd, KDSETMODE, KD_TEXT);
        ioctl(C.tty_fd, VT_ACTIVATE, 1);   /* hand a login console back */
        C.tty_restore_needed = 0;
    }
}

static void console_grab(void)
{
    C.tty_fd = open(C.tty, O_RDWR);
    if (C.tty_fd < 0) {
        logmsg("cannot open %s (%s); trying fbcon fallback", C.tty,
               strerror(errno));
        int fd = open("/sys/class/graphics/fbcon/cursor_blink", O_WRONLY);
        if (fd >= 0) {
            if (write(fd, "0", 1) < 0)
                logmsg("could not disable fbcon cursor blink");
            close(fd);
        }
        return;
    }
    if (ioctl(C.tty_fd, KDSETMODE, KD_GRAPHICS) == 0) {
        C.tty_restore_needed = 1;
    } else {
        logmsg("KDSETMODE KD_GRAPHICS failed on %s: %s (console may show "
               "through)", C.tty, strerror(errno));
        /* best effort: kill blanking + cursor via escape sequences */
        const char esc[] = "\033[9;0]\033[?25l";
        if (write(C.tty_fd, esc, sizeof(esc) - 1) < 0)
            logmsg("could not write console escape sequences");
    }
    /* Bring our VT to the foreground: KD_GRAPHICS only silences fbcon on the
     * *active* VT, so if we booted onto another VT (e.g. a getty on tty1/2)
     * its blinking cursor keeps being drawn on the framebuffer. Use a VT with
     * no getty (beyond logind's NAutoVTs, e.g. tty7). */
    int vt = vt_of(C.tty);
    if (vt > 0 && (ioctl(C.tty_fd, VT_ACTIVATE, vt) < 0 ||
                   ioctl(C.tty_fd, VT_WAITACTIVE, vt) < 0))
        logmsg("could not switch to VT %d (%s): console may show through",
               vt, strerror(errno));
}

/* -------------------------------------------------------------- mac */

static int parse_mac(const char *s, uint8_t mac[6])
{
    unsigned v[6];

    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return -1;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF)
            return -1;
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}

/* MAC of the interface the connected socket goes through: local address
 * (getsockname) -> owning interface (getifaddrs) -> SIOCGIFHWADDR. */
static void discover_mac(int sock, uint8_t mac[6])
{
    memset(mac, 0, 6);

    const char *env = getenv("SREMFB_MAC");
    if (env) {
        if (parse_mac(env, mac) == 0)
            return;
        logmsg("ignoring malformed SREMFB_MAC \"%s\"", env);
    }
    if (C.test_mode) {
        /* fixed locally-administered MAC, so repeated test runs reuse one
         * monitor identity; override with SREMFB_MAC for a second client */
        static const uint8_t test_mac[6] = { 0x02, 0x46, 0x42, 0, 0, 1 };
        memcpy(mac, test_mac, 6);
        return;
    }

    struct sockaddr_storage local;
    socklen_t sl = sizeof(local);
    if (getsockname(sock, (struct sockaddr *)&local, &sl) < 0)
        return;

    struct ifaddrs *ifas, *ifa;
    char name[IFNAMSIZ] = "";
    if (getifaddrs(&ifas) < 0)
        return;
    for (ifa = ifas; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != local.ss_family)
            continue;
        if (local.ss_family == AF_INET) {
            if (((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr !=
                ((struct sockaddr_in *)&local)->sin_addr.s_addr)
                continue;
        } else if (local.ss_family == AF_INET6) {
            if (memcmp(&((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr,
                       &((struct sockaddr_in6 *)&local)->sin6_addr, 16) != 0)
                continue;
        } else {
            continue;
        }
        snprintf(name, sizeof(name), "%s", ifa->ifa_name);
        break;
    }
    freeifaddrs(ifas);
    if (!name[0])
        return;

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return;
    struct ifreq ifr = {0};
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
    if (ioctl(s, SIOCGIFHWADDR, &ifr) == 0)
        memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    close(s);
}

/* ------------------------------------------------------ panel watch */

/* Is any DRM connector "connected"? 1 = yes, 0 = no (panel unplugged),
 * -1 = no DRM at all (watcher stays disabled). "unknown" statuses are
 * ignored — only an explicit "disconnected" everywhere counts as gone. */
static int panel_present(void)
{
    DIR *dir = opendir("/sys/class/drm");
    struct dirent *de;
    int have_conn = 0, connected = 0;

    if (!dir)
        return -1;
    while ((de = readdir(dir))) {
        char path[512], status[16] = "";
        if (strncmp(de->d_name, "card", 4) != 0 || !strchr(de->d_name, '-'))
            continue;
        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (!fgets(status, sizeof(status), f))
            status[0] = '\0';
        fclose(f);
        have_conn = 1;
        if (strncmp(status, "connected", 9) == 0 ||
            strncmp(status, "unknown", 7) == 0)
            connected = 1;
    }
    closedir(dir);
    if (!have_conn)
        return -1;
    return connected;
}

/* ---------------------------------------------------- decoded frames */

static inline uint8_t clamp255u(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* BT.601 limited range, matching the server's encoder VUI. */
static void yuv_row_to_fb(uint8_t *dst, const uint8_t *yrow,
                          const uint8_t *urow, const uint8_t *vrow,
                          unsigned uv_step, unsigned w)
{
    for (unsigned i = 0; i < w; i++) {
        int c = 298 * ((int)yrow[i] - 16);
        int du = (int)urow[(i / 2) * uv_step] - 128;
        int dv = (int)vrow[(i / 2) * uv_step] - 128;
        int r = clamp255u((c + 409 * dv + 128) >> 8);
        int g = clamp255u((c - 100 * du - 208 * dv + 128) >> 8);
        int b = clamp255u((c + 516 * du + 128) >> 8);
        if (C.bytespp == 2) {
            ((uint16_t *)dst)[i] =
                (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        } else {
            ((uint32_t *)dst)[i] =
                ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

/* Blit one decoded frame to the framebuffer: RGB565 direct rows, or
 * CPU YUV conversion row by row. */
static void frame_to_fb(const struct v4l2dec_frame *f)
{
    unsigned w = f->w < C.stream_w ? f->w : C.stream_w;
    unsigned h = f->h < C.stream_h ? f->h : C.stream_h;

    if (f->fourcc == V4L2_PIX_FMT_RGB565 && C.bytespp == 2) {
        for (unsigned y = 0; y < h; y++)
            fb_blit_row(0, y, w, f->data + (size_t)y * f->stride);
    } else if (C.rowbuf) {
        const uint8_t *luma = f->data;
        const uint8_t *chroma = f->data + (size_t)f->stride * f->coded_h;
        for (unsigned y = 0; y < h; y++) {
            const uint8_t *u, *v;
            unsigned step;
            if (f->fourcc == V4L2_PIX_FMT_NV12) {
                const uint8_t *uv = chroma + (size_t)(y / 2) * f->stride;
                u = uv;
                v = uv + 1;
                step = 2;
            } else {                       /* 'YU12' planar YUV420 */
                unsigned cs = f->stride / 2;
                u = chroma + (size_t)(y / 2) * cs;
                v = chroma + (size_t)(f->coded_h / 2) * cs +
                    (size_t)(y / 2) * cs;
                step = 1;
            }
            yuv_row_to_fb(C.rowbuf, luma + (size_t)y * f->stride, u, v,
                          step, w);
            fb_blit_row(0, y, w, C.rowbuf);
        }
    }
    if (C.blanked)
        fb_set_blank(0);
}

/*
 * The blit runs in its own thread behind a one-frame mailbox. Writing a
 * full 1080p frame into an uncached framebuffer takes tens of ms on a
 * Pi 3; done inline it back-pressures the decoder and the TCP stream
 * until whole seconds of display lag pile up in the queues. Here the
 * decode path always hands over the newest frame — one arriving while
 * the previous still waits simply replaces it (frame drop, at whatever
 * rate the blit sustains) — so the network and decoder never block on
 * the display, and a second core absorbs the fb writes.
 */
static pthread_mutex_t blit_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t blit_cv = PTHREAD_COND_INITIALIZER;
static struct v4l2dec_frame blit_next;
static int blit_have;                  /* a frame waits in the mailbox */
static int blit_busy;                  /* the thread is blitting one */
static int blit_up = -1;               /* -1 not started, 0 failed, 1 up */

static void blit_wait(void)            /* 200 ms cap: notice g_stop */
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 200 * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&blit_cv, &blit_mtx, &ts);
}

static void *blit_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&blit_mtx);
    while (!g_stop) {
        if (!blit_have) {
            blit_wait();
            continue;
        }
        struct v4l2dec_frame f = blit_next;
        blit_have = 0;
        blit_busy = 1;
        pthread_mutex_unlock(&blit_mtx);

        frame_to_fb(&f);
        v4l2dec_release(f.buf_index);

        pthread_mutex_lock(&blit_mtx);
        blit_busy = 0;
        pthread_cond_broadcast(&blit_cv);
    }
    pthread_mutex_unlock(&blit_mtx);
    return NULL;
}

/* Waits until the blit thread has nothing left to show. Required before
 * the main thread writes the fb again (the RAW snapshot that closes an
 * H.264 episode) and before the capture buffers go away (close). */
static void blit_sync(void)
{
    if (blit_up != 1)
        return;
    pthread_mutex_lock(&blit_mtx);
    while ((blit_have || blit_busy) && !g_stop)
        blit_wait();
    pthread_mutex_unlock(&blit_mtx);
}

/* Callback from v4l2dec.c: one decoded frame -> display. */
int v4l2dec_emit(const struct v4l2dec_frame *f)
{
    if (blit_up < 0) {
        pthread_t t;
        blit_up = pthread_create(&t, NULL, blit_thread, NULL) == 0;
        if (blit_up)
            pthread_detach(t);
        else
            logmsg("cannot start blit thread, blitting inline");
    }
    if (!blit_up) {
        frame_to_fb(f);
        return 0;
    }
    pthread_mutex_lock(&blit_mtx);
    if (blit_have)                     /* still unshown: drop it */
        v4l2dec_release(blit_next.buf_index);
    blit_next = *f;
    blit_have = 1;
    pthread_cond_broadcast(&blit_cv);
    pthread_mutex_unlock(&blit_mtx);
    return 1;
}

/* ------------------------------------------------------- panel edid */

/* Copies up to 13 printable chars into model[13] (not NUL-terminated
 * when full, per the wire format). */
static void model_copy(char model[13], const char *s)
{
    int n = 0;

    while (n < 13 && s[n] >= 0x20 && s[n] <= 0x7E)
        n++;
    while (n > 0 && s[n - 1] == ' ')
        n--;
    memcpy(model, s, (size_t)n);
}

/* "vendor model" of the panel attached to this SBC, from the EDID of the
 * first connected DRM connector. The server uses it as the virtual
 * monitor's model name, so the remote screen is recognizable in GNOME. */
static void discover_model(char model[13])
{
    memset(model, 0, 13);

    const char *env = getenv("SREMFB_MODEL");
    if (env && *env) {
        model_copy(model, env);
        return;
    }
    if (C.test_mode)
        return;                 /* server falls back to its own name */

    DIR *dir = opendir("/sys/class/drm");
    if (!dir)
        return;
    struct dirent *de;
    uint8_t e[128];
    int have = 0;
    while (!have && (de = readdir(dir))) {
        char path[512], status[16] = "";
        if (strncmp(de->d_name, "card", 4) != 0 || !strchr(de->d_name, '-'))
            continue;
        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (!fgets(status, sizeof(status), f))
            status[0] = '\0';
        fclose(f);
        if (strncmp(status, "connected", 9) != 0)
            continue;
        snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", de->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;
        have = fread(e, 1, sizeof(e), f) == sizeof(e) &&
               e[0] == 0x00 && e[1] == 0xFF && e[7] == 0x00;
        fclose(f);
    }
    closedir(dir);
    if (!have)
        return;

    char vendor[4], name[14] = "";
    vendor[0] = (char)('A' - 1 + ((e[8] >> 2) & 0x1F));
    vendor[1] = (char)('A' - 1 + (((e[8] & 3) << 3) | (e[9] >> 5)));
    vendor[2] = (char)('A' - 1 + (e[9] & 0x1F));
    vendor[3] = '\0';

    for (int off = 54; off <= 108; off += 18) {
        if (e[off] || e[off + 1] || e[off + 3] != 0xFC)
            continue;
        int n = 0;
        while (n < 13 && e[off + 5 + n] != 0x0A &&
               e[off + 5 + n] >= 0x20 && e[off + 5 + n] <= 0x7E) {
            name[n] = (char)e[off + 5 + n];
            n++;
        }
        while (n > 0 && name[n - 1] == ' ')
            n--;
        name[n] = '\0';
        break;
    }

    char full[32];
    if (name[0])
        snprintf(full, sizeof(full), "%s %s", vendor, name);
    else
        snprintf(full, sizeof(full), "%s %02X%02X", vendor, e[11], e[10]);
    model_copy(model, full);
}

/* -------------------------------------------------------- test mode */

static void test_dump_ppm(void)
{
    char name[64];
    snprintf(name, sizeof(name), "sremfb-test-%04u.ppm", C.frame_count / 30);
    FILE *f = fopen(name, "w");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", C.stream_w, C.stream_h);
    for (size_t i = 0; i < (size_t)C.stream_w * C.stream_h; i++) {
        const uint8_t *px = C.testbuf + i * 4;   /* B,G,R,X */
        fputc(px[2], f);
        fputc(px[1], f);
        fputc(px[0], f);
    }
    fclose(f);
    logmsg("wrote %s", name);
}

/* ---------------------------------------------------------- network */

static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints = {0}, *res = NULL, *ai;
    int fd = -1;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        logmsg("resolve %s: %s", host, gai_strerror(rc));
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1, idle = 10, intvl = 5, cnt = 3, user_to = 6000;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        /* tuned like the server (~25 s): without these the kernel
         * defaults are 2 hours — a dead server with a static screen
         * would leave the stale image on the panel that long instead
         * of dropping to "no signal" */
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
        /* and our writes (hello, pongs) must not linger unACKed either */
        setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_to,
                   sizeof(user_to));
    }
    return fd;
}

static int hello_exchange(int fd)
{
    struct sremfb_client_hello ch = {0};
    struct sremfb_server_hello sh;

    ch.magic = SREMFB_MAGIC;
    ch.proto_ver = SREMFB_PROTO_VER;
    if (!C.no_lz4)
        ch.flags |= SREMFB_HELLO_FLAG_LZ4;
    ch.flags |= SREMFB_HELLO_FLAG_FEEDBACK;
    if ((C.dec_found && !C.no_h264 && !C.h264_broken && !C.test_mode) ||
        C.test_sink)
        ch.flags |= SREMFB_HELLO_FLAG_H264;
    if (C.usb_active)
        ch.flags |= SREMFB_HELLO_FLAG_USB;
    discover_mac(fd, ch.mac);
    discover_model(ch.model);
    if (C.test_mode) {
        ch.xres = (uint16_t)C.test_w;
        ch.yres = (uint16_t)C.test_h;
        ch.bpp = 32;
        ch.pixfmt = SREMFB_PIX_XRGB8888;
        ch.red_off = 16;  ch.red_len = 8;
        ch.green_off = 8; ch.green_len = 8;
        ch.blue_off = 0;  ch.blue_len = 8;
    } else {
        ch.xres = (uint16_t)C.vinfo.xres;
        ch.yres = (uint16_t)C.vinfo.yres;
        ch.bpp = (uint8_t)C.vinfo.bits_per_pixel;
        ch.pixfmt = C.pixfmt;
        ch.red_off = (uint8_t)C.vinfo.red.offset;
        ch.red_len = (uint8_t)C.vinfo.red.length;
        ch.green_off = (uint8_t)C.vinfo.green.offset;
        ch.green_len = (uint8_t)C.vinfo.green.length;
        ch.blue_off = (uint8_t)C.vinfo.blue.offset;
        ch.blue_len = (uint8_t)C.vinfo.blue.length;
    }

    if (writen(fd, &ch, sizeof(ch)) < 0) {
        logmsg("failed to send hello: %s", strerror(errno));
        return -1;
    }
    if (readn(fd, &sh, sizeof(sh)) < 0) {
        logmsg("failed to read server hello");
        return -1;
    }
    if (sh.magic != SREMFB_MAGIC || sh.proto_ver != SREMFB_PROTO_VER) {
        logmsg("bad server hello (magic/version mismatch)");
        return -1;
    }
    if (sh.status != SREMFB_STATUS_OK) {
        logmsg("server refused: status %u%s", sh.status,
               sh.status == SREMFB_STATUS_NO_DEVICE ?
               " (no free screen on the server)" : "");
        return -1;
    }
    unsigned expect_fmt = C.test_mode ? SREMFB_PIX_XRGB8888 : C.pixfmt;
    if (sh.pixfmt != expect_fmt) {
        logmsg("server offers pixfmt %u, we need %u", sh.pixfmt, expect_fmt);
        return -1;
    }

    C.stream_w = sh.width;
    C.stream_h = sh.height;
    C.srv_flags = sh.flags;
    if (C.srv_flags & SREMFB_SRV_FLAG_H264) {
        free(C.rowbuf);
        C.rowbuf = malloc((size_t)C.stream_w * (C.bytespp ? C.bytespp : 4));
    }
    if (C.test_mode) {
        C.off_x = C.off_y = 0;
        free(C.testbuf);
        C.testbuf = calloc((size_t)C.stream_w * C.stream_h, 4);
        if (!C.testbuf) {
            logmsg("out of memory");
            return -1;
        }
    } else {
        C.off_x = ((int)C.vinfo.xres - (int)C.stream_w) / 2;
        C.off_y = ((int)C.vinfo.yres - (int)C.stream_h) / 2;
        if (C.stream_w != C.vinfo.xres || C.stream_h != C.vinfo.yres)
            logmsg("stream %ux%u != fb %ux%u, centering",
                   C.stream_w, C.stream_h, C.vinfo.xres, C.vinfo.yres);
    }
    logmsg("connected: stream %ux%u (mac %02x:%02x:%02x:%02x:%02x:%02x)",
           C.stream_w, C.stream_h, ch.mac[0], ch.mac[1], ch.mac[2],
           ch.mac[3], ch.mac[4], ch.mac[5]);
    return 0;
}

static void emit_row(unsigned sx, unsigned sy, unsigned w, const uint8_t *row)
{
    if (C.test_mode) {
        size_t off = ((size_t)sy * C.stream_w + sx) * 4;
        memcpy(C.testbuf + off, row, (size_t)w * 4);
    } else {
        fb_blit_row(sx, sy, w, row);
    }
}

#define AU_CAP (1u << 20)              /* matches the decoder buffers */

/* One H.264 access unit: sink it (test), or decode it. Returns -1 to
 * drop the connection (h264_broken set for decoder failures). */
static int handle_h264_au(const uint8_t *au, size_t len)
{
    if (C.test_sink) {
        static FILE *raw, *lens;
        if (!raw) {
            raw = fopen("sremfb-test.h264", "wb");
            lens = fopen("sremfb-test.h264.len", "wb");
            logmsg("H.264 sink: writing sremfb-test.h264(.len)");
        }
        if (!raw || !lens)
            return -1;
        uint32_t l32 = (uint32_t)len;
        fwrite(au, 1, len, raw);
        fwrite(&l32, 4, 1, lens);
        fflush(raw);
        fflush(lens);
        return 0;
    }

    if (!C.dec_open) {
        if (v4l2dec_open(C.stream_w, C.stream_h,
                         C.pixfmt == SREMFB_PIX_RGB565) < 0) {
            logmsg("H.264 decoder init failed — reconnecting without it");
            C.h264_broken = 1;
            return -1;
        }
        C.dec_open = 1;
    }
    if (v4l2dec_feed(au, len) < 0) {
        logmsg("H.264 decode failed — reconnecting without it");
        C.h264_broken = 1;
        return -1;
    }
    return 0;
}

/* Receive frames until error/EOF. Returns only on disconnect/stop. */
static void frame_loop(int fd)
{
    unsigned bytespp = C.test_mode ? 4 : C.bytespp;
    size_t frame_cap = (size_t)C.stream_w * C.stream_h * bytespp;
    size_t comp_cap = (size_t)LZ4_compressBound((int)frame_cap);
    uint8_t *framebuf = malloc(frame_cap ? frame_cap : 1);
    uint8_t *compbuf = C.no_lz4 ? NULL : malloc(comp_cap);
    uint8_t *aubuf = NULL;
    time_t last_panel_check = time(NULL);
    time_t last_rx = time(NULL);

    if (!framebuf || (!C.no_lz4 && !compbuf)) {
        logmsg("out of memory");
        goto out;
    }

    while (!g_stop) {
        /* Wait for socket data, decoder activity, or the panel-check
         * tick. Message bodies still use blocking reads: they follow
         * their header immediately. */
        struct pollfd pfd[2] = {
            { .fd = fd, .events = POLLIN },
            { .fd = v4l2dec_fd(), .events = POLLIN | POLLPRI },
        };
        int nf = pfd[1].fd >= 0 ? 2 : 1;
        int pr = poll(pfd, nf, 2000);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (time(NULL) - last_panel_check >= 2) {
            last_panel_check = time(NULL);
            if (C.hotplug_armed && panel_present() == 0) {
                logmsg("panel disconnected — unplugging from the server");
                break;
            }
            usb_export_tick();      /* bind USB devices plugged meanwhile */
        }
        /* liveness: a server that negotiated PING sends a heartbeat at
         * least every ~2 s even when the screen is static, so a long
         * silence means it (or the path) is gone — drop to "no signal"
         * now instead of waiting for the TCP keepalive. */
        if ((C.srv_flags & SREMFB_SRV_FLAG_PING) &&
            time(NULL) - last_rx > 6) {
            logmsg("server silent for 6 s — dropping to reconnect");
            break;
        }
        if (nf == 2 && pfd[1].revents) {
            if (v4l2dec_pump() < 0) {
                logmsg("H.264 decoder failed — reconnecting without it");
                C.h264_broken = 1;
                break;
            }
        }
        if (!(pfd[0].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;

        struct sremfb_frame_hdr hdr;
        if (readn(fd, &hdr, sizeof(hdr)) < 0)
            break;
        if (hdr.magic != SREMFB_MAGIC) {
            logmsg("bad frame magic 0x%08x, dropping connection", hdr.magic);
            break;
        }
        last_rx = time(NULL);

        if (hdr.encoding == SREMFB_ENC_BLANK ||
            hdr.encoding == SREMFB_ENC_UNBLANK) {
            if (hdr.payload_len != 0) {
                logmsg("bad control message payload len %u", hdr.payload_len);
                break;
            }
            fb_set_blank(hdr.encoding == SREMFB_ENC_BLANK);
            continue;
        }

        if (hdr.encoding == SREMFB_ENC_PING) {
            uint64_t t;
            if (hdr.payload_len != sizeof(t)) {
                logmsg("bad PING payload len %u", hdr.payload_len);
                break;
            }
            if (readn(fd, &t, sizeof(t)) < 0)
                break;
            struct sremfb_client_msg pong = {0};
            pong.magic = SREMFB_MAGIC;
            pong.type = SREMFB_CMSG_PONG;
            pong.t_echo_us = t;
            if (writen(fd, &pong, sizeof(pong)) < 0)
                break;
            continue;
        }

        if (hdr.encoding == SREMFB_ENC_H264) {
            if (hdr.payload_len == 0 || hdr.payload_len > AU_CAP) {
                logmsg("bad H.264 payload len %u", hdr.payload_len);
                break;
            }
            if (!aubuf && !(aubuf = malloc(AU_CAP))) {
                logmsg("out of memory");
                break;
            }
            if (readn(fd, aubuf, hdr.payload_len) < 0)
                break;
            if (handle_h264_au(aubuf, hdr.payload_len) < 0)
                break;
            continue;
        }

        if (hdr.encoding == SREMFB_ENC_H264_EOS) {
            if (hdr.payload_len != 0) {
                logmsg("bad H264_EOS payload len %u", hdr.payload_len);
                break;
            }
            if (C.test_sink) {
                logmsg("H.264 sink: episode over");
            } else if (C.dec_open) {
                if (v4l2dec_drain() < 0) {
                    logmsg("H.264 drain failed — reconnecting without it");
                    C.h264_broken = 1;
                    break;
                }
                /* the RAW snapshot follows: let the last decoded frame
                 * land before the main thread writes the fb again */
                blit_sync();
            }
            continue;
        }

        if (hdr.w == 0 || hdr.h == 0 ||
            (unsigned)hdr.x + hdr.w > C.stream_w ||
            (unsigned)hdr.y + hdr.h > C.stream_h) {
            logmsg("bad frame rect %ux%u+%u+%u", hdr.w, hdr.h, hdr.x, hdr.y);
            break;
        }
        size_t rect_bytes = (size_t)hdr.w * hdr.h * bytespp;

        if (hdr.encoding == SREMFB_ENC_RAW) {
            if (hdr.payload_len != rect_bytes) {
                logmsg("bad RAW payload len %u (rect %zu)",
                       hdr.payload_len, rect_bytes);
                break;
            }
            int fail = 0;
            for (unsigned row = 0; row < hdr.h; row++) {
                if (readn(fd, framebuf, (size_t)hdr.w * bytespp) < 0) {
                    fail = 1;
                    break;
                }
                emit_row(hdr.x, hdr.y + row, hdr.w, framebuf);
            }
            if (fail)
                break;
        } else if (hdr.encoding == SREMFB_ENC_LZ4 && compbuf) {
            if (hdr.payload_len == 0 || hdr.payload_len > comp_cap ||
                rect_bytes > frame_cap) {
                logmsg("bad LZ4 payload len %u", hdr.payload_len);
                break;
            }
            if (readn(fd, compbuf, hdr.payload_len) < 0)
                break;
            int n = LZ4_decompress_safe((const char *)compbuf,
                                        (char *)framebuf,
                                        (int)hdr.payload_len,
                                        (int)rect_bytes);
            if (n != (int)rect_bytes) {
                logmsg("LZ4 decode failed (%d, expected %zu)", n, rect_bytes);
                break;
            }
            for (unsigned row = 0; row < hdr.h; row++)
                emit_row(hdr.x, hdr.y + row, hdr.w,
                         framebuf + (size_t)row * hdr.w * bytespp);
        } else {
            logmsg("unsupported encoding %u", hdr.encoding);
            break;
        }

        /* pixels on screen: make sure the panel shows them */
        if (C.blanked)
            fb_set_blank(0);

        if (C.test_mode) {
            if (C.frame_count % 30 == 0)
                test_dump_ppm();
            C.frame_count++;
        }
    }
out:
    free(framebuf);
    free(compbuf);
    free(aubuf);
    if (C.dec_open) {
        blit_sync();               /* buffers are about to be unmapped */
        v4l2dec_close();
        C.dec_open = 0;
    }
}

/* ------------------------------------------------- decoder bring-up */

/* Replays a sink capture (FILE.h264 + FILE.h264.len) through the V4L2
 * decoder to the framebuffer at ~30 fps. No network. */
static int decode_test(const char *path)
{
    char lenpath[512];
    snprintf(lenpath, sizeof(lenpath), "%s.len", path);
    FILE *f = fopen(path, "rb");
    FILE *fl = fopen(lenpath, "rb");
    uint8_t *au = malloc(AU_CAP);
    uint32_t l32;
    int first = 1, rc = 1;
    unsigned n = 0;

    if (!f || !fl || !au) {
        logmsg("cannot open %s / %s", path, lenpath);
        goto out;
    }
    if (!v4l2dec_probe()) {
        logmsg("no V4L2 H.264 decoder on this machine");
        goto out;
    }
    C.stream_w = C.vinfo.xres;
    C.stream_h = C.vinfo.yres;
    C.rowbuf = malloc((size_t)C.stream_w * C.bytespp);

    while (!g_stop && fread(&l32, 4, 1, fl) == 1) {
        if (l32 == 0 || l32 > AU_CAP) {
            logmsg("bad AU length %u", l32);
            goto out;
        }
        if (fread(au, 1, l32, f) != l32) {
            logmsg("truncated %s", path);
            goto out;
        }
        if (first) {
            if (v4l2dec_open(C.vinfo.xres, C.vinfo.yres,
                             C.pixfmt == SREMFB_PIX_RGB565) < 0)
                goto out;
            first = 0;
        }
        if (v4l2dec_feed(au, l32) < 0)
            goto out;
        n++;
        usleep(33000);
    }
    if (!first && v4l2dec_drain() < 0)
        goto out;
    logmsg("decoded %u access units", n);
    rc = 0;
out:
    blit_sync();
    v4l2dec_close();
    if (f)
        fclose(f);
    if (fl)
        fclose(fl);
    free(au);
    return rc;
}

/* ------------------------------------------------------------- main */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--usb|--no-usb] [server] [port]\n"
            "       %s --test WxH [server] [port]\n"
            "env: SREMFB_SERVER SREMFB_PORT SREMFB_FBDEV SREMFB_TTY\n"
            "     SREMFB_WRITE_MODE SREMFB_MAC SREMFB_MODEL SREMFB_NO_LZ4\n"
            "     SREMFB_NO_H264 SREMFB_NO_HOTPLUG\n"
            "     SREMFB_USB SREMFB_USB_ALLOW SREMFB_USB_DENY\n",
            argv0, argv0);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *env;

    C.fb_fd = -1;
    C.tty_fd = -1;
    C.server = getenv("SREMFB_SERVER");
    C.port = (env = getenv("SREMFB_PORT")) ? env : NULL;
    C.fbdev = (env = getenv("SREMFB_FBDEV")) ? env : "/dev/fb0";
    C.tty = (env = getenv("SREMFB_TTY")) ? env : "/dev/tty1";
    env = getenv("SREMFB_WRITE_MODE");
    C.use_pwrite = env && strcmp(env, "pwrite") == 0;
    C.no_lz4 = getenv("SREMFB_NO_LZ4") != NULL;
    C.no_h264 = getenv("SREMFB_NO_H264") != NULL;

    const char *dectest = NULL;
    int usb_mode = -1;                  /* -1 auto, 0 off, 1 on */
    int argi = 1;
    while (argi < argc) {
        if (strcmp(argv[argi], "--usb") == 0)
            usb_mode = 1;
        else if (strcmp(argv[argi], "--no-usb") == 0)
            usb_mode = 0;
        else
            break;
        argi++;
    }
    if (argi < argc && strcmp(argv[argi], "--test") == 0) {
        argi++;
        if (argi >= argc ||
            sscanf(argv[argi], "%ux%u", &C.test_w, &C.test_h) != 2 ||
            C.test_w == 0 || C.test_h == 0 ||
            C.test_w > 16384 || C.test_h > 16384)
            usage(argv[0]);
        C.test_mode = 1;
        C.test_sink = getenv("SREMFB_TEST_H264_SINK") != NULL;
        argi++;
    } else if (argi + 1 < argc && strcmp(argv[argi], "--decode-test") == 0) {
        dectest = argv[argi + 1];
        argi += 2;
    }
    if (argi < argc)
        C.server = argv[argi++];
    if (argi < argc)
        C.port = argv[argi++];
    if (argi < argc || (!C.server && !dectest))
        usage(argv[0]);
    if (!C.port)
        C.port = "4629";

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;          /* no SA_RESTART: interrupt reads */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (dectest) {
        if (fb_open() < 0)
            return 1;
        return decode_test(dectest) ? 1 : 0;
    }

    if (!C.test_mode) {
        if (fb_open() < 0)
            return 1;
        console_grab();
        atexit(console_restore);
        fb_clear();
        fb_set_blank(1);                /* no signal yet */

        if (!C.no_h264)
            C.dec_found = v4l2dec_probe();
        C.hotplug_armed = getenv("SREMFB_NO_HOTPLUG") == NULL &&
                          panel_present() == 1;
        if (C.hotplug_armed)
            logmsg("watching the panel connector (SREMFB_NO_HOTPLUG=1 "
                   "to disable)");
        C.usb_active = usb_export_init(usb_mode);
    }

    unsigned backoff = 1;
    int panel_waiting = 0;
    while (!g_stop) {
        if (C.hotplug_armed && panel_present() == 0) {
            if (!panel_waiting) {
                logmsg("panel disconnected — waiting for it to come back");
                panel_waiting = 1;
            }
            sleep(2);
            continue;
        }
        if (panel_waiting) {
            logmsg("panel reconnected");
            panel_waiting = 0;
            backoff = 1;
        }

        int fd = tcp_connect(C.server, C.port);
        if (fd < 0) {
            logmsg("connect to %s:%s failed, retrying in %us",
                   C.server, C.port, backoff);
            sleep(backoff);
            if (backoff < 5)
                backoff++;
            continue;
        }

        if (hello_exchange(fd) == 0) {
            backoff = 1;
            frame_loop(fd);
            logmsg("disconnected");
        } else {
            /* refused/incompatible: don't hammer the server */
            if (backoff < 5)
                backoff++;
        }
        close(fd);
        if (!C.test_mode)
            fb_set_blank(1);            /* no signal: panel off */
        if (!g_stop)
            sleep(backoff);
    }

    logmsg("exiting");
    usb_export_stop();                  /* give the USB devices back */
    if (!C.test_mode) {
        fb_set_blank(0);                /* leave a usable console behind */
        fb_clear();
        console_restore();
    }
    if (C.fbmem)
        munmap(C.fbmem, C.fb_size);
    if (C.fb_fd >= 0)
        close(C.fb_fd);
    if (C.tty_fd >= 0)
        close(C.tty_fd);
    free(C.testbuf);
    free(C.rowbuf);
    return 0;
}
