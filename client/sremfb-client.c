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
 *   SREMFB_TTY         VT to switch to graphics mode, default /dev/tty1
 *   SREMFB_WRITE_MODE  "mmap" (default) or "pwrite" (deferred-io workaround)
 *   SREMFB_MAC         override the announced MAC (aa:bb:cc:dd:ee:ff)
 *   SREMFB_MODEL       override the announced panel model (13 chars max;
 *                      default: "vendor model" from the attached panel's
 *                      EDID via /sys/class/drm)
 *
 * Test mode (runs anywhere, no framebuffer needed):
 *   sremfb-client --test WxH [server] [port]
 *   Announces WxH @ 32bpp XRGB8888 and dumps every 30th frame to
 *   sremfb-test-NNNN.ppm instead of writing to a framebuffer.
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

#include <lz4.h>

#include "protocol.h"

static volatile sig_atomic_t g_stop = 0;

static struct {
    const char *server;
    const char *port;
    const char *fbdev;
    const char *tty;
    int use_pwrite;
    int no_lz4;

    int test_mode;
    unsigned test_w, test_h;

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

static void console_restore(void)
{
    if (C.tty_restore_needed && C.tty_fd >= 0) {
        ioctl(C.tty_fd, KDSETMODE, KD_TEXT);
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
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
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
        ch.flags = SREMFB_HELLO_FLAG_LZ4;
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

/* Receive frames until error/EOF. Returns only on disconnect/stop. */
static void frame_loop(int fd)
{
    unsigned bytespp = C.test_mode ? 4 : C.bytespp;
    size_t frame_cap = (size_t)C.stream_w * C.stream_h * bytespp;
    size_t comp_cap = (size_t)LZ4_compressBound((int)frame_cap);
    uint8_t *framebuf = malloc(frame_cap ? frame_cap : 1);
    uint8_t *compbuf = C.no_lz4 ? NULL : malloc(comp_cap);

    if (!framebuf || (!C.no_lz4 && !compbuf)) {
        logmsg("out of memory");
        goto out;
    }

    while (!g_stop) {
        struct sremfb_frame_hdr hdr;
        if (readn(fd, &hdr, sizeof(hdr)) < 0)
            break;
        if (hdr.magic != SREMFB_MAGIC) {
            logmsg("bad frame magic 0x%08x, dropping connection", hdr.magic);
            break;
        }

        if (hdr.encoding == SREMFB_ENC_BLANK ||
            hdr.encoding == SREMFB_ENC_UNBLANK) {
            if (hdr.payload_len != 0) {
                logmsg("bad control message payload len %u", hdr.payload_len);
                break;
            }
            fb_set_blank(hdr.encoding == SREMFB_ENC_BLANK);
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
}

/* ------------------------------------------------------------- main */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [server] [port]\n"
            "       %s --test WxH [server] [port]\n"
            "env: SREMFB_SERVER SREMFB_PORT SREMFB_FBDEV SREMFB_TTY\n"
            "     SREMFB_WRITE_MODE SREMFB_MAC SREMFB_MODEL SREMFB_NO_LZ4\n",
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

    int argi = 1;
    if (argi < argc && strcmp(argv[argi], "--test") == 0) {
        argi++;
        if (argi >= argc ||
            sscanf(argv[argi], "%ux%u", &C.test_w, &C.test_h) != 2 ||
            C.test_w == 0 || C.test_h == 0 ||
            C.test_w > 16384 || C.test_h > 16384)
            usage(argv[0]);
        C.test_mode = 1;
        argi++;
    }
    if (argi < argc)
        C.server = argv[argi++];
    if (argi < argc)
        C.port = argv[argi++];
    if (argi < argc || !C.server)
        usage(argv[0]);
    if (!C.port)
        C.port = "4629";

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;          /* no SA_RESTART: interrupt reads */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (!C.test_mode) {
        if (fb_open() < 0)
            return 1;
        console_grab();
        atexit(console_restore);
        fb_clear();
        fb_set_blank(1);                /* no signal yet */
    }

    unsigned backoff = 1;
    while (!g_stop) {
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
    return 0;
}
