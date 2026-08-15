/*
 * DRM/KMS output backend (opt-in, SREMFB_OUTPUT=drm), via libdrm.
 *
 * Legacy modeset only (drmModeSetCrtc) — no atomic — for the widest driver
 * compatibility (vc4, and Allwinner sun4i on the A20/Banana Pi, whose
 * kernel is older/patched).
 *
 * Two dumb buffers, selective page-flip: partial RAW damage rects are
 * written in place into the scanout buffer (like the fb mmap path —
 * nothing to reconcile), while full-frame batches (H.264 frames,
 * full-frame RAW/LZ4 snapshots) go to the back buffer and are published
 * with a page-flip — tear-free. The back buffer only ever receives full
 * frames, so it never leaks stale content; the centering borders are
 * black in both buffers from open(). If the second buffer or the flip
 * is unavailable, we degrade to the single-buffer in-place behaviour.
 *
 * The client picks the scanout format here, so a panel gets RGB565
 * (half the memory bandwidth and half the wire bytes, dithered by the
 * server) without touching the fbdev emulation depth or the kernel
 * cmdline: RG16 by default, XR24 (XRGB8888) as a fallback or via
 * SREMFB_DRM_DEPTH=32.
 *
 * Config: SREMFB_OUTPUT=drm, SREMFB_DRM_CARD (/dev/dri/cardN),
 * SREMFB_DRM_CONNECTOR (e.g. HDMI-A-1), SREMFB_DRM_MODE (WxH[@R]),
 * SREMFB_DRM_DEPTH (16|32). VT grab via SREMFB_TTY (shared idea with fb).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kd.h>
#include <linux/vt.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "protocol.h"
#include "output.h"

static void dlog(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "sremfb-client: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* Connector type -> sysfs name prefix (portable across libdrm versions). */
static const char *conn_type_name(uint32_t t)
{
    switch (t) {
    case DRM_MODE_CONNECTOR_VGA:         return "VGA";
    case DRM_MODE_CONNECTOR_DVII:        return "DVI-I";
    case DRM_MODE_CONNECTOR_DVID:        return "DVI-D";
    case DRM_MODE_CONNECTOR_Composite:   return "Composite";
    case DRM_MODE_CONNECTOR_LVDS:        return "LVDS";
    case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
    case DRM_MODE_CONNECTOR_HDMIA:       return "HDMI-A";
    case DRM_MODE_CONNECTOR_HDMIB:       return "HDMI-B";
    case DRM_MODE_CONNECTOR_TV:          return "TV";
    case DRM_MODE_CONNECTOR_eDP:         return "eDP";
    case DRM_MODE_CONNECTOR_DSI:         return "DSI";
    default:                             return "Unknown";
    }
}

struct drmbuf {
    uint32_t fb_id, handle;
    uint8_t *map;
    size_t size;
};

struct drmpriv {
    int fd;
    uint32_t conn_id, crtc_id;
    drmModeModeInfo mode;
    drmModeCrtc *saved;        /* to restore on close */
    struct drmbuf buf[2];      /* [1].map == NULL => single-buffer mode */
    int front, target;         /* scanout index, current write index */
    int no_flip;               /* flip broken/unavailable: stay in place */
    unsigned stride, bytespp;
    uint32_t dpms_prop;
    char sys_status[128];      /* /sys/class/drm/card<N>-<conn>/status */
    int tty_fd, tty_restore;
};

static struct drmpriv D = { .fd = -1, .tty_fd = -1 };

static uint32_t find_dpms_prop(int fd, uint32_t conn_id)
{
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
    uint32_t id = 0;

    if (!props)
        return 0;
    for (uint32_t i = 0; i < props->count_props && !id; i++) {
        drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
        if (p) {
            if (strcmp(p->name, "DPMS") == 0)
                id = p->prop_id;
            drmModeFreeProperty(p);
        }
    }
    drmModeFreeObjectProperties(props);
    return id;
}

/* Pick a CRTC that can drive this connector. */
static uint32_t find_crtc(int fd, drmModeRes *res, drmModeConnector *conn)
{
    if (conn->encoder_id) {
        drmModeEncoder *e = drmModeGetEncoder(fd, conn->encoder_id);
        if (e) {
            uint32_t c = e->crtc_id;
            drmModeFreeEncoder(e);
            if (c)
                return c;               /* the one already lit */
        }
    }
    for (int i = 0; i < conn->count_encoders; i++) {
        drmModeEncoder *e = drmModeGetEncoder(fd, conn->encoders[i]);
        if (!e)
            continue;
        for (int j = 0; j < res->count_crtcs; j++)
            if (e->possible_crtcs & (1u << j)) {
                uint32_t c = res->crtcs[j];
                drmModeFreeEncoder(e);
                return c;
            }
        drmModeFreeEncoder(e);
    }
    return 0;
}

static void destroy_buf(struct drmbuf *b)
{
    if (b->map)
        munmap(b->map, b->size);
    if (b->fb_id)
        drmModeRmFB(D.fd, b->fb_id);
    if (b->handle)
        drmModeDestroyDumbBuffer(D.fd, b->handle);
    memset(b, 0, sizeof(*b));
}

/* Allocate one dumb buffer + framebuffer, mmap it, paint it black. */
static int create_buf(uint32_t fourcc, uint32_t bpp, struct drmbuf *b)
{
    uint32_t handle, pitch, fb_id;
    uint64_t size, off;
    uint8_t *map;
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

    if (drmModeCreateDumbBuffer(D.fd, D.mode.hdisplay, D.mode.vdisplay,
                                bpp, 0, &handle, &pitch, &size))
        return -1;
    handles[0] = handle;
    pitches[0] = pitch;
    if (drmModeAddFB2(D.fd, D.mode.hdisplay, D.mode.vdisplay, fourcc,
                      handles, pitches, offsets, &fb_id, 0)) {
        drmModeDestroyDumbBuffer(D.fd, handle);
        return -1;
    }
    if (drmModeMapDumbBuffer(D.fd, handle, &off)) {
        drmModeRmFB(D.fd, fb_id);
        drmModeDestroyDumbBuffer(D.fd, handle);
        return -1;
    }
    map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, D.fd, off);
    if (map == MAP_FAILED) {
        drmModeRmFB(D.fd, fb_id);
        drmModeDestroyDumbBuffer(D.fd, handle);
        return -1;
    }
    memset(map, 0, size);
    b->fb_id = fb_id;
    b->handle = handle;
    b->map = map;
    b->size = size;
    D.stride = pitch;
    D.bytespp = bpp / 8;
    return 0;
}

/* Bring up scanout with one format; returns 0 on success. The second
 * buffer (page-flip) is best-effort: without it we just write in place. */
static int setup_fb(uint32_t fourcc, uint32_t bpp)
{
    if (create_buf(fourcc, bpp, &D.buf[0]))
        return -1;
    if (drmModeSetCrtc(D.fd, D.crtc_id, D.buf[0].fb_id, 0, 0,
                       &D.conn_id, 1, &D.mode)) {
        destroy_buf(&D.buf[0]);
        return -1;
    }
    D.front = D.target = 0;
    if (create_buf(fourcc, bpp, &D.buf[1])) {
        dlog("no second dumb buffer: full frames won't page-flip");
        D.no_flip = 1;
    }
    return 0;
}

/* Opens card index i if it has a connected connector; returns fd or -1. */
static int open_card_if_connected(int i)
{
    char path[32];
    snprintf(path, sizeof(path), "/dev/dri/card%d", i);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    drmModeRes *res = drmModeGetResources(fd);
    int ok = 0;
    if (res) {
        for (int c = 0; c < res->count_connectors && !ok; c++) {
            drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[c]);
            if (conn) {
                ok = conn->connection == DRM_MODE_CONNECTED;
                drmModeFreeConnector(conn);
            }
        }
        drmModeFreeResources(res);
    }
    if (ok)
        return fd;
    close(fd);
    return -1;
}

static int drm_open(struct sremfb_output *o, unsigned *w, unsigned *h,
                    uint8_t *pixfmt, unsigned *bytespp)
{
    const char *env;
    int card_idx = -1;

    /* choose the card */
    if ((env = getenv("SREMFB_DRM_CARD"))) {
        D.fd = open(env, O_RDWR | O_CLOEXEC);
        sscanf(env, "/dev/dri/card%d", &card_idx);
    } else {
        for (int i = 0; i < 16; i++) {
            D.fd = open_card_if_connected(i);
            if (D.fd >= 0) { card_idx = i; break; }
        }
    }
    if (D.fd < 0) {
        dlog("no DRM card with a connected output (set SREMFB_DRM_CARD)");
        return -1;
    }
    drmSetMaster(D.fd);

    drmModeRes *res = drmModeGetResources(D.fd);
    if (!res) {
        dlog("drmModeGetResources failed: %s", strerror(errno));
        return -1;
    }

    /* choose the connector */
    const char *want = getenv("SREMFB_DRM_CONNECTOR");
    drmModeConnector *conn = NULL;
    char name[32] = "";
    for (int c = 0; c < res->count_connectors; c++) {
        drmModeConnector *cc = drmModeGetConnector(D.fd, res->connectors[c]);
        if (!cc)
            continue;
        snprintf(name, sizeof(name), "%s-%u",
                 conn_type_name(cc->connector_type), cc->connector_type_id);
        int match = cc->connection == DRM_MODE_CONNECTED && cc->count_modes > 0 &&
                    (!want || strcmp(want, name) == 0);
        if (match) { conn = cc; break; }
        drmModeFreeConnector(cc);
    }
    if (!conn) {
        dlog("no usable DRM connector%s%s", want ? " named " : "",
             want ? want : "");
        drmModeFreeResources(res);
        return -1;
    }
    D.conn_id = conn->connector_id;
    snprintf(D.sys_status, sizeof(D.sys_status),
             "/sys/class/drm/card%d-%s/status", card_idx, name);

    /* choose the mode: SREMFB_DRM_MODE, else the preferred one, else [0] */
    unsigned mw = 0, mh = 0;
    if ((env = getenv("SREMFB_DRM_MODE")))
        sscanf(env, "%ux%u", &mw, &mh);
    int mi = -1;
    for (int m = 0; m < conn->count_modes; m++) {
        drmModeModeInfo *M = &conn->modes[m];
        if (mw) {
            if (M->hdisplay == mw && M->vdisplay == mh) { mi = m; break; }
        } else if (M->type & DRM_MODE_TYPE_PREFERRED) { mi = m; break; }
    }
    if (mi < 0)
        mi = 0;
    D.mode = conn->modes[mi];

    D.crtc_id = find_crtc(D.fd, res, conn);
    D.dpms_prop = find_dpms_prop(D.fd, D.conn_id);
    drmModeFreeConnector(conn);
    if (!D.crtc_id) {
        dlog("no CRTC for the connector");
        drmModeFreeResources(res);
        return -1;
    }
    D.saved = drmModeGetCrtc(D.fd, D.crtc_id);   /* to restore on exit */
    drmModeFreeResources(res);

    /* format: RGB565 (RG16) unless SREMFB_DRM_DEPTH=32; XR24 fallback */
    int want32 = (env = getenv("SREMFB_DRM_DEPTH")) && atoi(env) == 32;
    int ok = -1;
    if (!want32)
        ok = setup_fb(DRM_FORMAT_RGB565, 16);
    if (ok < 0)
        ok = setup_fb(DRM_FORMAT_XRGB8888, 32);
    if (ok < 0) {
        dlog("no usable scanout format (RGB565/XRGB8888) on this CRTC");
        return -1;
    }

    *pixfmt = D.bytespp == 2 ? SREMFB_PIX_RGB565 : SREMFB_PIX_XRGB8888;
    o->priv = &D;
    o->w = *w = D.mode.hdisplay;
    o->h = *h = D.mode.vdisplay;
    o->pixfmt = *pixfmt;
    o->bytespp = *bytespp = D.bytespp;
    dlog("DRM %s: %ux%u %s, stride %u", D.sys_status, o->w, o->h,
         D.bytespp == 2 ? "RGB565" : "XRGB8888", D.stride);
    return 0;
}

/* Full-frame batches target the back buffer (flipped by present());
 * partial damage writes in place into the scanout buffer. */
static void drm_begin(struct sremfb_output *o, int full)
{
    (void)o;
    D.target = (full && !D.no_flip) ? !D.front : D.front;
}

static void drm_write_row(struct sremfb_output *o, unsigned dx, unsigned dy,
                          const uint8_t *src, unsigned npix)
{
    (void)o;
    memcpy(D.buf[D.target].map + (size_t)dy * D.stride +
           (size_t)dx * D.bytespp, src, (size_t)npix * D.bytespp);
}

/* The user data handed to drmModePageFlip() comes back here via
 * drmHandleEvent() when the flip lands (vblank). */
static void flip_done(int fd, unsigned seq, unsigned sec, unsigned usec,
                      void *data)
{
    (void)fd; (void)seq; (void)sec; (void)usec;
    *(int *)data = 1;
}

/* Wait for the scheduled flip to land so the ex-front buffer can be
 * rewritten. Bounded: a stuck driver degrades, it doesn't hang us. */
static int wait_flip(int *done)
{
    drmEventContext ev = { .version = 2, .page_flip_handler = flip_done };

    for (int tries = 0; !*done && tries < 10; tries++) {
        struct pollfd p = { .fd = D.fd, .events = POLLIN };
        if (poll(&p, 1, 100) <= 0)
            continue;
        drmHandleEvent(D.fd, &ev);
    }
    return *done ? 0 : -1;
}

static void drm_present(struct sremfb_output *o)
{
    int done = 0;

    (void)o;
    if (D.target == D.front)
        return;                /* in-place writes are already scanned out */
    if (drmModePageFlip(D.fd, D.crtc_id, D.buf[D.target].fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT, &done) == 0) {
        static int announced;
        if (wait_flip(&done) < 0) {
            dlog("page-flip event lost — falling back to in-place writes");
            D.no_flip = 1;     /* the flip itself was queued: swap anyway */
        } else if (!announced) {
            announced = 1;
            dlog("page-flip active (tear-free full frames)");
        }
        D.front = D.target;
        return;
    }
    /* flip refused (driver quirk): blocking modeset to the new buffer,
     * and if even that fails, degrade to single-buffer for good */
    if (drmModeSetCrtc(D.fd, D.crtc_id, D.buf[D.target].fb_id, 0, 0,
                       &D.conn_id, 1, &D.mode) == 0) {
        D.front = D.target;
    } else {
        dlog("page-flip unavailable (%s) — falling back to in-place writes",
             strerror(errno));
        D.no_flip = 1;
        D.target = D.front;
    }
}

static void drm_clear(struct sremfb_output *o)
{
    (void)o;
    for (int i = 0; i < 2; i++)
        if (D.buf[i].map)
            memset(D.buf[i].map, 0, D.buf[i].size);
}

static void drm_blank(struct sremfb_output *o, int on)
{
    (void)o;
    if (D.dpms_prop)
        drmModeConnectorSetProperty(D.fd, D.conn_id, D.dpms_prop,
                                    on ? DRM_MODE_DPMS_OFF : DRM_MODE_DPMS_ON);
    else if (on && D.buf[D.front].map)
        memset(D.buf[D.front].map, 0, D.buf[D.front].size); /* no DPMS: black */
}

/* Panel connected? Read the connector's sysfs status (cheap, no probe). */
static int drm_panel_present(struct sremfb_output *o)
{
    (void)o;
    FILE *f = fopen(D.sys_status, "r");
    char s[16] = "";
    if (!f)
        return -1;
    if (!fgets(s, sizeof(s), f))
        s[0] = '\0';
    fclose(f);
    if (strncmp(s, "connected", 9) == 0 || strncmp(s, "unknown", 7) == 0)
        return 1;
    return 0;
}

static int vt_of(const char *tty)
{
    const char *e = tty + strlen(tty);
    while (e > tty && e[-1] >= '0' && e[-1] <= '9')
        e--;
    return *e ? atoi(e) : -1;
}

static void drm_grab(struct sremfb_output *o)
{
    const char *tty = getenv("SREMFB_TTY");

    (void)o;
    tty = tty ? tty : "/dev/tty1";
    D.tty_fd = open(tty, O_RDWR);
    if (D.tty_fd < 0)
        return;                /* best effort: no VT to grab */
    if (ioctl(D.tty_fd, KDSETMODE, KD_GRAPHICS) == 0)
        D.tty_restore = 1;
    int vt = vt_of(tty);
    if (vt > 0 && (ioctl(D.tty_fd, VT_ACTIVATE, vt) < 0 ||
                   ioctl(D.tty_fd, VT_WAITACTIVE, vt) < 0))
        dlog("could not switch to VT %d: console may show through", vt);
}

static void drm_ungrab(struct sremfb_output *o)
{
    (void)o;
    if (D.tty_restore && D.tty_fd >= 0) {
        ioctl(D.tty_fd, KDSETMODE, KD_TEXT);
        ioctl(D.tty_fd, VT_ACTIVATE, 1);
        D.tty_restore = 0;
    }
}

static void drm_close(struct sremfb_output *o)
{
    (void)o;
    if (D.saved) {             /* restore what was on the CRTC before us */
        drmModeSetCrtc(D.fd, D.saved->crtc_id, D.saved->buffer_id,
                       D.saved->x, D.saved->y, &D.conn_id, 1, &D.saved->mode);
        drmModeFreeCrtc(D.saved);
        D.saved = NULL;
    }
    destroy_buf(&D.buf[0]);
    destroy_buf(&D.buf[1]);
    if (D.fd >= 0) {
        drmDropMaster(D.fd);
        close(D.fd);
        D.fd = -1;
    }
    if (D.tty_fd >= 0) {
        close(D.tty_fd);
        D.tty_fd = -1;
    }
}

const struct sremfb_output_ops sremfb_output_drm = {
    .name = "drm",
    .open = drm_open,
    .begin = drm_begin,
    .write_row = drm_write_row,
    .present = drm_present,
    .clear = drm_clear,
    .blank = drm_blank,
    .panel_present = drm_panel_present,
    .grab = drm_grab,
    .ungrab = drm_ungrab,
    .close = drm_close,
};
