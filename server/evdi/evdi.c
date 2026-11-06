/* SPDX-License-Identifier: EUPL-1.2 */
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
 * most 16 rects. Static screen = no events = zero traffic.
 *
 * The cursor is composed here, from evdi's cursor events: left to the
 * kernel (cursor events off), it is blended into every grab, but a
 * cursor plane being *disabled* — mutter does that the moment the
 * pointer leaves the output — marks nothing dirty (evdi_modeset.c,
 * evdi_cursor_atomic_update: dirty rects only `if (fb != NULL)`, evdi
 * 1.14/1.15). No update, so the client kept the last cursor image
 * forever. With the events we know every show, hide, shape change and
 * move, and repaint exactly the old and new cursor rects.
 *
 * All evdi ioctls are unprivileged; access to /dev/dri/cardN comes from
 * the logind seat ACL, so the server runs as the session user.
 */
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lz4.h>

#include "evdi.h"

#define MAX_GRAB_RECTS  16
#define GRAB_BUFFER_ID  0

/* DRM constants, redeclared to avoid dragging libdrm headers in */
#define DPMS_MODE_ON    0
#define FOURCC_XR24     0x34325258u        /* DRM_FORMAT_XRGB8888 */
#define FOURCC_AR24     0x34325241u        /* DRM_FORMAT_ARGB8888 */
#define DRM_IOCTL_DROP_MASTER_ _IO('d', 0x1f)  /* DRM_IOCTL_DROP_MASTER */

/* backend-private state, hung off the generic core structs */
#define EV(c)    ((SremfbEvdiClient *)(c)->src_ctx)
#define EVS(srv) ((SremfbEvdiState *)(srv)->src)

static void sremfb_evdi_kick(SremfbClient *c);
static void plug_lit(SremfbServer *srv);

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

/* ------------------------------------------------------------ cursor */

/*
 * grabbuf holds the desktop *with* the cursor blended in, as the client
 * shows it; cur_under keeps what the cursor covers. Around every grab the
 * cursor is lifted (the kernel only rewrites the damaged rects, the rest
 * of grabbuf must be the bare desktop for the next blend) and put back on
 * the fresh pixels. A cursor event lifts it, moves/reshapes/hides it,
 * puts it back and reports the old and new rects as damage — no grab,
 * no update request.
 */

/* The cursor's rect clipped to the screen; FALSE when nothing shows. */
static gboolean cursor_rect(SremfbClient *c, struct sremfb_rect *r)
{
    SremfbEvdiClient *e = EV(c);

    if (!e->cur_on || !c->grabbuf || !e->mode_valid)
        return FALSE;
    r->x1 = MAX(e->cur_x, 0);
    r->y1 = MAX(e->cur_y, 0);
    r->x2 = MIN(e->cur_x + e->cur_w, c->geom.width);
    r->y2 = MIN(e->cur_y + e->cur_h, c->geom.height);
    return r->x2 > r->x1 && r->y2 > r->y1;
}

/* Puts the covered pixels back. Returns TRUE (and the rect) when the
 * cursor was on screen. */
static gboolean cursor_lift(SremfbClient *c, struct sremfb_rect *out)
{
    SremfbEvdiClient *e = EV(c);
    const struct sremfb_rect *r = &e->cur_rect;
    size_t w = (size_t)(r->x2 - r->x1);

    if (!e->cur_drawn)
        return FALSE;
    e->cur_drawn = FALSE;
    if (!c->grabbuf)
        return FALSE;
    for (int y = r->y1; y < r->y2; y++)
        memcpy(c->grabbuf + ((size_t)y * c->geom.width + r->x1) * 4,
               e->cur_under + (size_t)(y - r->y1) * w, w * 4);
    if (out)
        *out = *r;
    return TRUE;
}

/* s + d*(255-a)/255, rounded, on one 8-bit channel: premultiplied
 * source over an opaque destination (DRM cursor planes default to
 * premultiplied alpha; the kernel's own blend took it as straight alpha,
 * which darkened the antialiased edges a little). */
static inline uint32_t over8(uint32_t s, uint32_t d, uint32_t ia)
{
    uint32_t t = d * ia + 128;

    t = s + ((t + (t >> 8)) >> 8);
    return t > 255 ? 255 : t;
}

/* Blends the cursor at its current place, saving what it covers.
 * Returns TRUE (and the rect) when something was drawn. */
static gboolean cursor_draw(SremfbClient *c, struct sremfb_rect *out)
{
    SremfbEvdiClient *e = EV(c);
    struct sremfb_rect r;

    if (e->cur_drawn || !cursor_rect(c, &r))
        return FALSE;
    size_t w = (size_t)(r.x2 - r.x1), need = w * (size_t)(r.y2 - r.y1);
    if (need > e->cur_under_size) {
        g_free(e->cur_under);
        e->cur_under = g_new(uint32_t, need);
        e->cur_under_size = need;
    }
    for (int y = r.y1; y < r.y2; y++) {
        uint8_t *row = c->grabbuf + ((size_t)y * c->geom.width + r.x1) * 4;
        const uint32_t *src = e->cur_pix +
            (size_t)(y - e->cur_y) * e->cur_stride + (r.x1 - e->cur_x);
        uint32_t *dst = (uint32_t *)row;

        memcpy(e->cur_under + (size_t)(y - r.y1) * w, row, w * 4);
        for (size_t x = 0; x < w; x++) {
            uint32_t s = src[x], a = s >> 24;
            if (a == 0)
                continue;
            if (a == 255) {
                dst[x] = s & 0x00ffffffu;
                continue;
            }
            uint32_t d = dst[x], ia = 255 - a;
            dst[x] = over8(s & 0xff, d & 0xff, ia) |
                     over8((s >> 8) & 0xff, (d >> 8) & 0xff, ia) << 8 |
                     over8((s >> 16) & 0xff, (d >> 16) & 0xff, ia) << 16;
        }
    }
    e->cur_rect = r;
    e->cur_drawn = TRUE;
    if (out)
        *out = r;
    return TRUE;
}

/* Forgets the cursor image and what it covered (unplug). */
static void cursor_reset(SremfbClient *c)
{
    SremfbEvdiClient *e = EV(c);

    free(e->cur_pix);                  /* malloc'ed by libevdi */
    e->cur_pix = NULL;
    e->cur_on = FALSE;
    e->cur_drawn = FALSE;
    g_clear_pointer(&e->cur_under, g_free);
    e->cur_under_size = 0;
}

/* After a cursor change (lifted first by the caller: had/old), puts it
 * back and reports the old and new places as damage. */
static void cursor_repaint(SremfbClient *c, const struct sremfb_rect *old,
                           gboolean had)
{
    struct sremfb_rect rects[2];
    int n = 0;
    unsigned bytespp = (c->hello.pixfmt == SREMFB_PIX_RGB565) ? 2 : 4;

    if (had)
        rects[n++] = *old;
    if (cursor_draw(c, &rects[n]))
        n++;
    if (n == 0 || !EV(c)->grab_registered || c->fd < 0 ||
        c->state != SREMFB_CLIENT_STREAMING || c->lost_id)
        return;
    sremfb_xmit_damage(c, rects, n, bytespp);
}

/* Grabs the pending damage (consuming it even if the client just left)
 * and hands it to the transmit queue — nothing is sent from here, the
 * frames are built when each client's socket can take them (xmit.c). */
static void grab_and_send(SremfbClient *c)
{
    struct evdi_rect rects[MAX_GRAB_RECTS];
    int num = MAX_GRAB_RECTS;
    unsigned bytespp = (c->hello.pixfmt == SREMFB_PIX_RGB565) ? 2 : 4;

    if (!EV(c)->grab_registered)
        return;
    /* the kernel rewrites the damaged rects with the bare desktop: the
     * rest of grabbuf has to be bare too, then the cursor goes back on
     * top of the fresh pixels (inside the damaged rects, so no extra
     * damage for it) */
    cursor_lift(c, NULL);
    evdi_grab_pixels(EV(c)->dev->handle, rects, &num);
    cursor_draw(c, NULL);
    if (num <= 0 || c->fd < 0 || c->state != SREMFB_CLIENT_STREAMING)
        return;

    c->st_grabs++;
    /* struct evdi_rect and struct sremfb_rect are the same {x1,y1,x2,y2}
     * layout — the core takes damage in the neutral type. */
    sremfb_xmit_damage(c, (const struct sremfb_rect *)rects, num, bytespp);
    send_stats(c);
}

/* ------------------------------------------------------- update loop */

static gboolean kick_idle(gpointer data)
{
    SremfbClient *c = data;

    EV(c)->kick_id = 0;
    sremfb_evdi_kick(c);
    return G_SOURCE_REMOVE;
}

/* Requests the next frame. If damage is already pending the grab happens
 * now and the next request is deferred to the main loop, so the listen
 * socket and the other clients keep getting serviced under constant
 * damage. */
static void sremfb_evdi_kick(SremfbClient *c)
{
    if (c->state != SREMFB_CLIENT_STREAMING || !EV(c)->grab_registered ||
        c->fd < 0 || EV(c)->update_pending || c->lost_id)
        return;

    if (evdi_request_update(EV(c)->dev->handle, GRAB_BUFFER_ID)) {
        grab_and_send(c);
        if (!EV(c)->kick_id)
            EV(c)->kick_id = g_idle_add(kick_idle, c);
    } else {
        EV(c)->update_pending = TRUE;
    }
}

/* ----------------------------------------------------- evdi handlers */

static void on_update_ready(int buffer, void *data)
{
    SremfbClient *c = ((SremfbEvdiDevice *)data)->owner;

    (void)buffer;
    if (!c)
        return;
    EV(c)->update_pending = FALSE;
    grab_and_send(c);
    if (!EV(c)->kick_id)
        EV(c)->kick_id = g_idle_add(kick_idle, c);
}

static void on_cursor_set(struct evdi_cursor_set cs, void *data)
{
    SremfbClient *c = ((SremfbEvdiDevice *)data)->owner;
    struct sremfb_rect old;
    gboolean had;

    if (!c) {
        free(cs.buffer);
        return;
    }
    SremfbEvdiClient *e = EV(c);
    had = cursor_lift(c, &old);
    free(e->cur_pix);
    e->cur_pix = NULL;
    e->cur_on = FALSE;
    if (cs.enabled && cs.buffer && cs.width > 0 && cs.height > 0 &&
        cs.width <= 512 && cs.height <= 512 &&
        cs.stride >= cs.width * 4 && cs.stride % 4 == 0 &&
        cs.buffer_length >= (uint64_t)cs.stride * cs.height) {
        if (cs.pixel_format != FOURCC_AR24)
            g_warning("[%s] cursor format 0x%08x, assuming ARGB8888",
                      c->macstr, cs.pixel_format);
        e->cur_pix = cs.buffer;        /* ours now (libevdi hands it over) */
        e->cur_w = (int)cs.width;
        e->cur_h = (int)cs.height;
        e->cur_stride = (int)(cs.stride / 4);
        e->cur_on = TRUE;
    } else {
        free(cs.buffer);
    }
    cursor_repaint(c, &old, had);
}

static void on_cursor_move(struct evdi_cursor_move cm, void *data)
{
    SremfbClient *c = ((SremfbEvdiDevice *)data)->owner;
    struct sremfb_rect old;
    gboolean had;

    if (!c)
        return;
    had = cursor_lift(c, &old);
    EV(c)->cur_x = cm.x;
    EV(c)->cur_y = cm.y;
    cursor_repaint(c, &old, had);
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
    if (c->enc && (mode.width != c->geom.width ||
                   mode.height != c->geom.height)) {
        sremfb_enc_close(c->enc);
        c->enc = NULL;
        g_clear_pointer(&c->yuvbuf, g_free);
        c->xmit_mode = SREMFB_XMIT_RAW;
    }

    c->geom.width = mode.width;
    c->geom.height = mode.height;
    EV(c)->mode_valid = TRUE;
    EV(c)->dev->suspect = FALSE;       /* the card proved usable */

    /* (re)build the grab and wire buffers at the new size */
    if (EV(c)->grab_registered) {
        evdi_unregister_buffer(EV(c)->dev->handle, GRAB_BUFFER_ID);
        EV(c)->grab_registered = FALSE;
    }
    EV(c)->cur_drawn = FALSE;          /* its pixels go with grabbuf */
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
    evdi_register_buffer(EV(c)->dev->handle, buf);
    EV(c)->grab_registered = TRUE;
    EV(c)->update_pending = FALSE;

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
        flags |= sremfb_input_start(c);   /* devices before the hello */
        flags |= sremfb_audio_start(c);   /* output before the hello */
        g_clear_handle_id(&EV(c)->mode_timeout_id, g_source_remove);
        sremfb_xmit_hello(c, flags);
        c->state = SREMFB_CLIENT_STREAMING;
        c->st_grabs = c->st_rects = c->st_wire_bytes = c->st_raw_bytes = 0;
        c->st_since_us = g_get_monotonic_time();
        sremfb_ctl_start(c);
        /* a connector lighting up proves mutter is alive and accepting
         * hotplugs: the failure run ends here, which rearms the self-heal
         * budget and opens the plug gate (see "plug gate" below) */
        plug_lit(c->srv);
        if (!EV(c)->usb_early)         /* else announced while parked */
            sremfb_usb_peer_add(c);
        sremfb_layout_client(c);
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
        .cursor_set_handler = on_cursor_set,
        .cursor_move_handler = on_cursor_move,
        .user_data = dev,
    };

    (void)fd; (void)cond;
    evdi_handle_events(dev->handle, &ctx);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------- connector control */

/* ------------------------------------------ card numbers mutter knows */

/*
 * mutter (GNOME 48) never forgets a secondary GPU: when an evdi card is
 * removed (remove_all from a mode switch, or our own reset), its
 * /dev/dri/cardN entry stays in mutter's GPU list ("Failed to reopen
 * cardN" on every reconfiguration), and a *new* card that the kernel later
 * gives the same number is refused as a duplicate ("Failed to hotplug
 * secondary gpu: device already present") — its connector never lights
 * up. DRM reuses freed numbers, so after a stop / remove_all / start
 * cycle the fresh cards of the reset were exactly those poisoned numbers:
 * two 10 s mode timeouts, quarantine, then self-heal finally minted a
 * number mutter had never seen.
 *
 * So every card we create is checked: a number already handed to this
 * compositor instance is "dead" on arrival, and a new number must be
 * picked up by gnome-shell (its /proc/<pid>/fd shows the card within a
 * second, logind's TakeDevice) to count as usable. Dead cards are simply
 * left in place — occupying their number, so the next add gets a fresh
 * one — and never handed to a client; the next remove_all takes them
 * away. What mutter has seen is remembered in $XDG_RUNTIME_DIR for the
 * lifetime of the gnome-shell process, so a server restart knows too.
 */
#define EVDI_MAX_CARDS     64
#define CARDS_FILE         "sremfb-evdi-cards"

enum { CARD_UNKNOWN = 0, CARD_OK, CARD_DEAD };

static struct {
    gboolean loaded;
    pid_t shell;                       /* gnome-shell the states refer to */
    uint8_t state[EVDI_MAX_CARDS];
    gboolean no_verify;                /* can't watch the compositor */
} K;

/* The session's gnome-shell (ours: same uid), 0 when none. */
static pid_t compositor_pid(void)
{
    GDir *d = g_dir_open("/proc", 0, NULL);
    const char *n;
    pid_t found = 0;

    if (!d)
        return 0;
    while (!found && (n = g_dir_read_name(d))) {
        char path[64];
        gchar *comm = NULL;
        struct stat st;

        if (n[0] < '1' || n[0] > '9')
            continue;
        g_snprintf(path, sizeof(path), "/proc/%s", n);
        if (stat(path, &st) < 0 || st.st_uid != getuid())
            continue;
        g_snprintf(path, sizeof(path), "/proc/%s/comm", n);
        if (g_file_get_contents(path, &comm, NULL, NULL) &&
            g_strcmp0(g_strchomp(comm), "gnome-shell") == 0)
            found = (pid_t)atoi(n);
        g_free(comm);
    }
    g_dir_close(d);
    return found;
}

/* 1 = the compositor has /dev/dri/card<card> open, 0 = not, -1 = can't
 * tell (no /proc access). */
static int compositor_holds(pid_t pid, int card)
{
    char dir[64], want[32];
    GDir *d;
    const char *n;
    int held = 0;

    g_snprintf(dir, sizeof(dir), "/proc/%d/fd", (int)pid);
    g_snprintf(want, sizeof(want), "/dev/dri/card%d", card);
    d = g_dir_open(dir, 0, NULL);
    if (!d)
        return -1;
    while (!held && (n = g_dir_read_name(d))) {
        char path[96], target[64];
        ssize_t l;

        g_snprintf(path, sizeof(path), "%s/%s", dir, n);
        l = readlink(path, target, sizeof(target) - 1);
        if (l > 0) {
            target[l] = '\0';
            held = strcmp(target, want) == 0;
        }
    }
    g_dir_close(d);
    return held;
}

static gchar *cards_path(void)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");

    return g_build_filename(rundir && *rundir ? rundir : "/tmp",
                            CARDS_FILE, NULL);
}

/* Loads the card states when they belong to the running gnome-shell. */
static void cards_load(void)
{
    pid_t shell = compositor_pid();
    gchar *path, *txt = NULL;

    if (K.loaded && K.shell == shell)
        return;
    memset(&K, 0, sizeof(K));
    K.loaded = TRUE;
    K.shell = shell;
    path = cards_path();
    if (shell && g_file_get_contents(path, &txt, NULL, NULL)) {
        gchar **lines = g_strsplit(txt, "\n", -1);
        if (lines[0] && atoi(lines[0]) == (int)shell) {
            for (gchar **l = lines + 1; *l; l++) {
                int card = -1;
                char what[8] = "";
                if (sscanf(*l, "%d %7s", &card, what) == 2 &&
                    card >= 0 && card < EVDI_MAX_CARDS)
                    K.state[card] = strcmp(what, "ok") == 0 ? CARD_OK
                                                             : CARD_DEAD;
            }
        }
        g_strfreev(lines);
    }
    g_free(txt);
    g_free(path);
}

static void cards_save(void)
{
    GString *s = g_string_new(NULL);
    gchar *path = cards_path();

    g_string_append_printf(s, "%d\n", (int)K.shell);
    for (int i = 0; i < EVDI_MAX_CARDS; i++)
        if (K.state[i] != CARD_UNKNOWN)
            g_string_append_printf(s, "%d %s\n", i,
                                   K.state[i] == CARD_OK ? "ok" : "dead");
    g_file_set_contents(path, s->str, (gssize)s->len, NULL);
    g_string_free(s, TRUE);
    g_free(path);
}

/* A card mutter refuses (number reused within its lifetime). */
static gboolean card_dead(int card)
{
    cards_load();
    return card >= 0 && card < EVDI_MAX_CARDS && K.state[card] == CARD_DEAD;
}

/* Creates one evdi device, returns its card index once the node is
 * usable by us, or -1. */
static int evdi_add_one(void)
{
    gboolean existed[EVDI_MAX_CARDS];

    for (int i = 0; i < EVDI_MAX_CARDS; i++)
        existed[i] = evdi_check_device(i) == AVAILABLE;

    int fd = open("/sys/devices/evdi/add", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        g_warning("cannot add an evdi device (%s) — is "
                  "sremfb-evdi-perms.service active?", g_strerror(errno));
        return -1;
    }
    if (write(fd, "1", 1) < 0) {
        g_warning("evdi add failed: %s", g_strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    /* the node appears asynchronously (udev) */
    for (int tries = 0; tries < 100; tries++) {
        for (int i = 0; i < EVDI_MAX_CARDS; i++)
            if (!existed[i] && evdi_check_device(i) == AVAILABLE)
                return i;
        g_usleep(20 * 1000);
    }
    g_warning("new evdi device never appeared");
    return -1;
}

/* Adds evdi devices until one gets a card number the compositor accepts,
 * and returns it (-1 on failure). The refused ones stay as placeholders.
 * Each accepted card is also ready for us: mutter only sees it after
 * udev (and its uaccess ACL) processed it. */
static int evdi_add_usable(void)
{
    int unconfirmed = 0;

    cards_load();
    for (int attempt = 0; attempt < 24; attempt++) {
        int card = evdi_add_one();
        if (card < 0)
            return -1;
        if (card >= EVDI_MAX_CARDS)
            return card;               /* beyond what we track: hope */
        if (K.state[card] != CARD_UNKNOWN) {
            K.state[card] = CARD_DEAD;
            cards_save();
            g_message("evdi: card%d reuses a number gnome-shell already "
                      "knows (it would ignore it) — left as a placeholder",
                      card);
            continue;
        }
        if (!K.shell || K.no_verify) {
            K.state[card] = CARD_OK;
            cards_save();
            return card;
        }
        int held = 0;
        for (int t = 0; t < 150 && held == 0; t++) {   /* 1.5 s */
            held = compositor_holds(K.shell, card);
            if (held == 0)
                g_usleep(10 * 1000);
        }
        if (held != 0) {               /* picked up, or can't tell */
            if (held < 0) {
                K.no_verify = TRUE;
                g_message("evdi: cannot watch gnome-shell's devices, "
                          "trusting new cards blindly");
            }
            K.state[card] = CARD_OK;
            cards_save();
            return card;
        }
        K.state[card] = CARD_DEAD;
        cards_save();
        g_message("evdi: gnome-shell did not pick card%d up — left as a "
                  "placeholder", card);
        if (++unconfirmed >= 3) {
            /* a stuck or different compositor: stop second-guessing,
             * the mode timeout and quarantine still stand behind */
            K.no_verify = TRUE;
            g_warning("evdi: gnome-shell ignored 3 new cards in a row, "
                      "no longer checking");
        }
    }
    g_warning("evdi: no usable card number after 24 additions");
    return -1;
}

/* ------------------------------------------------------- plug gate */

/*
 * Every EDID plug is a hotplug for mutter, and a mutter that stopped
 * lighting our connectors still processes them — leaking its GEM/shmem
 * buffers each time (≈31 MB per 1920x1080 cycle, unevictable, owned by
 * no process). Night of 2026-11-06: once the self-heal budget was spent,
 * acquire fell back to the quarantined card and the client's retry loop
 * re-plugged it every ~16 s for six hours: 1368 cycles, swap full, OOM.
 *
 * So plugging goes through a gate, computed afresh at each connection
 * from a short history of timestamps (plugs, lit connectors, mode
 * timeouts) — no sticky flag:
 *
 *  - the failure run = mode timeouts since the last connector that lit
 *    up. Its first SELFHEAL_BUDGET failures may each mint a fresh device
 *    (self-heal); past that the budget is spent and plugging backs off:
 *    one probe plug BACKOFF_MIN_S after the last failure, then doubling,
 *    capped at BACKOFF_MAX_S. Clients are turned away meanwhile with
 *    SERVER_FAIL (what they would get after 10 s anyway, minus the
 *    hotplug).
 *  - whatever the path, at most FAILS_PER_HOUR mode timeouts and
 *    PLUGS_PER_HOUR plugs per sliding hour.
 *  - a connector lighting up ends the run; a different gnome-shell (the
 *    session was re-logged in, or the shell restarted) clears the whole
 *    history.
 */
#define SELFHEAL_BUDGET    8           /* fresh devices per failure run */
#define BACKOFF_MIN_S      60
#define BACKOFF_MAX_S      (30 * 60)
#define FAILS_PER_HOUR     16
#define PLUGS_PER_HOUR     60
#define PLUG_WINDOW_US     ((gint64)3600 * G_USEC_PER_SEC)
#define PLUG_EVENTS_MAX    1024

enum { SREMFB_PLUG_EV_PLUG = 0, SREMFB_PLUG_EV_LIT, SREMFB_PLUG_EV_FAIL };
enum { GATE_OPEN = 0, GATE_BACKOFF, GATE_FAIL_CAP, GATE_PLUG_CAP };

/* What the history says right now. */
typedef struct {
    int verdict;                       /* GATE_* */
    gint64 until;                      /* monotonic, when not GATE_OPEN */
    unsigned fail_run;                 /* failures since the last lit */
} PlugGate;

static GArray *plug_events(SremfbServer *srv)
{
    if (!EVS(srv)->plug_events)
        EVS(srv)->plug_events = g_array_new(FALSE, FALSE,
                                            sizeof(SremfbPlugEvent));
    return EVS(srv)->plug_events;
}

/* Drops what no rule looks at any more: older than the window, except
 * the failures of the current run (the backoff counts them all). A
 * different gnome-shell clears everything. */
static void plug_prune(SremfbServer *srv, gint64 now)
{
    GArray *ev = plug_events(srv);
    pid_t shell = compositor_pid();
    int last_lit = -1;
    guint keep = 0;

    if (shell != EVS(srv)->plug_shell) {
        if (ev->len > 0)
            g_message("plug gate: gnome-shell changed (pid %d -> %d), plug "
                      "history cleared", (int)EVS(srv)->plug_shell,
                      (int)shell);
        g_array_set_size(ev, 0);
        EVS(srv)->plug_shell = shell;
        EVS(srv)->gate_logged = GATE_OPEN;
        return;
    }
    for (guint i = 0; i < ev->len; i++)
        if (g_array_index(ev, SremfbPlugEvent, i).kind ==
            SREMFB_PLUG_EV_LIT)
            last_lit = (int)i;
    for (guint i = 0; i < ev->len; i++) {
        SremfbPlugEvent e = g_array_index(ev, SremfbPlugEvent, i);
        if (e.t >= now - PLUG_WINDOW_US ||
            (e.kind == SREMFB_PLUG_EV_FAIL && (int)i > last_lit))
            g_array_index(ev, SremfbPlugEvent, keep++) = e;
    }
    g_array_set_size(ev, keep);
    if (ev->len > PLUG_EVENTS_MAX)
        g_array_remove_range(ev, 0, ev->len - PLUG_EVENTS_MAX);
}

static void plug_record(SremfbServer *srv, uint8_t kind)
{
    gint64 now = g_get_monotonic_time();
    SremfbPlugEvent e = { .t = now, .kind = kind };

    plug_prune(srv, now);
    g_array_append_val(plug_events(srv), e);
}

/* When the count of `kind` events inside the sliding window drops below
 * `cap` again (0 = it already is). */
static gint64 window_free_at(GArray *ev, uint8_t kind, gint64 now,
                             unsigned cap)
{
    unsigned n = 0, k = 0;

    for (guint i = 0; i < ev->len; i++) {
        SremfbPlugEvent *e = &g_array_index(ev, SremfbPlugEvent, i);
        if (e->kind == kind && e->t >= now - PLUG_WINDOW_US)
            n++;
    }
    if (n < cap)
        return 0;
    /* the (n - cap + 1)-th oldest in the window has to age out */
    for (guint i = 0; i < ev->len; i++) {
        SremfbPlugEvent *e = &g_array_index(ev, SremfbPlugEvent, i);
        if (e->kind == kind && e->t >= now - PLUG_WINDOW_US &&
            ++k == n - cap + 1)
            return e->t + PLUG_WINDOW_US;
    }
    return 0;                          /* not reached */
}

static PlugGate plug_gate(SremfbServer *srv, gint64 now)
{
    PlugGate g = { GATE_OPEN, 0, 0 };
    GArray *ev;
    gint64 last_fail = 0, t;

    plug_prune(srv, now);
    ev = plug_events(srv);
    for (guint i = 0; i < ev->len; i++) {
        SremfbPlugEvent *e = &g_array_index(ev, SremfbPlugEvent, i);
        if (e->kind == SREMFB_PLUG_EV_LIT) {
            g.fail_run = 0;
        } else if (e->kind == SREMFB_PLUG_EV_FAIL) {
            g.fail_run++;
            last_fail = e->t;
        }
    }
    if (g.fail_run > SELFHEAL_BUDGET) {
        unsigned shift = MIN(g.fail_run - SELFHEAL_BUDGET - 1, 16u);
        gint64 delay = MIN((gint64)BACKOFF_MIN_S << shift,
                           (gint64)BACKOFF_MAX_S) * G_USEC_PER_SEC;
        if (last_fail + delay > now) {
            g.verdict = GATE_BACKOFF;
            g.until = last_fail + delay;
        }
    }
    t = window_free_at(ev, SREMFB_PLUG_EV_FAIL, now, FAILS_PER_HOUR);
    if (t > now && t > g.until) {
        g.verdict = GATE_FAIL_CAP;
        g.until = t;
    }
    t = window_free_at(ev, SREMFB_PLUG_EV_PLUG, now, PLUGS_PER_HOUR);
    if (t > now && t > g.until) {
        g.verdict = GATE_PLUG_CAP;
        g.until = t;
    }
    return g;
}

/* Gate verdict for a new client, journaled once per change. */
static int evdi_source_admit(SremfbServer *srv)
{
    gint64 now = g_get_monotonic_time();
    PlugGate g = plug_gate(srv, now);

    if (g.verdict != EVS(srv)->gate_logged) {
        if (g.verdict == GATE_OPEN) {
            g_message("plug gate open again (%u failure(s) in the current "
                      "run): next client probes the compositor",
                      g.fail_run);
        } else {
            gint64 secs = (g.until - now + G_USEC_PER_SEC - 1) /
                          G_USEC_PER_SEC;
            GDateTime *at = g_date_time_new_from_unix_local(
                g_get_real_time() / G_USEC_PER_SEC + secs);
            gchar *hms = at ? g_date_time_format(at, "%H:%M:%S") : NULL;
            const char *why =
                g.verdict == GATE_BACKOFF ?
                    "self-heal budget spent, the compositor keeps ignoring "
                    "our connectors (backoff)" :
                g.verdict == GATE_FAIL_CAP ?
                    "too many mode timeouts in the last hour (cap "
                    G_STRINGIFY(FAILS_PER_HOUR) ")" :
                    "too many plugs in the last hour (cap "
                    G_STRINGIFY(PLUGS_PER_HOUR) ")";
            g_warning("plug gate closed: %s — %u failure(s) in the current "
                      "run; clients refused (SERVER_FAIL) until %s (%" 
                      G_GINT64_FORMAT " s). A GNOME session re-login "
                      "clears mutter and reopens the gate", why, g.fail_run,
                      hms ? hms : "?", secs);
            g_free(hms);
            if (at)
                g_date_time_unref(at);
        }
        EVS(srv)->gate_logged = g.verdict;
    }
    return g.verdict == GATE_OPEN ? SREMFB_STATUS_OK
                                  : SREMFB_STATUS_SERVER_FAIL;
}

/* A connector lit up: the failure run is over. */
static void plug_lit(SremfbServer *srv)
{
    PlugGate g = plug_gate(srv, g_get_monotonic_time());

    if (g.fail_run > 0)
        g_message("plug gate: connector lit after %u failure(s), back to "
                  "normal", g.fail_run);
    plug_record(srv, SREMFB_PLUG_EV_LIT);
    EVS(srv)->gate_logged = GATE_OPEN;
}

/* Self-heal: once a mode-timeout proved that mutter wedges on the
 * devices we inherited, stop trusting pre-existing free devices: create a
 * brand-new one at acquire time and plug it immediately. Bounded per
 * failure run, so a compositor that is truly gone does not get cards
 * forever (and past the budget the plug gate backs off). Returns the new
 * card index, or -1. */
static int self_heal_add(SremfbServer *srv)
{
    PlugGate g = plug_gate(srv, g_get_monotonic_time());

    if (g.fail_run > SELFHEAL_BUDGET) {
        g_debug("self-heal budget spent (%u failures) — a session "
                "re-login will clear mutter", g.fail_run);
        return -1;
    }
    return evdi_add_usable();
}

/* ------------------------------------------------------ power save */

/*
 * Night of 2026-11-06, 03:18:30: Diana's unattended APT run restarted
 * sremfb-client on Milim while Riza had been blanked since 01:43. Every
 * earlier reconnect (28 over three weeks) happened right after a "dpms
 * on" — the client's panel waking is what makes it reconnect — and lit in
 * 0-3 s: the pooled card replays the live KMS state (evdi patch,
 * "Reconnected without modeset"), or mutter modesets a fresh plug. This
 * one came with the screens in power save, and in power save mutter
 * modesets nothing at all: meta_kms_device_disable() has turned every
 * CRTC off (so the evdi replay, which needs an active CRTC, has nothing
 * to replay), pending mode sets are held back
 * (meta_renderer_native_pop_pending_mode_set) and only replayed when the
 * session wakes up (on_power_save_mode_changed -> modes reset). A hotplug
 * does not wake it. The 10 s timer then read mutter's silence as a
 * wedge: quarantine, eight fresh cards burnt, then the loop on the
 * quarantined card until the OOM — while the client's keyboard, the one
 * thing that could have woken the session, is only teleported once the
 * connector lights up.
 *
 * So each tick of the mode timer asks mutter for its PowerSaveMode
 * (property of org.gnome.Mutter.DisplayConfig, read afresh every time):
 * blanked, or woken less than SREMFB_PS_GRACE_US ago, the client is
 * parked — the connector stays plugged, nothing is recorded against the
 * compositor, and the client's USB devices are announced right away so
 * its keyboard can wake the session. The wake-up modesets our connector
 * like any other and on_mode_changed takes it from there. Unknown (no
 * session bus, no mutter): the 10 s rule as before.
 */
static int compositor_power_save(void)
{
    GError *err = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    GVariant *r, *v;
    int mode = -1;

    if (!bus) {
        g_debug("power save unknown: %s", err->message);
        g_error_free(err);
        return -1;
    }
    r = g_dbus_connection_call_sync(
        bus, "org.gnome.Mutter.DisplayConfig",
        "/org/gnome/Mutter/DisplayConfig",
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.gnome.Mutter.DisplayConfig",
                      "PowerSaveMode"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, NULL,
        &err);
    g_object_unref(bus);
    if (!r) {
        g_debug("power save unknown: %s", err->message);
        g_error_free(err);
        return -1;
    }
    g_variant_get(r, "(v)", &v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32))
        mode = g_variant_get_int32(v);
    g_variant_unref(v);
    g_variant_unref(r);
    return mode;
}

static gboolean on_mode_timeout(gpointer data)
{
    SremfbClient *c = data;
    gint64 now = g_get_monotonic_time();
    int ps = compositor_power_save();

    if (ps > 0)
        EV(c)->ps_seen = now;
    if (sremfb_mode_wait_verdict(ps, now, EV(c)->ps_seen) ==
        SREMFB_MODE_WAIT_PARK) {
        if (!EV(c)->usb_early) {
            g_message("[%s] compositor in power save (mode %d): connector "
                      "kept plugged until the session wakes up%s", c->macstr,
                      ps, c->usb_cap ? "" : " (client has no USB: only a "
                      "local input on this machine can wake it)");
            sremfb_usb_peer_add(c);    /* its keyboard can wake it */
            EV(c)->usb_early = TRUE;
        }
        return G_SOURCE_CONTINUE;      /* next look in 10 s */
    }

    EV(c)->mode_timeout_id = 0;
    g_warning("[%s] compositor did not light up the connector within 10s "
              "(is a Wayland/GNOME session running?)", c->macstr);
    if (EV(c)->dev) {
        /* mutter probably hit its EBUSY-on-reopen bug on this card:
         * quarantine it so the client's retry lands on another device,
         * and let the next acquire self-heal with a fresh device */
        EV(c)->dev->suspect = TRUE;
        EVS(c->srv)->wedge_seen = TRUE;
        g_warning("[%s] quarantining /dev/dri/card%d", c->macstr,
                  EV(c)->dev->card);
    }
    plug_record(c->srv, SREMFB_PLUG_EV_FAIL);
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
static void sremfb_evdi_plug(SremfbClient *c)
{
    sremfb_edid_build(EV(c)->edid, c->hello.xres, c->hello.yres,
                      client_serial(c), c->hello.model);
    /* evdi_connect2: the legacy evdi_connect derives the pixel-per-second
     * limit as area x 60, which silently drops our 120 Hz EDID mode */
    evdi_connect2(EV(c)->dev->handle, EV(c)->edid, sizeof(EV(c)->edid),
                  (uint32_t)c->hello.xres * c->hello.yres,
                  (uint32_t)c->hello.xres * c->hello.yres * 120u);
    /* after the connect: the kernel turns them off on every disconnect */
    evdi_enable_cursor_events(EV(c)->dev->handle, true);
    EV(c)->plugged = TRUE;
    plug_record(c->srv, SREMFB_PLUG_EV_PLUG);
    c->state = SREMFB_CLIENT_MODE_WAIT;
    EV(c)->mode_timeout_id = g_timeout_add_seconds(10, on_mode_timeout, c);
    g_message("[%s] connector plugged at %ux%u (serial 0x%08x), waiting "
              "for the compositor", c->macstr, c->hello.xres, c->hello.yres,
              client_serial(c));
}

/* "Unplug the cable". The compositor sees a disconnect and re-tiles, and
 * will restore the layout on the next plug (stable EDID identity). */
static void sremfb_evdi_unplug(SremfbClient *c)
{
    sremfb_xmit_reset(c);
    sremfb_ctl_stop(c);
    g_clear_handle_id(&EV(c)->mode_timeout_id, g_source_remove);
    g_clear_handle_id(&EV(c)->kick_id, g_source_remove);
    if (EV(c)->grab_registered) {
        evdi_unregister_buffer(EV(c)->dev->handle, GRAB_BUFFER_ID);
        EV(c)->grab_registered = FALSE;
    }
    cursor_reset(c);
    g_clear_pointer(&c->grabbuf, g_free);
    g_clear_pointer(&c->shadowbuf, g_free);
    g_clear_pointer(&c->rectbuf, g_free);
    g_clear_pointer(&c->sendbuf, g_free);
    c->rectbuf_size = c->sendbuf_size = 0;
    EV(c)->mode_valid = FALSE;
    EV(c)->update_pending = FALSE;
    if (EV(c)->plugged) {
        evdi_disconnect(EV(c)->dev->handle);
        EV(c)->plugged = FALSE;
        g_message("[%s] connector unplugged", c->macstr);
    }
}

/* ------------------------------------------------------ device setup */

/* Cold-boot only: recreate fresh evdi devices so the compositor gets
 * brand-new cards (it does not survive a close/reopen of a card it was
 * already driving — EBUSY on reopen, hotplugs then ignored; the
 * quarantine above is the runtime fallback).
 *
 * Crucially this must NOT run on a mid-session *restart*: by then the
 * compositor (gnome-shell / Xwayland) holds every evdi /dev/dri/cardN
 * open, and remove_all here removes the card from under it, forcing a
 * reopen that wedges mutter. A per-session marker in $XDG_RUNTIME_DIR
 * (tmpfs: gone on logout and reboot, kept across a daemon restart within
 * the same session) tells the two apart — on a restart we keep the
 * devices in place. It lives there, not in /run, because the server runs
 * as the session user and cannot write /run; and $XDG_RUNTIME_DIR being
 * cleared on relogin is correct — a fresh session gets a fresh mutter, so
 * the reset is safe again. The flock probe only catches a second *server*
 * instance, not the compositor, so it cannot stand in for this.
 *
 * Needs write access to /sys/devices/evdi/{remove_all,add}: the udev rule
 * shipped with the package hands them to group "video". */
void sremfb_evdi_reset(unsigned count)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    char marker[256];

    g_snprintf(marker, sizeof(marker), "%s/sremfb-evdi-reset",
               rundir && *rundir ? rundir : "/run");
    /* The marker only protects devices the compositor holds: when none
     * is left (removed on purpose, e.g. a game mode that unplugs the
     * cards for VR, or a new session that kept $XDG_RUNTIME_DIR), a
     * restart must create them again instead of waiting forever. */
    gboolean usable = FALSE;
    for (int i = 0; i < EVDI_MAX_CARDS && !usable; i++)
        usable = evdi_check_device(i) == AVAILABLE && !card_dead(i);
    if (g_file_test(marker, G_FILE_TEST_EXISTS) && usable) {
        g_message("evdi reset skipped: restart (keeping the devices the "
                  "compositor already holds open)");
        return;
    }

    for (int i = 0; i < EVDI_MAX_CARDS; i++) {
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
    if (rfd < 0 || access("/sys/devices/evdi/add", W_OK) < 0) {
        g_message("evdi reset skipped (no write access to "
                  "/sys/devices/evdi — udev rule missing or group)");
        if (rfd >= 0)
            close(rfd);
        return;
    }
    if (write(rfd, "1", 1) < 0)
        g_warning("evdi remove_all failed: %s", g_strerror(errno));
    close(rfd);
    g_usleep(300 * 1000);
    /* one by one, skipping the numbers mutter already knows (see
     * evdi_add_usable) */
    unsigned made = 0;
    for (unsigned i = 0; i < count; i++)
        if (evdi_add_usable() >= 0)
            made++;
    g_message("evdi devices reset: %u fresh device(s)", made);
    /* Mark the cold-boot reset as done so a later restart keeps the
     * compositor's devices instead of wedging it. */
    g_file_set_contents(marker, "", 0, NULL);
}

gboolean sremfb_evdi_probe(void)
{
    for (int i = 0; i < EVDI_MAX_CARDS; i++)
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

    for (guint i = 0; i < EVS(srv)->devices->len; i++) {
        SremfbEvdiDevice *dev = g_ptr_array_index(EVS(srv)->devices, i);
        if (dev->owner || (dev->suspect && !allow_suspect))
            continue;
        dev->owner = c;
        EV(c)->dev = dev;
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
    /* The first opener of a DRM primary node becomes its master. mutter
     * hands an unused evdi card back to logind after a while (its fd
     * stays open, but no longer master): our lock fd would then grab
     * master and mutter's TakeDevice on hotplug fails for good with
     * "Failed to reopen cardN: EBUSY". Never keep it. Harmless when
     * someone else is already master (EINVAL/EACCES, ignored). */
    (void)ioctl(lock_fd, DRM_IOCTL_DROP_MASTER_, 0);
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
    g_ptr_array_add(EVS(c->srv)->devices, dev);
    EV(c)->dev = dev;
    g_message("[%s] using evdi device /dev/dri/card%d%s", c->macstr, i, how);
    return TRUE;
}

static gboolean sremfb_evdi_acquire(SremfbClient *c)
{
    SremfbServer *srv = c->srv;

    if (acquire_pooled(c, FALSE))
        return TRUE;

    /* Once a wedge was seen, pre-existing free devices are mutter
     * minefields: hand out a brand-new one instead — created right now,
     * so the EDID plug lands within mutter's short acceptance window. */
    if (EVS(srv)->wedge_seen) {
        int fresh = self_heal_add(srv);
        if (fresh >= 0 && acquire_card(c, fresh, " (fresh, self-heal)"))
            return TRUE;
    }

    for (int i = 0; i < EVDI_MAX_CARDS; i++) {
        if (evdi_check_device(i) != AVAILABLE || card_dead(i))
            continue;
        if (acquire_card(c, i, ""))
            return TRUE;
    }

    /* nothing clean left: a quarantined card is better than a refusal */
    return acquire_pooled(c, TRUE);
}

/* The compositor's name for the card's single connector ("DVI-I-2"):
 * DRM numbers connectors per type across all cards, so sysfs'
 * card<N>-<name> is unique and is what mutter reports. */
static void set_connector(SremfbClient *c)
{
    char prefix[16];
    GDir *d = g_dir_open("/sys/class/drm", 0, NULL);
    const char *n;

    c->connector[0] = '\0';
    if (!d)
        return;
    g_snprintf(prefix, sizeof(prefix), "card%d-", EV(c)->dev->card);
    while ((n = g_dir_read_name(d)))
        if (g_str_has_prefix(n, prefix)) {
            g_strlcpy(c->connector, n + strlen(prefix), sizeof(c->connector));
            break;
        }
    g_dir_close(d);
}

/* The device stays open and flocked: only the ownership is returned. */
static void sremfb_evdi_release(SremfbClient *c)
{
    if (EV(c)->dev) {
        EV(c)->dev->owner = NULL;
        EV(c)->dev = NULL;
    }
}

/* --------------------------------------------------------- source ops */

/* Claim a device and plug an EDID. The compositor lights the connector
 * asynchronously; on_mode_changed then transitions the client to
 * STREAMING and sends the server hello. */
static int evdi_source_acquire(SremfbClient *c)
{
    /* the gate again, right before the hotplug: admit() and acquire()
     * run back to back today, but no path may plug past it */
    if (plug_gate(c->srv, g_get_monotonic_time()).verdict != GATE_OPEN)
        return SREMFB_STATUS_SERVER_FAIL;
    c->src_ctx = g_new0(SremfbEvdiClient, 1);
    if (!sremfb_evdi_acquire(c)) {
        g_message("[%s] no free evdi device — raise initial_device_count "
                  "in /etc/modprobe.d/sremfb.conf (or "
                  "echo 1 > /sys/devices/evdi/add)", c->macstr);
        g_clear_pointer(&c->src_ctx, g_free);
        return SREMFB_STATUS_NO_DEVICE;
    }
    set_connector(c);
    sremfb_evdi_plug(c);
    return SREMFB_STATUS_OK;
}

static void evdi_source_release(SremfbClient *c)
{
    if (!c->src_ctx)
        return;
    sremfb_evdi_unplug(c);
    sremfb_evdi_release(c);
    g_clear_pointer(&c->src_ctx, g_free);
}

const struct sremfb_source_ops sremfb_evdi_ops = {
    .acquire = evdi_source_acquire,
    .release = evdi_source_release,
    .admit = evdi_source_admit,
};

void sremfb_evdi_close_all(SremfbServer *srv)
{
    if (!EVS(srv)->devices)
        return;
    for (guint i = 0; i < EVS(srv)->devices->len; i++) {
        SremfbEvdiDevice *dev = g_ptr_array_index(EVS(srv)->devices, i);
        g_clear_handle_id(&dev->watch_id, g_source_remove);
        evdi_close(dev->handle);
        close(dev->lock_fd);
        g_free(dev);
    }
    g_ptr_array_free(EVS(srv)->devices, TRUE);
    EVS(srv)->devices = NULL;
    if (EVS(srv)->plug_events)
        g_array_free(EVS(srv)->plug_events, TRUE);
    EVS(srv)->plug_events = NULL;
}
