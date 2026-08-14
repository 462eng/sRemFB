/*
 * SPICE frame source: acts as a spice-client-glib client of a QEMU/KVM VM
 * (qxl display) and feeds its canvas into the sRemFB core exactly like the
 * EVDI backend feeds a virtual monitor. One process = one VM = one
 * SpiceSession; every connected sremfb client mirrors the same canvas
 * (fan-out), scaling to its own geometry when it differs from the guest.
 *
 * SPICE -> sRemFB mapping:
 *   display-primary-create  -> snapshot the surface, repaint every client
 *   display-invalidate      -> damage, fanned out (blit + xmit) per client
 *   display-primary-destroy -> BLANK (guest mode change / reboot)
 *   display-mark            -> gate streaming until the display is valid
 *   channel closed/error    -> source lost on every client + reconnect
 *
 * PING/PONG, RAW/LZ4, RGB565+dither, the adaptive H.264 episodes and the
 * per-client non-blocking queue are all core: unchanged, downstream of
 * here.
 */
#include <stdlib.h>
#include <string.h>

#include <lz4.h>

#include "spice.h"

#define SP(srv) ((SremfbSpiceState *)(srv)->src)
#define SPC(c)  ((SremfbSpiceClient *)(c)->src_ctx)

static void spice_on_disconnect(SremfbSpiceState *st);

static unsigned bytespp(const SremfbClient *c)
{
    return c->hello.pixfmt == SREMFB_PIX_RGB565 ? 2 : 4;
}

/* ------------------------------------------------------ frame buffers */

static void alloc_buffers(SremfbClient *c)
{
    int w = c->geom.width, h = c->geom.height;
    unsigned bpp = bytespp(c);

    c->grabbuf = g_malloc0((size_t)w * h * 4);
    c->shadowbuf = g_malloc0((size_t)w * h * 4);
    c->rectbuf_size = (size_t)w * h * bpp;
    c->sendbuf_size = sizeof(struct sremfb_frame_hdr) +
                      (size_t)LZ4_compressBound((int)c->rectbuf_size);
    c->rectbuf = g_malloc(c->rectbuf_size);
    c->sendbuf = g_malloc(c->sendbuf_size);
}

static void free_buffers(SremfbClient *c)
{
    g_clear_pointer(&c->grabbuf, g_free);
    g_clear_pointer(&c->shadowbuf, g_free);
    g_clear_pointer(&c->rectbuf, g_free);
    g_clear_pointer(&c->sendbuf, g_free);
    c->rectbuf_size = c->sendbuf_size = 0;
}

/* ------------------------------------------------------- blit + scale */

/* Letterbox-fit scale of the guest canvas into the client geometry. */
static void scale_params(const SremfbSpiceState *st, const SremfbClient *c,
                         double *s, int *ox, int *oy, int *dw, int *dh)
{
    double rx = (double)c->geom.width / st->width;
    double ry = (double)c->geom.height / st->height;

    *s = rx < ry ? rx : ry;
    *dw = (int)(st->width * *s + 0.5);
    *dh = (int)(st->height * *s + 0.5);
    *ox = (c->geom.width - *dw) / 2;
    *oy = (c->geom.height - *dh) / 2;
}

/* Copies a guest damage rect (sx,sy,sw,sh) into the client's grabbuf,
 * scaling if needed. Returns the damaged rect in *stream* coordinates via
 * *out, or FALSE if nothing landed. */
static gboolean blit_region(SremfbSpiceState *st, SremfbClient *c,
                            int sx, int sy, int sw, int sh,
                            struct sremfb_rect *out)
{
    int pw = st->width, ph = st->height, pstride = st->stride;
    uint8_t *gb = c->grabbuf;
    int gw = c->geom.width;

    if (!st->data)
        return FALSE;
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw > pw) sw = pw - sx;
    if (sy + sh > ph) sh = ph - sy;
    if (sw <= 0 || sh <= 0)
        return FALSE;

    if (!SPC(c)->scale) {
        for (int row = 0; row < sh; row++)
            memcpy(gb + ((size_t)(sy + row) * gw + sx) * 4,
                   st->data + (size_t)(sy + row) * pstride + (size_t)sx * 4,
                   (size_t)sw * 4);
        *out = (struct sremfb_rect){ sx, sy, sx + sw, sy + sh };
        return TRUE;
    }

    double s; int ox, oy, dw, dh;
    scale_params(st, c, &s, &ox, &oy, &dw, &dh);
    int dx0 = (int)(ox + sx * s);
    int dy0 = (int)(oy + sy * s);
    int dx1 = (int)(ox + (sx + sw) * s + 0.999);   /* round outward */
    int dy1 = (int)(oy + (sy + sh) * s + 0.999);
    if (dx0 < ox) dx0 = ox;
    if (dy0 < oy) dy0 = oy;
    if (dx1 > ox + dw) dx1 = ox + dw;
    if (dy1 > oy + dh) dy1 = oy + dh;
    if (dx1 <= dx0 || dy1 <= dy0)
        return FALSE;

    for (int dy = dy0; dy < dy1; dy++) {
        int syy = (int)((dy - oy) / s);
        if (syy < 0) syy = 0; else if (syy >= ph) syy = ph - 1;
        const uint8_t *srow = st->data + (size_t)syy * pstride;
        uint8_t *drow = gb + (size_t)dy * gw * 4;
        for (int dx = dx0; dx < dx1; dx++) {
            int sxx = (int)((dx - ox) / s);
            if (sxx < 0) sxx = 0; else if (sxx >= pw) sxx = pw - 1;
            memcpy(drow + (size_t)dx * 4, srow + (size_t)sxx * 4, 4);
        }
    }
    *out = (struct sremfb_rect){ dx0, dy0, dx1, dy1 };
    return TRUE;
}

/* Payload-less control message (BLANK/UNBLANK) to one client. */
static void spice_send_ctrl(SremfbClient *c, uint8_t encoding)
{
    struct sremfb_frame_hdr hdr = {0};

    if (c->fd < 0 || c->state != SREMFB_CLIENT_STREAMING || c->lost_id)
        return;
    hdr.magic = SREMFB_MAGIC;
    hdr.encoding = encoding;
    sremfb_xmit_ctrl(c, &hdr, sizeof(hdr));
}

/* Repaint one client's whole frame from the current primary. */
static void repaint_client(SremfbSpiceState *st, SremfbClient *c)
{
    struct sremfb_rect out;

    if (!st->data)
        return;
    SPC(c)->scale = (st->width != c->geom.width || st->height != c->geom.height);
    if (SPC(c)->scale)
        memset(c->grabbuf, 0, (size_t)c->geom.width * c->geom.height * 4);
    blit_region(st, c, 0, 0, st->width, st->height, &out);
    spice_send_ctrl(c, SREMFB_ENC_UNBLANK);
    c->dirty_all = TRUE;
    sremfb_xmit_kick(c);
}

/* ------------------------------------------------------ client fan-out */

static gboolean is_streaming(const SremfbClient *c)
{
    return c->src_ctx && SPC(c)->streaming;
}

static void repaint_all(SremfbSpiceState *st)
{
    for (guint i = 0; i < st->srv->clients->len; i++) {
        SremfbClient *c = g_ptr_array_index(st->srv->clients, i);
        if (is_streaming(c))
            repaint_client(st, c);
    }
}

static void blank_all(SremfbSpiceState *st)
{
    for (guint i = 0; i < st->srv->clients->len; i++) {
        SremfbClient *c = g_ptr_array_index(st->srv->clients, i);
        if (is_streaming(c))
            spice_send_ctrl(c, SREMFB_ENC_BLANK);
    }
}

static void lost_all(SremfbSpiceState *st)
{
    for (guint i = 0; i < st->srv->clients->len; i++) {
        SremfbClient *c = g_ptr_array_index(st->srv->clients, i);
        if (c->src_ctx)
            sremfb_schedule_client_lost(c);
    }
}

static gboolean no_other_streaming(SremfbSpiceState *st, const SremfbClient *self)
{
    for (guint i = 0; i < st->srv->clients->len; i++) {
        SremfbClient *c = g_ptr_array_index(st->srv->clients, i);
        if (c != self && is_streaming(c))
            return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------ SPICE display signals */

static void on_primary_create(SpiceChannel *channel, gint format, gint width,
                              gint height, gint stride, gint shmid,
                              gpointer data, gpointer user)
{
    SremfbSpiceState *st = user;

    (void)channel; (void)shmid;
    st->fmt = format;
    st->width = width;
    st->height = height;
    st->stride = stride;
    st->data = data;
    st->have_primary = TRUE;
    if (format != SPICE_SURFACE_FMT_32_xRGB)
        g_warning("SPICE primary format %d (expected 32_xRGB=%d) — colours "
                  "may be wrong", format, SPICE_SURFACE_FMT_32_xRGB);
    g_message("SPICE primary %dx%d stride %d fmt %d", width, height, stride,
              format);
    if (st->marked)
        repaint_all(st);
}

static void on_primary_destroy(SpiceChannel *channel, gpointer user)
{
    SremfbSpiceState *st = user;

    (void)channel;
    st->have_primary = FALSE;
    st->data = NULL;
    g_message("SPICE primary destroyed (guest mode change/reboot) — blanking");
    blank_all(st);
}

static void on_invalidate(SpiceChannel *channel, gint x, gint y, gint w,
                          gint h, gpointer user)
{
    SremfbSpiceState *st = user;

    (void)channel;
    if (!st->have_primary || !st->marked || !st->data)
        return;
    for (guint i = 0; i < st->srv->clients->len; i++) {
        SremfbClient *c = g_ptr_array_index(st->srv->clients, i);
        struct sremfb_rect out;
        if (is_streaming(c) && blit_region(st, c, x, y, w, h, &out))
            sremfb_xmit_damage(c, &out, 1, bytespp(c));
    }
}

static void on_mark(SpiceChannel *channel, gboolean mark, gpointer user)
{
    SremfbSpiceState *st = user;

    (void)channel;
    st->marked = mark;
    g_message("SPICE display mark=%d", mark);
    if (mark && st->have_primary)
        repaint_all(st);
}

/* ---------------------------------------------- session / reconnection */

static void on_channel_event(SpiceChannel *channel, SpiceChannelEvent ev,
                             gpointer user)
{
    SremfbSpiceState *st = user;

    (void)channel;
    switch (ev) {
    case SPICE_CHANNEL_OPENED:
        st->backoff_s = 1;
        break;
    case SPICE_CHANNEL_CLOSED:
    case SPICE_CHANNEL_ERROR_CONNECT:
    case SPICE_CHANNEL_ERROR_TLS:
    case SPICE_CHANNEL_ERROR_LINK:
    case SPICE_CHANNEL_ERROR_AUTH:
    case SPICE_CHANNEL_ERROR_IO:
        spice_on_disconnect(st);
        break;
    default:
        break;
    }
}

static void on_channel_new(SpiceSession *session, SpiceChannel *channel,
                           gpointer user)
{
    SremfbSpiceState *st = user;

    (void)session;
    g_signal_connect(channel, "channel-event",
                     G_CALLBACK(on_channel_event), st);

    if (SPICE_IS_MAIN_CHANNEL(channel)) {
        st->main = SPICE_MAIN_CHANNEL(channel);
        g_message("SPICE main channel");
        return;
    }
    if (SPICE_IS_DISPLAY_CHANNEL(channel)) {
        gint id = 0;
        g_object_get(channel, "channel-id", &id, NULL);
        if (id != st->cfg.display_id)
            return;                    /* not the display we mirror */
        st->display = channel;
        g_signal_connect(channel, "display-primary-create",
                         G_CALLBACK(on_primary_create), st);
        g_signal_connect(channel, "display-primary-destroy",
                         G_CALLBACK(on_primary_destroy), st);
        g_signal_connect(channel, "display-invalidate",
                         G_CALLBACK(on_invalidate), st);
        g_signal_connect(channel, "display-mark",
                         G_CALLBACK(on_mark), st);
        spice_channel_connect(channel);
        g_message("SPICE display channel %d attached", id);
    }
}

static gboolean reconnect_cb(gpointer data)
{
    SremfbSpiceState *st = data;

    st->reconnect_id = 0;
    g_message("SPICE reconnecting to %s:%d", st->cfg.host, st->cfg.port);
    spice_session_connect(st->session);
    return G_SOURCE_REMOVE;
}

static void spice_on_disconnect(SremfbSpiceState *st)
{
    if (st->reconnect_id)
        return;                        /* already tearing down */

    g_message("SPICE session down — dropping clients, reconnecting");
    st->have_primary = FALSE;
    st->marked = FALSE;
    st->data = NULL;
    st->main = NULL;
    st->display = NULL;
    lost_all(st);
    spice_session_disconnect(st->session);

    int delay = st->backoff_s;
    st->backoff_s = delay < 30 ? delay * 2 : 30;
    st->reconnect_id = g_timeout_add_seconds(delay, reconnect_cb, st);
}

/* -------------------------------------------------------- source ops */

static void request_resize(SremfbSpiceState *st, int w, int h)
{
    if (!st->main)
        return;
    spice_main_channel_update_display(st->main, st->cfg.display_id,
                                      0, 0, w, h, TRUE);
    spice_main_channel_send_monitor_config(st->main);
    g_message("requested guest resize to %dx%d (vdagent)", w, h);
}

static void spice_stream_start(SremfbClient *c)
{
    uint8_t flags = 0;

    if (c->feedback)
        flags |= SREMFB_SRV_FLAG_PING;
    if (c->feedback && c->h264_cap && !c->h264_failed &&
        getenv("SREMFB_NO_H264") == NULL)
        flags |= SREMFB_SRV_FLAG_H264;
    sremfb_xmit_hello(c, flags);
    c->state = SREMFB_CLIENT_STREAMING;
    c->st_grabs = c->st_rects = c->st_wire_bytes = c->st_raw_bytes = 0;
    c->st_since_us = g_get_monotonic_time();
    SPC(c)->streaming = TRUE;
    sremfb_ctl_start(c);
    repaint_client(SP(c->srv), c);
    g_message("[%s] streaming %dx%d from SPICE%s", c->macstr,
              c->geom.width, c->geom.height, SPC(c)->scale ? " (scaled)" : "");
}

static int spice_acquire(SremfbClient *c)
{
    SremfbSpiceState *st = SP(c->srv);

    if (!st->have_primary || !st->marked)
        return SREMFB_STATUS_SERVER_FAIL;   /* VM not ready: client retries */

    c->geom.width = c->hello.xres;
    c->geom.height = c->hello.yres;
    gboolean scale = (st->width != c->geom.width ||
                      st->height != c->geom.height);
    if (scale && st->cfg.resize == SREMFB_RESIZE_OFF)
        return SREMFB_STATUS_BAD_HELLO;

    c->src_ctx = g_new0(SremfbSpiceClient, 1);
    SPC(c)->scale = scale;
    alloc_buffers(c);

    /* first client drives the guest resolution; the rest scale */
    if (scale && st->cfg.resize == SREMFB_RESIZE_AGENT &&
        no_other_streaming(st, c))
        request_resize(st, c->geom.width, c->geom.height);

    spice_stream_start(c);
    return SREMFB_STATUS_OK;
}

static void spice_release(SremfbClient *c)
{
    if (!c->src_ctx)
        return;
    sremfb_xmit_reset(c);
    sremfb_ctl_stop(c);
    free_buffers(c);
    g_clear_pointer(&c->src_ctx, g_free);
}

const struct sremfb_source_ops sremfb_spice_ops = {
    .acquire = spice_acquire,
    .release = spice_release,
};

/* ------------------------------------------------------- session setup */

void sremfb_spice_start(SremfbServer *srv)
{
    SremfbSpiceState *st = SP(srv);
    char portstr[16];

    st->srv = srv;
    st->backoff_s = 1;
    st->session = spice_session_new();
    g_snprintf(portstr, sizeof(portstr), "%d", st->cfg.port);

    g_object_set(st->session, "host", st->cfg.host, NULL);
    if (st->cfg.tls) {
        g_object_set(st->session, "tls-port", portstr, NULL);
        if (st->cfg.ca_file)
            g_object_set(st->session, "ca-file", st->cfg.ca_file, NULL);
    } else {
        g_object_set(st->session, "port", portstr, NULL);
    }
    if (st->cfg.password)
        g_object_set(st->session, "password", st->cfg.password, NULL);

    g_signal_connect(st->session, "channel-new",
                     G_CALLBACK(on_channel_new), st);
    g_message("connecting to SPICE %s:%d%s", st->cfg.host, st->cfg.port,
              st->cfg.tls ? " (TLS)" : "");
    spice_session_connect(st->session);
}
