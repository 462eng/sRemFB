/*
 * Framebuffer output backend (/dev/fb0), the default and legacy path.
 * Extracted verbatim from sremfb-client.c: mmap (or pwrite fallback),
 * FBIOBLANK for panel power, a VT grab so fbcon does not fight for the
 * screen, and the DRM-connector scan for panel hotplug.
 *
 * Config (unchanged): SREMFB_FBDEV (default /dev/fb0), SREMFB_WRITE_MODE
 * ("pwrite" for deferred-io framebuffers), SREMFB_TTY (VT to grab).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>

#include "protocol.h"
#include "output.h"

static void olog(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "sremfb-client: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

struct fbpriv {
    int fd;
    uint8_t *mem;              /* NULL in pwrite mode */
    size_t size;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    unsigned stride;
    unsigned bytespp;
    int use_pwrite;
    const char *fbdev;
    const char *tty;
    int tty_fd;
    int tty_restore;
};

static struct fbpriv FB = { .fd = -1, .tty_fd = -1 };

static int fb_open(struct sremfb_output *o, unsigned *w, unsigned *h,
                   uint8_t *pixfmt, unsigned *bytespp)
{
    const char *env;

    FB.fbdev = (env = getenv("SREMFB_FBDEV")) ? env : "/dev/fb0";
    env = getenv("SREMFB_WRITE_MODE");
    FB.use_pwrite = env && strcmp(env, "pwrite") == 0;

    FB.fd = open(FB.fbdev, O_RDWR);
    if (FB.fd < 0) {
        olog("cannot open %s: %s", FB.fbdev, strerror(errno));
        return -1;
    }
    if (ioctl(FB.fd, FBIOGET_VSCREENINFO, &FB.vinfo) < 0 ||
        ioctl(FB.fd, FBIOGET_FSCREENINFO, &FB.finfo) < 0) {
        olog("FBIOGET_*SCREENINFO failed on %s: %s", FB.fbdev, strerror(errno));
        return -1;
    }

    if (FB.vinfo.bits_per_pixel == 32 && FB.vinfo.red.offset == 16 &&
        FB.vinfo.green.offset == 8 && FB.vinfo.blue.offset == 0) {
        *pixfmt = SREMFB_PIX_XRGB8888;
        FB.bytespp = 4;
    } else if (FB.vinfo.bits_per_pixel == 16 && FB.vinfo.red.offset == 11 &&
               FB.vinfo.green.offset == 5 && FB.vinfo.blue.offset == 0) {
        *pixfmt = SREMFB_PIX_RGB565;
        FB.bytespp = 2;
    } else {
        olog("unsupported fb format: %ubpp r@%u/%u g@%u/%u b@%u/%u "
             "(need 32bpp XRGB8888 or 16bpp RGB565)",
             FB.vinfo.bits_per_pixel,
             FB.vinfo.red.offset, FB.vinfo.red.length,
             FB.vinfo.green.offset, FB.vinfo.green.length,
             FB.vinfo.blue.offset, FB.vinfo.blue.length);
        return -1;
    }
    FB.stride = FB.finfo.line_length;

    olog("%s: %ux%u %ubpp (%s), stride %u",
         FB.fbdev, FB.vinfo.xres, FB.vinfo.yres, FB.vinfo.bits_per_pixel,
         *pixfmt == SREMFB_PIX_XRGB8888 ? "XRGB8888" : "RGB565", FB.stride);

    if (!FB.use_pwrite) {
        FB.size = FB.finfo.smem_len;
        FB.mem = mmap(NULL, FB.size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, FB.fd, 0);
        if (FB.mem == MAP_FAILED) {
            olog("mmap failed (%s), falling back to pwrite mode",
                 strerror(errno));
            FB.mem = NULL;
            FB.use_pwrite = 1;
        }
    }

    o->priv = &FB;
    o->w = *w = FB.vinfo.xres;
    o->h = *h = FB.vinfo.yres;
    o->pixfmt = *pixfmt;
    o->bytespp = *bytespp = FB.bytespp;
    return 0;
}

static void fb_write_row(struct sremfb_output *o, unsigned dx, unsigned dy,
                         const uint8_t *src, unsigned npix)
{
    (void)o;
    off_t off = (off_t)dy * FB.stride + (off_t)dx * FB.bytespp;
    size_t len = (size_t)npix * FB.bytespp;

    if (FB.mem) {
        memcpy(FB.mem + off, src, len);
    } else {
        ssize_t r = pwrite(FB.fd, src, len, off);
        (void)r;               /* best effort, as before */
    }
}

static void fb_present(struct sremfb_output *o)
{
    (void)o;                   /* mmap/pwrite writes are already visible */
}

static void fb_clear(struct sremfb_output *o)
{
    (void)o;
    if (FB.mem) {
        memset(FB.mem, 0, FB.size);
    } else if (FB.fd >= 0) {
        uint8_t zeros[4096] = {0};
        off_t total = (off_t)FB.stride * FB.vinfo.yres;
        for (off_t off = 0; off < total; off += (off_t)sizeof(zeros)) {
            size_t n = sizeof(zeros);
            if (off + (off_t)n > total)
                n = (size_t)(total - off);
            if (pwrite(FB.fd, zeros, n, off) < 0)
                break;
        }
    }
}

static void fb_blank(struct sremfb_output *o, int on)
{
    if (FB.fd >= 0 &&
        ioctl(FB.fd, FBIOBLANK,
              on ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK) == 0)
        return;
    if (on)                    /* driver cannot power down: paint black */
        fb_clear(o);
}

/* Is any DRM connector "connected"? 1 yes, 0 no, -1 no DRM/no watcher. */
static int fb_panel_present(struct sremfb_output *o)
{
    DIR *dir = opendir("/sys/class/drm");
    struct dirent *de;
    int have_conn = 0, connected = 0;

    (void)o;
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

static int vt_of(const char *tty)
{
    const char *e = tty + strlen(tty);
    while (e > tty && e[-1] >= '0' && e[-1] <= '9')
        e--;
    return *e ? atoi(e) : -1;
}

static void fb_grab(struct sremfb_output *o)
{
    const char *env = getenv("SREMFB_TTY");

    (void)o;
    FB.tty = env ? env : "/dev/tty1";
    FB.tty_fd = open(FB.tty, O_RDWR);
    if (FB.tty_fd < 0) {
        olog("cannot open %s (%s); trying fbcon fallback", FB.tty,
             strerror(errno));
        int fd = open("/sys/class/graphics/fbcon/cursor_blink", O_WRONLY);
        if (fd >= 0) {
            if (write(fd, "0", 1) < 0)
                olog("could not disable fbcon cursor blink");
            close(fd);
        }
        return;
    }
    if (ioctl(FB.tty_fd, KDSETMODE, KD_GRAPHICS) == 0) {
        FB.tty_restore = 1;
    } else {
        olog("KDSETMODE KD_GRAPHICS failed on %s: %s (console may show "
             "through)", FB.tty, strerror(errno));
        const char esc[] = "\033[9;0]\033[?25l";
        if (write(FB.tty_fd, esc, sizeof(esc) - 1) < 0)
            olog("could not write console escape sequences");
    }
    int vt = vt_of(FB.tty);
    if (vt > 0 && (ioctl(FB.tty_fd, VT_ACTIVATE, vt) < 0 ||
                   ioctl(FB.tty_fd, VT_WAITACTIVE, vt) < 0))
        olog("could not switch to VT %d (%s): console may show through",
             vt, strerror(errno));
}

static void fb_ungrab(struct sremfb_output *o)
{
    (void)o;
    if (FB.tty_restore && FB.tty_fd >= 0) {
        ioctl(FB.tty_fd, KDSETMODE, KD_TEXT);
        ioctl(FB.tty_fd, VT_ACTIVATE, 1);
        FB.tty_restore = 0;
    }
}

static void fb_close(struct sremfb_output *o)
{
    (void)o;
    if (FB.mem) {
        munmap(FB.mem, FB.size);
        FB.mem = NULL;
    }
    if (FB.fd >= 0) {
        close(FB.fd);
        FB.fd = -1;
    }
    if (FB.tty_fd >= 0) {
        close(FB.tty_fd);
        FB.tty_fd = -1;
    }
}

const struct sremfb_output_ops sremfb_output_fb = {
    .name = "fb",
    .open = fb_open,
    .write_row = fb_write_row,
    .present = fb_present,
    .clear = fb_clear,
    .blank = fb_blank,
    .panel_present = fb_panel_present,
    .grab = fb_grab,
    .ungrab = fb_ungrab,
    .close = fb_close,
};
