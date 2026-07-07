/*
 * EVDI backend: each client's virtual screen is a real DRM connector
 * provided by the evdi kernel module (DisplayLink's driver, packaged as
 * evdi-dkms). Plugging an EDID makes the compositor treat it exactly like
 * a physical monitor being connected: hotplug, layout restore from
 * monitors.xml, lock screen, DPMS — all native. No screencast session,
 * hence no capture indicator and no lock-time inhibition; the identity
 * comes from our EDID (serial derived from the client's MAC), so each
 * client's position survives everything, reboots included.
 *
 * Flow, per client: hello -> acquire a free evdi device (flock) ->
 * connect an EDID at the client's resolution -> compositor sets a mode
 * (mode_changed) -> register a grab buffer -> request_update /
 * grab_pixels loop driven by damage. The kernel merges damage into at
 * most 16 rects and blends the cursor into the grabbed pixels (cursor
 * events stay disabled). Static screen = no events = zero traffic.
 *
 * All evdi ioctls are unprivileged; access to /dev/dri/cardN comes from
 * the logind seat ACL, so the server runs as the session user.
 */
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include <lz4.h>

#include "sremfb-server.h"

#define MAX_GRAB_RECTS  16
#define GRAB_BUFFER_ID  0

/* DRM constants, redeclared to avoid dragging libdrm headers in */
#define DPMS_MODE_ON    0
#define FOURCC_XR24     0x34325258u        /* DRM_FORMAT_XRGB8888 */

static void sremfb_evdi_kick(SremfbClient *c);

/* -------------------------------------------------------------- wire */

static void send_stats(SremfbClient *c)
{
    gint64 now = g_get_monotonic_time();
    double dt = (double)(now - c->st_since_us) / G_USEC_PER_SEC;

    if (dt < 5.0)
        return;
    if (c->st_grabs > 0) {
        double ratio = c->st_wire_bytes ?
            (double)c->st_raw_bytes / (double)c->st_wire_bytes : 1.0;
        char ctl[96] = "";
        if (c->feedback && c->xmit_mode == SREMFB_XMIT_H264)
            g_snprintf(ctl, sizeof(ctl),
                       " [%s, delay %.0f ms, %d kbps, cap %.1f MB/s]",
                       sremfb_ctl_state_name(c),
                       c->delay_ewma_us / 1000.0,
                       sremfb_enc_bitrate(c->enc),
                       c->capacity_Bps / 1e6);
        else if (c->feedback)
            g_snprintf(ctl, sizeof(ctl), " [%s, delay %.0f ms]",
                       sremfb_ctl_state_name(c),
                       c->delay_ewma_us / 1000.0);
        g_message("[%s] stats: %.1f fps, %.1f MB/s wire (ratio %.2fx, "
                  "%.1f rects/frame)%s", c->macstr,
                  (double)c->st_grabs / dt,
                  (double)c->st_wire_bytes / dt / 1e6, ratio,
                  (double)c->st_rects / (double)c->st_grabs, ctl);
    }
    c->st_grabs = c->st_rects = c->st_wire_bytes = c->st_raw_bytes = 0;
    c->st_since_us = now;
}

/* Payload-less control message (BLANK/UNBLANK), through the queue. */
static void send_ctrl(SremfbClient *c, uint8_t encoding)
{
    struct sremfb_frame_hdr hdr = {0};

    if (c->fd < 0 || c->state != SREMFB_CLIENT_STREAMING || c->lost_id)
        return;
    hdr.magic = SREMFB_MAGIC;
    hdr.encoding = encoding;
    sremfb_xmit_ctrl(c, &hdr, sizeof(hdr));
}

/* Grabs the pending damage (consuming it even if the client just left)
 * and hands it to the transmit queue — nothing is sent from here, the
 * frames are built when each client's socket can take them (xmit.c). */
static void grab_and_send(SremfbClient *c)
{
    struct evdi_rect rects[MAX_GRAB_RECTS];
    int num = MAX_GRAB_RECTS;
    unsigned bytespp = (c->hello.pixfmt == SREMFB_PIX_RGB565) ? 2 : 4;

    if (!c->grab_registered)
        return;
    evdi_grab_pixels(c->dev->handle, rects, &num);
    if (num <= 0 || c->fd < 0 || c->state != SREMFB_CLIENT_STREAMING)
        return;

    c->st_grabs++;
    sremfb_xmit_damage(c, rects, num, bytespp);
    send_stats(c);
}

/* ------------------------------------------------------- update loop */

static gboolean kick_idle(gpointer data)
{
    SremfbClient *c = data;

    c->kick_id = 0;
    sremfb_evdi_kick(c);
    return G_SOURCE_REMOVE;
}

/* Requests the next frame. If damage is already pending the grab happens
 * now and the next request is deferred to the main loop, so the listen
 * socket and the other clients keep getting serviced under constant
 * damage. */
static void sremfb_evdi_kick(SremfbClient *c)
{
    if (c->state != SREMFB_CLIENT_STREAMING || !c->grab_registered ||
        c->fd < 0 || c->update_pending || c->lost_id)
        return;

    if (evdi_request_update(c->dev->handle, GRAB_BUFFER_ID)) {
        grab_and_send(c);
        if (!c->kick_id)
            c->kick_id = g_idle_add(kick_idle, c);
    } else {
        c->update_pending = TRUE;
    }
}

/* ----------------------------------------------------- evdi handlers */

static void on_update_ready(int buffer, void *data)
{
    SremfbClient *c = ((SremfbEvdiDevice *)data)->owner;

    (void)buffer;
    if (!c)
        return;
    c->update_pending = FALSE;
    grab_and_send(c);
    if (!c->kick_id)
        c->kick_id = g_idle_add(kick_idle, c);
}

static void on_mode_changed(struct evdi_mode mode, void *data)
{
    SremfbClient *c = ((SremfbEvdiDevice *)data)->owner;

    if (!c)
        return;
    g_message("[%s] mode set: %dx%d@%d %dbpp fourcc 0x%08x", c->macstr,
              mode.width, mode.height, mode.refresh_rate,
              mode.bits_per_pixel, mode.pixel_format);

    if (mode.width <= 0 || mode.height <= 0 || mode.bits_per_pixel != 32) {
        g_warning("[%s] unusable mode, ignoring", c->macstr);
        return;
    }
    if (mode.pixel_format != FOURCC_XR24)
        g_warning("[%s] unexpected pixel format, assuming BGRx byte order",
                  c->macstr);

    /* the wire buffers are about to be rebuilt: drop anything in
     * flight that points into them */
    sremfb_xmit_reset(c);

    /* a resolution change invalidates the encoder and the H.264 episode */
    if (c->enc && (mode.width != c->mode.width ||
                   mode.height != c->mode.height)) {
        sremfb_enc_close(c->enc);
        c->enc = NULL;
        g_clear_pointer(&c->yuvbuf, g_free);
        c->xmit_mode = SREMFB_XMIT_RAW;
    }

    c->mode = mode;
    c->mode_valid = TRUE;
    c->dev->suspect = FALSE;       /* the card proved usable */

    /* (re)build the grab and wire buffers at the new size */
    if (c->grab_registered) {
        evdi_unregister_buffer(c->dev->handle, GRAB_BUFFER_ID);
        c->grab_registered = FALSE;
    }
    g_free(c->grabbuf);
    c->grabbuf = g_malloc((size_t)mode.width * mode.height * 4);
    g_free(c->shadowbuf);
    c->shadowbuf = g_malloc0((size_t)mode.width * mode.height * 4);

    struct evdi_buffer buf = {
        .id = GRAB_BUFFER_ID,
        .buffer = c->grabbuf,
        .width = mode.width,
        .height = mode.height,
        .stride = mode.width * 4,
    };
    evdi_register_buffer(c->dev->handle, buf);
    c->grab_registered = TRUE;
    c->update_pending = FALSE;

    unsigned bytespp = (c->hello.pixfmt == SREMFB_PIX_RGB565) ? 2 : 4;
    c->rectbuf_size = (size_t)mode.width * mode.height * bytespp;
    c->sendbuf_size = sizeof(struct sremfb_frame_hdr) +
                      (size_t)LZ4_compressBound((int)c->rectbuf_size);
    c->rectbuf = g_realloc(c->rectbuf, c->rectbuf_size);
    c->sendbuf = g_realloc(c->sendbuf, c->sendbuf_size);

    if (c->state == SREMFB_CLIENT_MODE_WAIT && c->fd >= 0) {
        uint8_t flags = 0;
        if (c->feedback)
            flags |= SREMFB_SRV_FLAG_PING;
        if (c->feedback && c->h264_cap && !c->h264_failed &&
            getenv("SREMFB_NO_H264") == NULL)
            flags |= SREMFB_SRV_FLAG_H264;
        g_clear_handle_id(&c->mode_timeout_id, g_source_remove);
        sremfb_xmit_hello(c, flags);
        c->state = SREMFB_CLIENT_STREAMING;
        c->st_grabs = c->st_rects = c->st_wire_bytes = c->st_raw_bytes = 0;
        c->st_since_us = g_get_monotonic_time();
        sremfb_ctl_start(c);
        /* a connector lighting up proves mutter is alive and accepting
         * hotplugs — rearm the self-heal budget (it only guards against
         * creating devices forever when mutter is truly gone) */
        c->srv->selfheal_left = 8;
        sremfb_usb_peer_add(c);
        g_message("[%s] streaming %dx%d", c->macstr, mode.width, mode.height);
    }
    sremfb_evdi_kick(c);
}

static void on_dpms(int dpms_mode, void *data)
{
    SremfbClient *c = ((SremfbEvdiDevice *)data)->owner;

    if (!c)
        return;
    g_message("[%s] dpms %s", c->macstr,
              dpms_mode == DPMS_MODE_ON ? "on" : "off");
    if (dpms_mode == DPMS_MODE_ON) {
        send_ctrl(c, SREMFB_ENC_UNBLANK);
        sremfb_evdi_kick(c);
    } else {
        send_ctrl(c, SREMFB_ENC_BLANK);
    }
}

static void on_crtc_state(int state, void *data)
{
    SremfbClient *c = ((SremfbEvdiDevice *)data)->owner;

    if (c && state)
        sremfb_evdi_kick(c);
}

/* Events are dispatched per device; a free device (owner == NULL) still
 * drains its queue, the handlers just ignore what they see. */
static gboolean on_evdi_ready(gint fd, GIOCondition cond, gpointer data)
{
    SremfbEvdiDevice *dev = data;
    struct evdi_event_context ctx = {
        .dpms_handler = on_dpms,
        .mode_changed_handler = on_mode_changed,
        .update_ready_handler = on_update_ready,
        .crtc_state_handler = on_crtc_state,
        .user_data = dev,
    };

    (void)fd; (void)cond;
    evdi_handle_events(dev->handle, &ctx);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------- connector control */

/* Self-heal: once a mode-timeout proved that mutter wedges on the
 * devices we inherited (EBUSY on reopen — typically after a server
 * restart, or when the boot-time reset raced the compositor's own
 * enumeration), stop trusting pre-existing free devices: create a
 * brand-new one at acquire time and plug it immediately. mutter accepts
 * a freshly created card but closes it again within a few seconds of
 * inactivity, so creating it while the client connection is already in
 * hand (plug follows in <1 s) is the only reliable timing. Bounded: a
 * reused minor number can still be stale for mutter ("device already
 * present" in its journal) and burn an attempt. Returns the new card
 * index, or -1. */
static int self_heal_add(SremfbServer *srv)
{
    gboolean existed[32] = { FALSE };

    if (srv->selfheal_left == 0) {
        g_warning("self-heal budget spent — a session re-login will "
                  "clear mutter");
        return -1;
    }
    for (int i = 0; i < 32; i++)
        existed[i] = evdi_check_device(i) == AVAILABLE;

    int fd = open("/sys/devices/evdi/add", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        g_warning("cannot add a fresh evdi device (%s) — is "
                  "sremfb-evdi-perms.service active?", g_strerror(errno));
        return -1;
    }
    if (write(fd, "1", 1) < 0) {
        g_warning("evdi add failed: %s", g_strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    srv->selfheal_left--;

    /* the node appears asynchronously (udev) */
    for (int tries = 0; tries < 40; tries++) {
        for (int i = 0; i < 32; i++)
            if (!existed[i] && evdi_check_device(i) == AVAILABLE)
                return i;
        g_usleep(50 * 1000);
    }
    g_warning("fresh evdi device never appeared");
    return -1;
}

static gboolean on_mode_timeout(gpointer data)
{
    SremfbClient *c = data;

    c->mode_timeout_id = 0;
    g_warning("[%s] compositor did not light up the connector within 10s "
              "(is a Wayland/GNOME session running?)", c->macstr);
    if (c->dev) {
        /* mutter probably hit its EBUSY-on-reopen bug on this card:
         * quarantine it so the client's retry lands on another device,
         * and let the next acquire self-heal with a fresh device */
        c->dev->suspect = TRUE;
        c->srv->wedge_seen = TRUE;
        g_warning("[%s] quarantining /dev/dri/card%d", c->macstr,
                  c->dev->card);
    }
    if (c->fd >= 0)
        net_send_server_hello(c->fd, 0, 0, 0, SREMFB_STATUS_SERVER_FAIL, 0);
    sremfb_client_lost(c);
    return G_SOURCE_REMOVE;
}

/* The EDID serial is what makes GNOME remember one position per client:
 * derived from the MAC when the client provides one, from the peer
 * address otherwise (test mode). */
static uint32_t client_serial(const SremfbClient *c)
{
    static const uint8_t zero[6];
    const uint8_t *m = c->hello.mac;

    if (memcmp(m, zero, 6) != 0)
        return ((uint32_t)m[2] << 24) | ((uint32_t)m[3] << 16) |
               ((uint32_t)m[4] << 8) | m[5];

    uint32_t h = 5381;
    for (const char *p = c->peer; *p; p++)
        h = h * 33 + (uint8_t)*p;
    return h;
}

/* "Plug the cable": connect an EDID at the client's resolution. The
 * compositor reacts with a hotplug, restores the layout position and
 * sets our mode (on_mode_changed). */
void sremfb_evdi_plug(SremfbClient *c)
{
    sremfb_edid_build(c->edid, c->hello.xres, c->hello.yres,
                      client_serial(c), c->hello.model);
    evdi_connect(c->dev->handle, c->edid, sizeof(c->edid),
                 (uint32_t)c->hello.xres * c->hello.yres);
    c->plugged = TRUE;
    c->state = SREMFB_CLIENT_MODE_WAIT;
    c->mode_timeout_id = g_timeout_add_seconds(10, on_mode_timeout, c);
    g_message("[%s] connector plugged at %ux%u (serial 0x%08x), waiting "
              "for the compositor", c->macstr, c->hello.xres, c->hello.yres,
              client_serial(c));
}

/* "Unplug the cable". The compositor sees a disconnect and re-tiles, and
 * will restore the layout on the next plug (stable EDID identity). */
void sremfb_evdi_unplug(SremfbClient *c)
{
    sremfb_xmit_reset(c);
    sremfb_ctl_stop(c);
    g_clear_handle_id(&c->mode_timeout_id, g_source_remove);
    g_clear_handle_id(&c->kick_id, g_source_remove);
    if (c->grab_registered) {
        evdi_unregister_buffer(c->dev->handle, GRAB_BUFFER_ID);
        c->grab_registered = FALSE;
    }
    g_clear_pointer(&c->grabbuf, g_free);
    g_clear_pointer(&c->shadowbuf, g_free);
    g_clear_pointer(&c->rectbuf, g_free);
    g_clear_pointer(&c->sendbuf, g_free);
    c->rectbuf_size = c->sendbuf_size = 0;
    c->mode_valid = FALSE;
    c->update_pending = FALSE;
    if (c->plugged) {
        evdi_disconnect(c->dev->handle);
        c->plugged = FALSE;
        g_message("[%s] connector unplugged", c->macstr);
    }
}

/* ------------------------------------------------------ device setup */

/* Best-effort at startup: recreate fresh evdi devices. A server restart
 * closes/reopens the devices, and mutter does not survive that (EBUSY on
 * reopen, hotplugs then ignored on the card — the quarantine above is
 * the runtime fallback); brand-new devices avoid the bug entirely.
 * Needs write access to /sys/devices/evdi/{remove_all,add}: the udev
 * rule shipped with the package hands them to group "video". Skipped if
 * another process (second server instance) holds an evdi device. */
void sremfb_evdi_reset(unsigned count)
{
    for (int i = 0; i < 32; i++) {
        char path[32];
        if (evdi_check_device(i) != AVAILABLE)
            continue;
        g_snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
            close(fd);
            g_message("evdi reset skipped: another process uses card%d", i);
            return;
        }
        close(fd);                     /* releases the probe lock */
    }

    int rfd = open("/sys/devices/evdi/remove_all", O_WRONLY | O_CLOEXEC);
    int afd = open("/sys/devices/evdi/add", O_WRONLY | O_CLOEXEC);
    if (rfd < 0 || afd < 0) {
        g_message("evdi reset skipped (no write access to "
                  "/sys/devices/evdi — udev rule missing or group)");
        if (rfd >= 0)
            close(rfd);
        if (afd >= 0)
            close(afd);
        return;
    }
    if (write(rfd, "1", 1) < 0)
        g_warning("evdi remove_all failed: %s", g_strerror(errno));
    close(rfd);
    g_usleep(300 * 1000);
    for (unsigned i = 0; i < count; i++)
        if (write(afd, "1", 1) < 0)
            g_warning("evdi add failed: %s", g_strerror(errno));
    close(afd);
    g_usleep(300 * 1000);
    g_message("evdi devices reset: %u fresh device(s)", count);
}

gboolean sremfb_evdi_probe(void)
{
    for (int i = 0; i < 32; i++)
        if (evdi_check_device(i) == AVAILABLE)
            return TRUE;
    return FALSE;
}

/* Hands the client a device from the pool, opening a new one if needed.
 * libevdi only tracks usage within one process; the flock arbitrates
 * with other processes (and our own pool entries, since each open() is
 * a distinct file description). */
static gboolean acquire_pooled(SremfbClient *c, gboolean allow_suspect)
{
    SremfbServer *srv = c->srv;

    for (guint i = 0; i < srv->devices->len; i++) {
        SremfbEvdiDevice *dev = g_ptr_array_index(srv->devices, i);
        if (dev->owner || (dev->suspect && !allow_suspect))
            continue;
        dev->owner = c;
        c->dev = dev;
        g_message("[%s] using evdi device /dev/dri/card%d (pooled%s)",
                  c->macstr, dev->card,
                  dev->suspect ? ", suspect — last resort" : "");
        return TRUE;
    }
    return FALSE;
}

/* Opens card index i into the pool for client c. */
static gboolean acquire_card(SremfbClient *c, int i, const char *how)
{
    char path[32];

    g_snprintf(path, sizeof(path), "/dev/dri/card%d", i);
    int lock_fd = open(path, O_RDWR | O_CLOEXEC);
    if (lock_fd < 0)
        return FALSE;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        close(lock_fd);                /* claimed by another process/us */
        return FALSE;
    }
    evdi_handle handle = evdi_open(i);
    if (!handle) {
        close(lock_fd);
        return FALSE;
    }
    SremfbEvdiDevice *dev = g_new0(SremfbEvdiDevice, 1);
    dev->handle = handle;
    dev->card = i;
    dev->lock_fd = lock_fd;
    dev->owner = c;
    dev->watch_id = g_unix_fd_add(evdi_get_event_ready(handle),
                                  G_IO_IN, on_evdi_ready, dev);
    g_ptr_array_add(c->srv->devices, dev);
    c->dev = dev;
    g_message("[%s] using evdi device /dev/dri/card%d%s", c->macstr, i, how);
    return TRUE;
}

gboolean sremfb_evdi_acquire(SremfbClient *c)
{
    SremfbServer *srv = c->srv;

    if (acquire_pooled(c, FALSE))
        return TRUE;

    /* Once a wedge was seen, pre-existing free devices are mutter
     * minefields: hand out a brand-new one instead — created right now,
     * so the EDID plug lands within mutter's short acceptance window. */
    if (srv->wedge_seen) {
        int fresh = self_heal_add(srv);
        if (fresh >= 0 && acquire_card(c, fresh, " (fresh, self-heal)"))
            return TRUE;
    }

    for (int i = 0; i < 32; i++) {
        if (evdi_check_device(i) != AVAILABLE)
            continue;
        if (acquire_card(c, i, ""))
            return TRUE;
    }

    /* nothing clean left: a quarantined card is better than a refusal */
    return acquire_pooled(c, TRUE);
}

/* The device stays open and flocked: only the ownership is returned. */
void sremfb_evdi_release(SremfbClient *c)
{
    if (c->dev) {
        c->dev->owner = NULL;
        c->dev = NULL;
    }
}

void sremfb_evdi_close_all(SremfbServer *srv)
{
    if (!srv->devices)
        return;
    for (guint i = 0; i < srv->devices->len; i++) {
        SremfbEvdiDevice *dev = g_ptr_array_index(srv->devices, i);
        g_clear_handle_id(&dev->watch_id, g_source_remove);
        evdi_close(dev->handle);
        close(dev->lock_fd);
        g_free(dev);
    }
    g_ptr_array_free(srv->devices, TRUE);
    srv->devices = NULL;
}
