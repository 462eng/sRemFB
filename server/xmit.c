/*
 * Per-client non-blocking transmit (the head-of-line fix, 1.2.0).
 *
 * The old path sent every frame with blocking send()s straight from the
 * damage handler: one client with a full socket buffer (slow link, slow
 * panel, or just a top-bar cat animation fanned out to every EVDI
 * output) stalled the single-threaded main loop, and every other client
 * stuttered with it.
 *
 * Here nothing ever blocks:
 *
 *  - Control messages (PING, BLANK/UNBLANK, H264 EOS, the server hello)
 *    are tiny; they are queued verbatim (GBytes FIFO) and drained when
 *    the socket is writable.
 *
 *  - Frame data is *never* queued. Damage only accumulates as a dirty
 *    region (16 rects, collapsing to a bounding box on overflow) plus
 *    grabbuf, which always holds the full current frame. The next frame
 *    message is built — converted, compressed or encoded — only when
 *    the socket can take it, from the freshest pixels available. A slow
 *    client therefore receives fewer, coalesced frames (exactly the
 *    frame-drop the Pi 3 display path already does) and can never hold
 *    anybody else's data hostage.
 *
 * Ordering on the wire is preserved: a partially sent frame finishes
 * first, then queued control messages, then the next frame is built.
 * The H.264 EOS + full-snapshot sequence that closes an episode maps
 * onto this as "queue EOS, mark everything dirty".
 */
#include <errno.h>
#include <string.h>

#include <glib-unix.h>
#include <lz4.h>

#include "sremfb-server.h"

#define XMIT_DIRTY_MAX  16          /* matches struct SremfbClient */

/* Encode pacing when the *decoder* is behind (Pi 3 tops out at
 * 1080p30): retry cadence while sremfb_ctl_skip_encode() says wait. */
#define XMIT_RETRY_MS   50

static gboolean on_out_ready(gint fd, GIOCondition cond, gpointer data);

static void xmit_fail(SremfbClient *c)
{
    g_message("[%s] send failed: %s", c->macstr, g_strerror(errno));
    sremfb_schedule_client_lost(c);
}

void sremfb_xmit_kick(SremfbClient *c)
{
    if (c->fd < 0 || c->lost_id || c->out_watch_id)
        return;
    if (!c->out_active && g_queue_is_empty(&c->outq) &&
        c->dirty_n == 0 && !c->dirty_all)
        return;
    c->out_watch_id = g_unix_fd_add(c->fd, G_IO_OUT | G_IO_ERR | G_IO_HUP,
                                    on_out_ready, c);
}

/* ------------------------------------------------------ control queue */

void sremfb_xmit_ctrl(SremfbClient *c, const void *msg, size_t len)
{
    if (c->fd < 0 || c->lost_id)
        return;
    g_queue_push_tail(&c->outq, g_bytes_new(msg, len));
    c->wire_total += len;          /* traffic intent (idle detection) */
    sremfb_xmit_kick(c);
}

void sremfb_xmit_hello(SremfbClient *c, uint8_t flags)
{
    struct sremfb_server_hello sh;

    net_fill_server_hello(&sh, (uint16_t)c->mode.width,
                          (uint16_t)c->mode.height, c->hello.pixfmt,
                          SREMFB_STATUS_OK, flags);
    sremfb_xmit_ctrl(c, &sh, sizeof(sh));
}

/* ------------------------------------------------------- dirty region */

static gboolean rects_touch(const struct evdi_rect *a,
                            const struct evdi_rect *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 &&
           a->y1 <= b->y2 && b->y1 <= a->y2;
}

static void rect_union(struct evdi_rect *a, const struct evdi_rect *b)
{
    a->x1 = MIN(a->x1, b->x1);
    a->y1 = MIN(a->y1, b->y1);
    a->x2 = MAX(a->x2, b->x2);
    a->y2 = MAX(a->y2, b->y2);
}

static void dirty_add(SremfbClient *c, const struct evdi_rect *r)
{
    if (c->dirty_all)
        return;
    for (int i = 0; i < c->dirty_n; i++)
        if (rects_touch(&c->dirty[i], r)) {
            rect_union(&c->dirty[i], r);
            return;
        }
    if (c->dirty_n < XMIT_DIRTY_MAX) {
        c->dirty[c->dirty_n++] = *r;
        return;
    }
    /* overflow: collapse everything into one bounding box */
    for (int i = 1; i < c->dirty_n; i++)
        rect_union(&c->dirty[0], &c->dirty[i]);
    rect_union(&c->dirty[0], r);
    c->dirty_n = 1;
}

/* Shrinks the rect to the rows that really differ from what the client
 * last got (shadowbuf). Not an optimization for honest damage — it's
 * there because mutter fans some top-bar animations (RunCat…) out as
 * *full-frame* damage on every output while the pixels are identical:
 * without the trim that costs a full frame on the wire per tick per
 * client, and 50-100 ms of decompress+blit on the SBC behind which the
 * cursor updates queue up. Returns FALSE when nothing really changed. */
static gboolean rect_trim(SremfbClient *c, struct evdi_rect *r)
{
    size_t stride = (size_t)c->mode.width * 4;
    size_t rowoff = (size_t)r->x1 * 4;
    size_t rowlen = (size_t)(r->x2 - r->x1) * 4;

    if (!c->shadowbuf)
        return TRUE;
    while (r->y1 < r->y2 &&
           memcmp(c->grabbuf + (size_t)r->y1 * stride + rowoff,
                  c->shadowbuf + (size_t)r->y1 * stride + rowoff,
                  rowlen) == 0)
        r->y1++;
    while (r->y2 > r->y1 &&
           memcmp(c->grabbuf + (size_t)(r->y2 - 1) * stride + rowoff,
                  c->shadowbuf + (size_t)(r->y2 - 1) * stride + rowoff,
                  rowlen) == 0)
        r->y2--;
    return r->y2 > r->y1;
}

/* Damage arrived (grabbuf freshly filled). Clamps, trims, accumulates,
 * arms. */
void sremfb_xmit_damage(SremfbClient *c, const struct evdi_rect *rects,
                        int num, unsigned bytespp)
{
    size_t raw = 0;

    for (int i = 0; i < num; i++) {
        struct evdi_rect r = {
            .x1 = CLAMP(rects[i].x1, 0, c->mode.width),
            .y1 = CLAMP(rects[i].y1, 0, c->mode.height),
            .x2 = CLAMP(rects[i].x2, 0, c->mode.width),
            .y2 = CLAMP(rects[i].y2, 0, c->mode.height),
        };
        if (r.x2 <= r.x1 || r.y2 <= r.y1)
            continue;
        if (!rect_trim(c, &r))
            continue;                  /* identical pixels: not damage */
        raw += (size_t)(r.x2 - r.x1) * (size_t)(r.y2 - r.y1) * bytespp;
        if (c->xmit_mode == SREMFB_XMIT_H264)
            c->dirty_all = TRUE;
        else
            dirty_add(c, &r);
    }
    if (raw == 0)
        return;
    c->dirty_raw += raw;
    if (c->feedback)
        sremfb_ctl_on_damage(c, raw);
    sremfb_xmit_kick(c);
}

/* --------------------------------------------------------- mode switch */

/* RAW -> H264: the first AU is an IDR, so it repaints the full frame and
 * there is no visual gap. H264 -> RAW: an EOS tells the client to drain
 * and display everything its decoder holds, then a full RAW/LZ4 snapshot
 * makes the screen pixel-exact before normal rects resume. */
void sremfb_xmit_set_mode(SremfbClient *c, SremfbXmitMode mode)
{
    if (c->xmit_mode == mode || c->state != SREMFB_CLIENT_STREAMING)
        return;

    if (mode == SREMFB_XMIT_H264) {
        uint32_t w = (uint32_t)c->mode.width, h = (uint32_t)c->mode.height;
        if (!c->enc) {
            int kbps = sremfb_ctl_initial_kbps(c);
            c->enc = sremfb_enc_open((int)w, (int)h, kbps);
            if (!c->enc) {
                g_warning("[%s] x264 init failed, staying raw", c->macstr);
                c->h264_failed = TRUE;
                return;
            }
            size_t luma = (size_t)w * h;
            size_t chroma = (size_t)((w + 1) / 2) * ((h + 1) / 2);
            c->yuvbuf = g_malloc(luma + 2 * chroma);
            g_message("[%s] switching to H.264 at %d kbps", c->macstr, kbps);
        } else {
            g_message("[%s] switching to H.264 at %d kbps", c->macstr,
                      sremfb_enc_bitrate(c->enc));
        }
        c->force_idr = TRUE;
        c->xmit_mode = SREMFB_XMIT_H264;
        if (c->dirty_n || c->dirty_all) {
            c->dirty_n = 0;
            c->dirty_all = TRUE;
        }
    } else {
        struct sremfb_frame_hdr eos = {0};

        g_message("[%s] switching back to raw", c->macstr);
        c->xmit_mode = SREMFB_XMIT_RAW;
        eos.magic = SREMFB_MAGIC;
        eos.encoding = SREMFB_ENC_H264_EOS;
        sremfb_xmit_ctrl(c, &eos, sizeof(eos));
        c->dirty_n = 0;
        c->dirty_all = TRUE;           /* the deterministic snapshot */
        sremfb_xmit_kick(c);
    }
}

/* ------------------------------------------------------- frame builds */

/* Points out_seg at a built message. seg0 lives in c->sendbuf; seg1 (the
 * H.264 payload) lives in x264's buffer, valid until the next encode —
 * which cannot happen before this frame is fully sent. */
static void frame_start(SremfbClient *c, const uint8_t *seg0, size_t len0,
                        const uint8_t *seg1, size_t len1)
{
    c->out_seg[0] = seg0;
    c->out_seg_len[0] = len0;
    c->out_seg[1] = seg1;
    c->out_seg_len[1] = len1;
    c->out_off = 0;
    c->out_active = TRUE;

    c->st_rects++;
    c->st_wire_bytes += len0 + len1;
    c->wire_total += len0 + len1;
    if (c->feedback)
        sremfb_ctl_on_deliver(c, len0 + len1);
}

/* The rect just went out (or into the encoder): remember what the
 * client now shows, for the damage trim. */
static void shadow_update(SremfbClient *c, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h)
{
    size_t stride = (size_t)c->mode.width * 4;

    if (!c->shadowbuf)
        return;
    for (uint32_t row = 0; row < h; row++)
        memcpy(c->shadowbuf + (size_t)(y + row) * stride + (size_t)x * 4,
               c->grabbuf + (size_t)(y + row) * stride + (size_t)x * 4,
               (size_t)w * 4);
}

/* One RAW/LZ4 rect from the current grabbuf into sendbuf. */
static void build_raw_rect(SremfbClient *c, const struct evdi_rect *r)
{
    unsigned bytespp = (c->hello.pixfmt == SREMFB_PIX_RGB565) ? 2 : 4;
    uint32_t x = (uint32_t)r->x1, y = (uint32_t)r->y1;
    uint32_t w = (uint32_t)(r->x2 - r->x1), h = (uint32_t)(r->y2 - r->y1);
    size_t raw_len = (size_t)w * h * bytespp;
    struct sremfb_frame_hdr *hdr = (struct sremfb_frame_hdr *)c->sendbuf;
    uint8_t *payload = c->sendbuf + sizeof(*hdr);
    size_t out_len = raw_len;

    for (uint32_t row = 0; row < h; row++)
        sremfb_convert_bgrx_row(
            c->rectbuf + (size_t)row * w * bytespp,
            c->grabbuf + ((size_t)(y + row) * c->mode.width + x) * 4,
            w, c->hello.pixfmt, x, y + row);
    shadow_update(c, x, y, w, h);

    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = SREMFB_MAGIC;
    hdr->encoding = SREMFB_ENC_RAW;
    hdr->x = (uint16_t)x;
    hdr->y = (uint16_t)y;
    hdr->w = (uint16_t)w;
    hdr->h = (uint16_t)h;

    if (c->lz4) {
        int csz = LZ4_compress_fast((const char *)c->rectbuf,
                                    (char *)payload, (int)raw_len,
                                    LZ4_compressBound((int)raw_len), 1);
        if (csz > 0 && (size_t)csz < raw_len) {
            hdr->encoding = SREMFB_ENC_LZ4;
            out_len = (size_t)csz;
        }
    }
    if (hdr->encoding == SREMFB_ENC_RAW)
        memcpy(payload, c->rectbuf, raw_len);
    hdr->payload_len = (uint32_t)out_len;

    c->st_raw_bytes += raw_len;
    frame_start(c, c->sendbuf, sizeof(*hdr) + out_len, NULL, 0);
}

/* One full-frame H.264 access unit from the current grabbuf. */
static void build_h264_frame(SremfbClient *c)
{
    uint32_t w = (uint32_t)c->mode.width, h = (uint32_t)c->mode.height;
    size_t luma = (size_t)w * h;
    size_t chroma = (size_t)((w + 1) / 2) * ((h + 1) / 2);
    uint8_t *y = c->yuvbuf, *u = y + luma, *v = u + chroma;
    uint8_t *nal = NULL;
    int is_idr = 0;

    sremfb_convert_bgrx_to_i420(y, u, v, c->grabbuf, w, h);
    shadow_update(c, 0, 0, w, h);
    int sz = sremfb_enc_encode(c->enc, y, u, v, c->force_idr, &nal, &is_idr);
    c->force_idr = FALSE;
    if (sz < 0) {
        g_warning("[%s] x264 encode failed, falling back to raw", c->macstr);
        c->h264_failed = TRUE;
        sremfb_xmit_set_mode(c, SREMFB_XMIT_RAW);
        return;                        /* the snapshot repaints everything */
    }
    c->last_enc_us = g_get_monotonic_time();
    if (sz == 0)
        return;                        /* nothing out (shouldn't happen) */

    struct sremfb_frame_hdr *hdr = (struct sremfb_frame_hdr *)c->sendbuf;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = SREMFB_MAGIC;
    hdr->encoding = SREMFB_ENC_H264;
    hdr->reserved[0] = is_idr ? SREMFB_H264_FLAG_IDR : 0;
    hdr->w = (uint16_t)w;
    hdr->h = (uint16_t)h;
    hdr->payload_len = (uint32_t)sz;

    c->st_raw_bytes += c->dirty_raw;
    frame_start(c, c->sendbuf, sizeof(*hdr), nal, (size_t)sz);
}

static gboolean retry_cb(gpointer data)
{
    SremfbClient *c = data;

    c->out_retry_id = 0;
    sremfb_xmit_kick(c);
    return G_SOURCE_REMOVE;
}

/* Builds the next frame message, if any is due. Returns TRUE if one was
 * started (out_active). */
static gboolean build_next(SremfbClient *c)
{
    if (c->state != SREMFB_CLIENT_STREAMING || !c->grabbuf || !c->sendbuf)
        return FALSE;

    if (c->xmit_mode == SREMFB_XMIT_H264) {
        if (!c->dirty_all && c->dirty_n == 0)
            return FALSE;
        /* decoder behind (network is fine but the delay says the far
         * end can't chew faster): pace the encodes instead of piling
         * AUs into the TCP buffers */
        if (sremfb_ctl_skip_encode(c)) {
            if (!c->out_retry_id)
                c->out_retry_id = g_timeout_add(XMIT_RETRY_MS, retry_cb, c);
            return FALSE;
        }
        c->dirty_all = FALSE;
        c->dirty_n = 0;
        c->dirty_raw = 0;
        build_h264_frame(c);
        return c->out_active;
    }

    if (c->dirty_all) {
        struct evdi_rect full = {
            .x1 = 0, .y1 = 0,
            .x2 = c->mode.width, .y2 = c->mode.height,
        };
        c->dirty_all = FALSE;
        c->dirty_n = 0;
        c->dirty_raw = 0;
        build_raw_rect(c, &full);
        return c->out_active;
    }
    if (c->dirty_n > 0) {
        struct evdi_rect r = c->dirty[0];
        c->dirty_n--;
        memmove(&c->dirty[0], &c->dirty[1],
                (size_t)c->dirty_n * sizeof(r));
        if (c->dirty_n == 0)
            c->dirty_raw = 0;
        build_raw_rect(c, &r);
        return c->out_active;
    }
    return FALSE;
}

/* --------------------------------------------------------- the drain */

/* Pushes bytes out while the socket takes them. Never blocks; on EAGAIN
 * the G_IO_OUT watch stays armed. One frame build per pass, so the main
 * loop (other clients, evdi events) breathes under constant damage. */
static gboolean on_out_ready(gint fd, GIOCondition cond, gpointer data)
{
    SremfbClient *c = data;

    if (cond & (G_IO_ERR | G_IO_HUP)) {
        c->out_watch_id = 0;
        errno = EPIPE;
        xmit_fail(c);
        return G_SOURCE_REMOVE;
    }

    /* 1. finish the frame in flight */
    if (c->out_active) {
        size_t total = c->out_seg_len[0] + c->out_seg_len[1];
        while (c->out_off < total) {
            int i = c->out_off < c->out_seg_len[0] ? 0 : 1;
            size_t off = i ? c->out_off - c->out_seg_len[0] : c->out_off;
            ssize_t n = net_send_some(fd, c->out_seg[i] + off,
                                      c->out_seg_len[i] - off);
            if (n < 0) {
                c->out_watch_id = 0;
                xmit_fail(c);
                return G_SOURCE_REMOVE;
            }
            if (n == 0)
                return G_SOURCE_CONTINUE;      /* EAGAIN: wait writable */
            c->out_off += (size_t)n;
        }
        c->out_active = FALSE;
        c->out_off = 0;
    }

    /* 2. queued control messages */
    while (!g_queue_is_empty(&c->outq)) {
        GBytes *b = g_queue_peek_head(&c->outq);
        gsize len = 0;
        const uint8_t *p = g_bytes_get_data(b, &len);
        ssize_t n = net_send_some(fd, p + c->outq_off, len - c->outq_off);
        if (n < 0) {
            c->out_watch_id = 0;
            xmit_fail(c);
            return G_SOURCE_REMOVE;
        }
        c->outq_off += (size_t)n;
        if (c->outq_off < len)
            return G_SOURCE_CONTINUE;          /* EAGAIN mid-message */
        g_bytes_unref(g_queue_pop_head(&c->outq));
        c->outq_off = 0;
    }

    /* 3. build the next frame (one per pass) */
    if (build_next(c))
        return G_SOURCE_CONTINUE;

    /* idle: disarm */
    c->out_watch_id = 0;
    return G_SOURCE_REMOVE;
}

/* Drops everything in flight. For teardown and for mode changes (the
 * wire buffers are about to be reallocated). Queued control messages
 * are dropped too: a mode change is followed by a fresh hello, and
 * teardown closes the socket anyway. */
void sremfb_xmit_reset(SremfbClient *c)
{
    GBytes *b;

    g_clear_handle_id(&c->out_watch_id, g_source_remove);
    g_clear_handle_id(&c->out_retry_id, g_source_remove);
    while ((b = g_queue_pop_head(&c->outq)))
        g_bytes_unref(b);
    c->outq_off = 0;
    c->out_active = FALSE;
    c->out_off = 0;
    c->dirty_n = 0;
    c->dirty_all = FALSE;
    c->dirty_raw = 0;
    c->pings_outstanding = 0;      /* queued PINGs died with the queue */
}
