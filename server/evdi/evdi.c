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
#define DRM_IOCTL_DROP_MASTER_ _IO('d', 0x1f)  /* DRM_IOCTL_DROP_MASTER */

/* backend-private state, hung off the generic core structs */
#define EV(c)    ((SremfbEvdiClient *)(c)->src_ctx)
#define EVS(srv) ((SremfbEvdiState *)(srv)->src)

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

    if (!EV(c)->grab_registered)
        return;
    evdi_grab_pixels(EV(c)->dev->handle, rects, &num);
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
        g_clear_handle_id(&EV(c)->mode_timeout_id, g_source_remove);
        sremfb_xmit_hello(c, flags);
        c->state = SREMFB_CLIENT_STREAMING;
        c->st_grabs = c->st_rects = c->st_wire_bytes = c->st_raw_bytes = 0;
        c->st_since_us = g_get_monotonic_time();
        sremfb_ctl_start(c);
        /* a connector lighting up proves mutter is alive and accepting
         * hotplugs — rearm the self-heal budget (it only guards against
         * creating devices forever when mutter is truly gone) */
        EVS(c->srv)->selfheal_left = 8;
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

/* Self-heal: once a mode-timeout proved that mutter wedges on the
 * devices we inherited, stop trusting pre-existing free devices: create a
 * brand-new one at acquire time and plug it immediately. Bounded, so a
 * compositor that is truly gone does not get cards forever. Returns the
 * new card index, or -1. */
static int self_heal_add(SremfbServer *srv)
{
    if (EVS(srv)->selfheal_left == 0) {
        g_warning("self-heal budget spent — a session re-login will "
                  "clear mutter");
        return -1;
    }
    EVS(srv)->selfheal_left--;
    return evdi_add_usable();
}

static gboolean on_mode_timeout(gpointer data)
{
    SremfbClient *c = data;

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
    EV(c)->plugged = TRUE;
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
}
