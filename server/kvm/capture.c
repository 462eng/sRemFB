/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * Source de frames V4L2 de sremfb-kvm (§4.3) : le pont HDMI -> CSI-2
 * TC358743 vu par unicam en mode legacy (/dev/video0 : EDID, timings et
 * format se pilotent sur le nœud vidéo, sans media-ctl).
 *
 * Correspondance avec le cœur sRemFB :
 *   V4L2_EVENT_SOURCE_CHANGE -> probe_signal() (débouncé, + timer 1 s
 *                               tant qu'il n'y a pas de signal)
 *   pas de signal            -> STREAMOFF, BLANK à tous les clients
 *   nouveau mode             -> restream, puis RESIZE=scale (letterbox +
 *                               UNBLANK) ou reconnect (client coupé)
 *   frame (DQBUF)            -> diff par tuiles contre la frame précédente
 *                               (retenue hors du driver, zéro copie) ->
 *                               conversion des seules tuiles sales vers
 *                               l'image maître BGRx -> fan-out (blit +
 *                               sremfb_xmit_damage) à chaque client
 *
 * Un process = une cible ; tous les clients regardent la même image, à
 * leur géométrie (letterbox plus proche voisin si elle diffère, comme le
 * backend SPICE). Sans device (SREMFB_KVM_DEVICE=test:… / file:…), la
 * source synthétique de testsrc.c remplace DQBUF par un appel direct à
 * sremfb_kvm_frame() : même chemin diff -> convert -> fan-out.
 *
 * Tout le chemin matériel (S_EDID, DV_TIMINGS, SOURCE_CHANGE, formats)
 * est écrit d'après la doc V4L2 et linux/videodev2.h : à valider sur Pi.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <glib-unix.h>
#include <linux/videodev2.h>
#include <lz4.h>

#include "kvm.h"

#define KS(srv) ((SremfbKvmState *)(srv)->src)
#define KC(c)   ((SremfbKvmClient *)(c)->src_ctx)

#define KVM_MAX_RECTS   16          /* comme dirty[] du cœur */
#define KVM_DEBOUNCE_US 300000      /* au plus un restream / 300 ms */
#define KVM_POLL_MS     1000        /* QUERY_DV_TIMINGS tant que sans signal */
#define KVM_STATS_S     10

#ifndef SREMFB_KVM_ENC
#define SREMFB_KVM_ENC  "?"         /* nom de l'encodeur lié (Makefile) */
#endif

/* Mode détecté par probe_signal(). */
typedef struct {
    int w, h;
    unsigned fps;
    gboolean have_timings;          /* t valide (pas pour un device sans
                                       DV timings ni en mode test) */
    struct v4l2_dv_timings t;
} KvmMode;

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;

    do
        r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

static const char *fourcc_str(uint32_t f, char out[5])
{
    for (int i = 0; i < 4; i++) {
        char ch = (char)((f >> (8 * i)) & 0xFF);
        out[i] = g_ascii_isprint(ch) ? ch : '?';
    }
    out[4] = '\0';
    return out;
}

static unsigned client_bpp(const SremfbClient *c)
{
    return c->hello.pixfmt == SREMFB_PIX_RGB565 ? 2 : 4;
}

static gboolean is_streaming(const SremfbClient *c)
{
    return c->src_ctx && KC(c)->streaming;
}

/* ------------------------------------------------------------ letterbox */

void sremfb_kvm_fit(struct sremfb_kvm_fit *f, int sw, int sh, int dw, int dh)
{
    f->src_w = sw;
    f->src_h = sh;
    f->dst_w = dw;
    f->dst_h = dh;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || (sw == dw && sh == dh)) {
        f->s = 1.0;
        f->ox = f->oy = 0;
        f->dw = dw;
        f->dh = dh;
        return;
    }
    double rx = (double)dw / sw, ry = (double)dh / sh;
    f->s = rx < ry ? rx : ry;
    f->dw = MIN((int)(sw * f->s + 0.5), dw);
    f->dh = MIN((int)(sh * f->s + 0.5), dh);
    f->ox = (dw - f->dw) / 2;
    f->oy = (dh - f->dh) / 2;
}

static gboolean fit_identity(const struct sremfb_kvm_fit *f)
{
    return f->s == 1.0 && f->ox == 0 && f->oy == 0 &&
           f->src_w == f->dst_w && f->src_h == f->dst_h;
}

/* ------------------------------------------------------ buffers client */

/* Repris de spice_source.c (§4.10 : factorisation laissée locale). */
static void alloc_buffers(SremfbClient *c)
{
    int w = c->geom.width, h = c->geom.height;
    unsigned bpp = client_bpp(c);

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

/* ---------------------------------------------------------- blit + scale */

/* Copie un rect source (pixels de l'image maître) dans le grabbuf du
 * client selon son fit : memcpy ligne à ligne à l'échelle 1, plus proche
 * voisin sinon (code de blit_region() de spice_source.c). Rend le rect
 * touché en coordonnées flux (arrondi vers l'extérieur) dans *out. */
static gboolean blit_rect(const SremfbKvmState *st, SremfbClient *c,
                          const struct sremfb_rect *r, struct sremfb_rect *out)
{
    const struct sremfb_kvm_fit *f = &KC(c)->fit;
    int pw = st->src_w, ph = st->src_h;
    size_t pstride = (size_t)pw * 4;
    uint8_t *gb = c->grabbuf;
    int gw = c->geom.width;
    int sx = MAX(r->x1, 0), sy = MAX(r->y1, 0);
    int sx2 = MIN(r->x2, pw), sy2 = MIN(r->y2, ph);

    if (!st->master || !gb || f->src_w != pw || f->src_h != ph ||
        sx2 <= sx || sy2 <= sy)
        return FALSE;

    if (f->s == 1.0) {
        int dx = f->ox + sx, dy = f->oy + sy;
        int w = MIN(sx2, sx + (f->dst_w - dx)) - sx;
        int h = MIN(sy2, sy + (f->dst_h - dy)) - sy;
        if (w <= 0 || h <= 0)
            return FALSE;
        for (int row = 0; row < h; row++)
            memcpy(gb + ((size_t)(dy + row) * gw + dx) * 4,
                   st->master + (size_t)(sy + row) * pstride + (size_t)sx * 4,
                   (size_t)w * 4);
        *out = (struct sremfb_rect){ dx, dy, dx + w, dy + h };
        return TRUE;
    }

    double s = f->s;
    int ox = f->ox, oy = f->oy;
    int dx0 = (int)(ox + sx * s);
    int dy0 = (int)(oy + sy * s);
    int dx1 = (int)(ox + sx2 * s + 0.999);
    int dy1 = (int)(oy + sy2 * s + 0.999);
    dx0 = MAX(dx0, ox);
    dy0 = MAX(dy0, oy);
    dx1 = MIN(dx1, ox + f->dw);
    dy1 = MIN(dy1, oy + f->dh);
    if (dx1 <= dx0 || dy1 <= dy0)
        return FALSE;

    for (int dy = dy0; dy < dy1; dy++) {
        int syy = CLAMP((int)((dy - oy) / s), 0, ph - 1);
        const uint32_t *srow = (const uint32_t *)(st->master + (size_t)syy * pstride);
        uint32_t *drow = (uint32_t *)(gb + (size_t)dy * gw * 4);
        for (int dx = dx0; dx < dx1; dx++)
            drow[dx] = srow[CLAMP((int)((dx - ox) / s), 0, pw - 1)];
    }
    *out = (struct sremfb_rect){ dx0, dy0, dx1, dy1 };
    return TRUE;
}

/* Message de contrôle sans charge (BLANK/UNBLANK), par la file. */
static void send_ctrl(SremfbClient *c, uint8_t encoding)
{
    struct sremfb_frame_hdr hdr = {0};

    if (c->fd < 0 || c->state != SREMFB_CLIENT_STREAMING || c->lost_id)
        return;
    hdr.magic = SREMFB_MAGIC;
    hdr.encoding = encoding;
    sremfb_xmit_ctrl(c, &hdr, sizeof(hdr));
}

/* Image complète d'un client depuis l'image maître : bandes noires,
 * UNBLANK, plein cadre. Seulement quand le master contient une frame
 * (prev >= 0) — sinon c'est la première frame qui s'en chargera. */
static void repaint_client(SremfbKvmState *st, SremfbClient *c)
{
    struct sremfb_rect full = { 0, 0, st->src_w, st->src_h }, out;

    if (!st->signal || !st->master || st->prev < 0) {
        KC(c)->repaint = TRUE;
        return;
    }
    KC(c)->repaint = FALSE;
    if (!fit_identity(&KC(c)->fit))
        memset(c->grabbuf, 0, (size_t)c->geom.width * c->geom.height * 4);
    blit_rect(st, c, &full, &out);
    send_ctrl(c, SREMFB_ENC_UNBLANK);
    c->dirty_all = TRUE;
    sremfb_xmit_kick(c);
}

static void fanout(SremfbKvmState *st, const struct sremfb_rect *rects, int n)
{
    if (!st->srv->clients)             /* avant sremfb_serve() */
        return;
    for (guint i = 0; i < st->srv->clients->len; i++) {
        SremfbClient *c = g_ptr_array_index(st->srv->clients, i);
        struct sremfb_rect out[KVM_MAX_RECTS];
        int k = 0;

        if (!is_streaming(c))
            continue;
        if (KC(c)->repaint) {
            c->st_grabs++;
            repaint_client(st, c);
            continue;
        }
        for (int j = 0; j < n && k < KVM_MAX_RECTS; j++)
            if (blit_rect(st, c, &rects[j], &out[k]))
                k++;
        if (k > 0) {
            c->st_grabs++;
            /* le trim contre shadowbuf est fait par le cœur */
            sremfb_xmit_damage(c, out, k, client_bpp(c));
        }
    }
}

static void blank_all(SremfbKvmState *st)
{
    if (!st->srv->clients)
        return;
    for (guint i = 0; i < st->srv->clients->len; i++) {
        SremfbClient *c = g_ptr_array_index(st->srv->clients, i);
        if (is_streaming(c)) {
            send_ctrl(c, SREMFB_ENC_BLANK);
            KC(c)->repaint = FALSE;
        }
    }
}

/* Nouveau mode de la cible : politique SREMFB_KVM_RESIZE (§4.3.4). */
static void apply_resize(SremfbKvmState *st)
{
    if (!st->srv->clients)
        return;
    for (guint i = 0; i < st->srv->clients->len; i++) {
        SremfbClient *c = g_ptr_array_index(st->srv->clients, i);
        gboolean same;

        if (!is_streaming(c))
            continue;
        same = c->geom.width == st->src_w && c->geom.height == st->src_h;
        if (!same && st->resize == SREMFB_KVM_RESIZE_RECONNECT &&
            st->geometry == SREMFB_KVM_GEOM_SOURCE) {
            g_message("[%s] mode de la cible %dx%d != flux %dx%d : client "
                      "coupé (SREMFB_KVM_RESIZE=reconnect)", c->macstr,
                      st->src_w, st->src_h, c->geom.width, c->geom.height);
            sremfb_schedule_client_lost(c);
            continue;
        }
        sremfb_kvm_fit(&KC(c)->fit, st->src_w, st->src_h,
                       c->geom.width, c->geom.height);
        memset(c->grabbuf, 0, (size_t)c->geom.width * c->geom.height * 4);
        if (!same)
            g_message("[%s] mode %dx%d dans le flux %dx%d : letterbox "
                      "%dx%d+%d+%d", c->macstr, st->src_w, st->src_h,
                      c->geom.width, c->geom.height, KC(c)->fit.dw,
                      KC(c)->fit.dh, KC(c)->fit.ox, KC(c)->fit.oy);
        KC(c)->repaint = TRUE;         /* UNBLANK + plein cadre à la 1re frame */
    }
}

/* ------------------------------------------------------------ frames */

static void release_buf(SremfbKvmState *st, int idx)
{
    if (idx < 0 || idx >= st->nbuf)
        return;
    if (st->fd >= 0) {
        struct v4l2_buffer b = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = (uint32_t)idx,
        };
        if (xioctl(st->fd, VIDIOC_QBUF, &b) < 0) {
            g_warning("capture : QBUF %d : %s", idx, g_strerror(errno));
            return;
        }
    }
    st->buf[idx].queued = TRUE;
}

void sremfb_kvm_frame(SremfbKvmState *st, int idx)
{
    struct sremfb_rect rects[KVM_MAX_RECTS];
    const uint8_t *cur = st->buf[idx].map;
    const uint8_t *prev = st->prev >= 0 ? st->buf[st->prev].map : NULL;
    int n, nr = 0;

    st->buf[idx].queued = FALSE;
    st->frames++;
    n = sremfb_kvm_diff(st, cur, prev);
    if (n > 0) {
        st->frames_dirty++;
        st->tiles_dirty_total += (guint64)n;
        nr = sremfb_kvm_rects(st, rects, KVM_MAX_RECTS);
        for (int i = 0; i < nr; i++)
            sremfb_kvm_convert_rect(st, cur, &rects[i]);
    }
    /* la frame précédente retourne au driver, la courante la remplace */
    release_buf(st, st->prev);
    st->prev = idx;
    fanout(st, rects, nr);
}

static void signal_lost(SremfbKvmState *st, const char *why);

static gboolean on_frame_ready(gint fd, GIOCondition cond, gpointer data)
{
    SremfbKvmState *st = data;
    struct v4l2_buffer b = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    /* un seul DQBUF par passage : la boucle respire */
    if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
        if (errno == EAGAIN && !(cond & G_IO_ERR))
            return G_SOURCE_CONTINUE;
        /* perte de signal annoncée par le driver, ou flux cassé : on
         * relâche le watch (sinon POLLERR boucle) et on resonde */
        g_message("capture : DQBUF : %s — nouvelle détection",
                  g_strerror(errno));
        st->frame_watch_id = 0;
        st->signal = FALSE;            /* force le restream si le mode tient */
        sremfb_kvm_source_changed(st);
        return G_SOURCE_REMOVE;
    }
    if ((int)b.index >= st->nbuf)
        return G_SOURCE_CONTINUE;
    st->buf[b.index].queued = FALSE;
    if ((b.flags & V4L2_BUF_FLAG_ERROR) || b.bytesused == 0 ||
        b.bytesused < st->src_stride * (size_t)st->src_h) {
        /* frame tronquée (changement de mode en cours) */
        release_buf(st, (int)b.index);
        return G_SOURCE_CONTINUE;
    }
    sremfb_kvm_frame(st, (int)b.index);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------ flux V4L2 */

static void stop_stream(SremfbKvmState *st)
{
    g_clear_handle_id(&st->frame_watch_id, g_source_remove);
    if (st->fd >= 0) {
        if (st->streaming) {
            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (xioctl(st->fd, VIDIOC_STREAMOFF, &type) < 0)
                g_warning("capture : STREAMOFF : %s", g_strerror(errno));
        }
        for (int i = 0; i < st->nbuf; i++)
            if (st->buf[i].map && st->buf[i].map != MAP_FAILED)
                munmap(st->buf[i].map, st->buf[i].len);
        if (st->nbuf > 0) {
            struct v4l2_requestbuffers rb = {
                .count = 0,
                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                .memory = V4L2_MEMORY_MMAP,
            };
            xioctl(st->fd, VIDIOC_REQBUFS, &rb);
        }
    } else if (st->test) {
        sremfb_kvm_test_stop(st->test);
    }
    memset(st->buf, 0, sizeof(st->buf));
    st->nbuf = 0;
    st->prev = -1;
    st->streaming = FALSE;
    st->signal = FALSE;
}

/* S_DV_TIMINGS + S_FMT (UYVY, repli RGB24) ; relit le format négocié. */
static gboolean v4l2_set_format(SremfbKvmState *st, KvmMode *m)
{
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    uint32_t want[2] = { st->pixfmt, 0 };
    char fs[5];

    if (m->have_timings && xioctl(st->fd, VIDIOC_S_DV_TIMINGS, &m->t) < 0)
        g_warning("capture : S_DV_TIMINGS : %s", g_strerror(errno));
    if (st->pixfmt == V4L2_PIX_FMT_UYVY)
        want[1] = V4L2_PIX_FMT_RGB24;

    for (int i = 0; i < 2 && want[i]; i++) {
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = (uint32_t)m->w;
        fmt.fmt.pix.height = (uint32_t)m->h;
        fmt.fmt.pix.pixelformat = want[i];
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (xioctl(st->fd, VIDIOC_S_FMT, &fmt) < 0) {
            g_warning("capture : S_FMT %s : %s", fourcc_str(want[i], fs),
                      g_strerror(errno));
            continue;
        }
        if (fmt.fmt.pix.pixelformat == want[i])
            break;
        g_message("capture : le pont refuse %s (propose %s)",
                  fourcc_str(want[i], fs),
                  fourcc_str(fmt.fmt.pix.pixelformat, fs));
    }
    uint32_t got = fmt.fmt.pix.pixelformat;
    if (got != V4L2_PIX_FMT_UYVY && got != V4L2_PIX_FMT_RGB24) {
        g_warning("capture : ni UYVY ni RGB24 acceptés — pas de flux");
        return FALSE;
    }
    st->src_fourcc = got;
    st->src_w = (int)fmt.fmt.pix.width;
    st->src_h = (int)fmt.fmt.pix.height;
    st->src_stride = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline :
                     (size_t)st->src_w * sremfb_kvm_src_bpp(got);
    st->src_size = fmt.fmt.pix.sizeimage;
    /* §4.5 / §13 Q4 : range réel du pont, à valider sur matériel */
    st->full_range = got == V4L2_PIX_FMT_UYVY &&
                     fmt.fmt.pix.quantization == V4L2_QUANTIZATION_FULL_RANGE;
    g_message("capture : format %s %dx%d stride %zu sizeimage %zu "
              "colorspace %u quantization %u (%s range)",
              fourcc_str(got, fs), st->src_w, st->src_h, st->src_stride,
              st->src_size, fmt.fmt.pix.colorspace, fmt.fmt.pix.quantization,
              st->full_range ? "full" : "limited");
    return st->src_w > 0 && st->src_h > 0;
}

static gboolean v4l2_start(SremfbKvmState *st)
{
    struct v4l2_requestbuffers rb = {
        .count = SREMFB_KVM_NBUF,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (xioctl(st->fd, VIDIOC_REQBUFS, &rb) < 0) {
        g_warning("capture : REQBUFS : %s", g_strerror(errno));
        return FALSE;
    }
    st->nbuf = (int)MIN(rb.count, (uint32_t)SREMFB_KVM_NBUF);
    if (st->nbuf < 2) {
        g_warning("capture : %u buffer(s) seulement", rb.count);
        return FALSE;
    }
    for (int i = 0; i < st->nbuf; i++) {
        struct v4l2_buffer b = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = (uint32_t)i,
        };
        if (xioctl(st->fd, VIDIOC_QUERYBUF, &b) < 0) {
            g_warning("capture : QUERYBUF %d : %s", i, g_strerror(errno));
            return FALSE;
        }
        st->buf[i].len = b.length;
        st->buf[i].map = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, st->fd, b.m.offset);
        if (st->buf[i].map == MAP_FAILED) {
            st->buf[i].map = NULL;
            g_warning("capture : mmap %d : %s", i, g_strerror(errno));
            return FALSE;
        }
        if (b.length < st->src_stride * (size_t)st->src_h) {
            g_warning("capture : buffer %d trop petit (%u)", i, b.length);
            return FALSE;
        }
    }
    for (int i = 0; i < st->nbuf; i++)
        release_buf(st, i);
    if (xioctl(st->fd, VIDIOC_STREAMON, &type) < 0) {
        g_warning("capture : STREAMON : %s", g_strerror(errno));
        return FALSE;
    }
    st->frame_watch_id = g_unix_fd_add(st->fd, G_IO_IN | G_IO_ERR,
                                       on_frame_ready, st);
    return TRUE;
}

static gboolean start_stream(SremfbKvmState *st, KvmMode *m)
{
    gboolean ok;

    if (st->fd >= 0) {
        if (!v4l2_set_format(st, m))
            return FALSE;
    } else {
        st->src_w = m->w;
        st->src_h = m->h;
        st->src_fourcc = V4L2_PIX_FMT_UYVY;
        st->src_stride = (size_t)m->w * 2;
        st->src_size = st->src_stride * (size_t)m->h;
        st->full_range = FALSE;
    }
    st->src_fps = m->fps;
    g_free(st->master);
    st->master = g_malloc0((size_t)st->src_w * st->src_h * 4);
    sremfb_kvm_tiles_alloc(st);
    st->prev = -1;

    st->streaming = TRUE;              /* STREAMOFF à défaire si échec */
    if (st->fd >= 0) {
        ok = v4l2_start(st);
    } else {
        st->nbuf = SREMFB_KVM_NBUF;
        ok = sremfb_kvm_test_start(st->test);
    }
    if (!ok) {
        stop_stream(st);
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------- signal / timings */

static gboolean poll_cb(gpointer data);

static void arm_poll(SremfbKvmState *st)
{
    if (!st->poll_id)
        st->poll_id = g_timeout_add(KVM_POLL_MS, poll_cb, st);
}

static void signal_lost(SremfbKvmState *st, const char *why)
{
    gboolean was = st->signal || st->streaming;
    gboolean blank = was || st->src_w > 0;

    stop_stream(st);
    st->src_w = st->src_h = 0;
    st->src_fps = 0;
    if (blank) {
        g_message("capture : signal perdu (%s) — écran noir aux clients", why);
        blank_all(st);
    } else if (!st->lost_logged) {
        g_message("capture : pas de signal (%s) — en attente", why);
    }
    st->lost_logged = TRUE;
    arm_poll(st);
}

/* Détection du mode courant. FALSE = pas de signal (why renseigné). */
static gboolean query_mode(SremfbKvmState *st, KvmMode *m, const char **why)
{
    memset(m, 0, sizeof(*m));
    if (st->fd < 0) {
        *why = "source de test éteinte";
        return st->test && sremfb_kvm_test_query(st->test, &m->w, &m->h, &m->fps);
    }
    if (xioctl(st->fd, VIDIOC_QUERY_DV_TIMINGS, &m->t) < 0) {
        int e = errno;
        if (e == ENOTTY) {
            /* device sans DV timings (webcam de dev…) : format courant */
            struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
            if (xioctl(st->fd, VIDIOC_G_FMT, &fmt) < 0) {
                *why = "G_FMT impossible";
                return FALSE;
            }
            m->w = (int)fmt.fmt.pix.width;
            m->h = (int)fmt.fmt.pix.height;
            return m->w > 0 && m->h > 0;
        }
        *why = e == ENOLINK ? "pas de lien" :
               e == ENOLCK  ? "signal instable" :
               e == ERANGE  ? "mode hors plage" : g_strerror(e);
        return FALSE;
    }
    if (m->t.type != V4L2_DV_BT_656_1120) {
        *why = "timings non BT.656/1120";
        return FALSE;
    }
    const struct v4l2_bt_timings *bt = &m->t.bt;
    uint64_t frame = (uint64_t)V4L2_DV_BT_FRAME_WIDTH(bt) *
                     V4L2_DV_BT_FRAME_HEIGHT(bt);
    m->w = (int)bt->width;
    m->h = (int)bt->height;
    m->fps = frame ? (unsigned)((bt->pixelclock + frame / 2) / frame) : 0;
    m->have_timings = TRUE;
    if (bt->interlaced)
        g_message("capture : source entrelacée (non gérée, image en champs)");
    if (m->w <= 0 || m->h <= 0) {
        *why = "timings vides";
        return FALSE;
    }
    return TRUE;
}

static void probe_signal(SremfbKvmState *st)
{
    KvmMode m;
    const char *why = "";
    char fs[5];

    if (!query_mode(st, &m, &why)) {
        signal_lost(st, why);
        return;
    }
    if (st->streaming && st->signal && m.w == st->src_w && m.h == st->src_h)
        return;

    gboolean had = st->src_w > 0;
    stop_stream(st);
    if (!start_stream(st, &m)) {
        signal_lost(st, "flux impossible");
        return;
    }
    st->signal = TRUE;
    st->lost_logged = FALSE;
    st->last_restream_us = g_get_monotonic_time();
    g_clear_handle_id(&st->poll_id, g_source_remove);
    g_message("capture : signal %s — mode %dx%dp%u %s", had ? "changé" :
              "présent", st->src_w, st->src_h, st->src_fps,
              fourcc_str(st->src_fourcc, fs));
    apply_resize(st);
}

static gboolean poll_cb(gpointer data)
{
    SremfbKvmState *st = data;

    st->poll_id = 0;
    probe_signal(st);
    if (!st->signal)
        arm_poll(st);
    return G_SOURCE_REMOVE;
}

static gboolean debounce_cb(gpointer data)
{
    SremfbKvmState *st = data;

    st->debounce_id = 0;
    probe_signal(st);
    return G_SOURCE_REMOVE;
}

/* Rafale d'événements (BIOS -> chargeur -> OS) : un probe différé, pas
 * plus d'un restream par 300 ms. */
void sremfb_kvm_source_changed(SremfbKvmState *st)
{
    gint64 wait;

    if (st->debounce_id)
        return;
    wait = st->last_restream_us + KVM_DEBOUNCE_US - g_get_monotonic_time();
    st->debounce_id = g_timeout_add((guint)MAX(wait / 1000, 20),
                                    debounce_cb, st);
}

static gboolean on_event(gint fd, GIOCondition cond, gpointer data)
{
    SremfbKvmState *st = data;
    struct v4l2_event ev;
    gboolean changed = FALSE;

    (void)cond;
    for (;;) {
        memset(&ev, 0, sizeof(ev));
        if (xioctl(fd, VIDIOC_DQEVENT, &ev) < 0)
            break;
        if (ev.type == V4L2_EVENT_SOURCE_CHANGE)
            changed = TRUE;
    }
    if (changed)
        sremfb_kvm_source_changed(st);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------- EDID */

/* Lit et vérifie un EDID (blocs de 128 octets, en-tête + checksums). */
static uint8_t *edid_load(const char *path, unsigned *blocks)
{
    gchar *data = NULL;
    gsize len = 0;
    GError *err = NULL;
    static const uint8_t hdr[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };

    if (!g_file_get_contents(path, &data, &len, &err)) {
        g_warning("EDID %s : %s", path, err->message);
        g_error_free(err);
        return NULL;
    }
    if (len < 128 || len % 128 || len > 4 * 128) {
        g_warning("EDID %s : %zu octets (attendu 128 ou 256, blocs de 128)",
                  path, (size_t)len);
        g_free(data);
        return NULL;
    }
    if (memcmp(data, hdr, 8) != 0) {
        g_warning("EDID %s : en-tête EDID absent (fichier hex ? il faut le "
                  ".bin binaire)", path);
        g_free(data);
        return NULL;
    }
    for (gsize b = 0; b < len / 128; b++) {
        uint8_t sum = 0;
        for (int i = 0; i < 128; i++)
            sum += (uint8_t)data[b * 128 + i];
        if (sum != 0) {
            g_warning("EDID %s : checksum du bloc %zu fausse (somme 0x%02x)",
                      path, (size_t)b, sum);
            g_free(data);
            return NULL;
        }
    }
    *blocks = (unsigned)(len / 128);
    return (uint8_t *)data;
}

/* Pose l'EDID sur le pont. TRUE = posé (ou rien à poser). */
static gboolean edid_apply(SremfbKvmState *st)
{
    unsigned blocks = 0;
    uint8_t *data;

    if (!st->edid_path || !*st->edid_path)
        return TRUE;
    data = edid_load(st->edid_path, &blocks);
    if (!data)
        return FALSE;
    if (st->fd < 0) {
        g_message("EDID %s : %u bloc(s) valide(s), non posé (pas de device)",
                  st->edid_path, blocks);
        g_free(data);
        return TRUE;
    }
    struct v4l2_edid e = {
        .pad = 0, .start_block = 0, .blocks = blocks, .edid = data,
    };
    gboolean ok = xioctl(st->fd, VIDIOC_S_EDID, &e) == 0;
    if (ok)
        g_message("EDID %s posé (%u blocs)", st->edid_path, blocks);
    else
        g_warning("EDID : S_EDID : %s — EDID du pont inchangé",
                  g_strerror(errno));
    g_free(data);
    return ok;
}

/* -------------------------------------------------------- statistiques */

static gboolean stats_cb(gpointer data)
{
    SremfbKvmState *st = data;
    gint64 now = g_get_monotonic_time();
    size_t ntiles = (size_t)st->tiles_x * st->tiles_y;

    if (!st->srv->clients)
        return G_SOURCE_CONTINUE;
    for (guint i = 0; i < st->srv->clients->len; i++) {
        SremfbClient *c = g_ptr_array_index(st->srv->clients, i);
        SremfbKvmClient *kc;
        char ctl[96] = "";
        if (!is_streaming(c))
            continue;
        kc = KC(c);
        double dt = (double)(now - kc->st_since_us) / G_USEC_PER_SEC;
        if (dt <= 0.5)
            continue;
        guint64 df = st->frames - kc->st_frames;
        guint64 dd = st->frames_dirty - kc->st_frames_dirty;
        guint64 dtl = st->tiles_dirty_total - kc->st_tiles;
        double pct = df && ntiles ? 100.0 * (double)dtl / ((double)df * ntiles)
                                  : 0.0;
        if (c->feedback)
            g_snprintf(ctl, sizeof(ctl), " [%s, délai %.0f ms%s]",
                       sremfb_ctl_state_name(c), c->delay_ewma_us / 1000.0,
                       c->enc ? ", encodeur ouvert" : "");
        if (st->signal)
            g_message("[%s] %.1f fps capture, %.1f fps damage, %.0f %% tuiles "
                      "sales, mode %s, %.1f Mb/s wire (%.1f envois/s)%s",
                      c->macstr, (double)df / dt, (double)dd / dt, pct,
                      c->xmit_mode == SREMFB_XMIT_H264 ? "h264" : "raw",
                      (double)c->st_wire_bytes * 8.0 / dt / 1e6,
                      (double)c->st_rects / dt, ctl);
        else
            g_message("[%s] pas de signal, mode %s, %.1f Mb/s wire%s",
                      c->macstr,
                      c->xmit_mode == SREMFB_XMIT_H264 ? "h264" : "raw",
                      (double)c->st_wire_bytes * 8.0 / dt / 1e6, ctl);
        c->st_grabs = c->st_rects = c->st_wire_bytes = c->st_raw_bytes = 0;
        kc->st_since_us = now;
        kc->st_frames = st->frames;
        kc->st_frames_dirty = st->frames_dirty;
        kc->st_tiles = st->tiles_dirty_total;
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------- source ops */

static int kvm_acquire(SremfbClient *c)
{
    SremfbKvmState *st = KS(c->srv);
    SremfbKvmClient *kc;
    gboolean have = st->signal && st->src_w > 0 && st->src_h > 0;
    uint8_t flags = 0;

    if (st->fd < 0 && !st->test)
        return SREMFB_STATUS_SERVER_FAIL;

    /* géométrie fixée pour toute la connexion (§4.3.5) */
    if (st->geometry == SREMFB_KVM_GEOM_SOURCE && have) {
        c->geom.width = st->src_w;
        c->geom.height = st->src_h;
    } else {
        c->geom.width = c->hello.xres;
        c->geom.height = c->hello.yres;
    }

    c->src_ctx = kc = g_new0(SremfbKvmClient, 1);
    alloc_buffers(c);
    sremfb_kvm_fit(&kc->fit, have ? st->src_w : 0, have ? st->src_h : 0,
                   c->geom.width, c->geom.height);

    if (c->feedback)
        flags |= SREMFB_SRV_FLAG_PING;
    if (c->feedback && c->h264_cap && !c->h264_failed &&
        getenv("SREMFB_NO_H264") == NULL)
        flags |= SREMFB_SRV_FLAG_H264;
    flags |= sremfb_input_start(c);    /* avant le hello, comme evdi.c */
    sremfb_xmit_hello(c, flags);
    c->state = SREMFB_CLIENT_STREAMING;
    c->st_grabs = c->st_rects = c->st_wire_bytes = c->st_raw_bytes = 0;
    c->st_since_us = kc->st_since_us = g_get_monotonic_time();
    kc->st_frames = st->frames;
    kc->st_frames_dirty = st->frames_dirty;
    kc->st_tiles = st->tiles_dirty_total;
    kc->streaming = TRUE;
    sremfb_ctl_start(c);

    if (have)
        repaint_client(st, c);
    else
        send_ctrl(c, SREMFB_ENC_BLANK);
    g_message("[%s] flux %dx%d (%s)%s%s", c->macstr, c->geom.width,
              c->geom.height,
              st->geometry == SREMFB_KVM_GEOM_SOURCE && have ? "mode de la cible"
                                                             : "demandé par le client",
              have ? "" : ", pas de signal : écran noir",
              have && !fit_identity(&kc->fit) ? ", letterbox" : "");
    return SREMFB_STATUS_OK;
}

static void kvm_release(SremfbClient *c)
{
    if (!c->src_ctx)
        return;
    sremfb_xmit_reset(c);
    sremfb_ctl_stop(c);
    free_buffers(c);
    g_clear_pointer(&c->src_ctx, g_free);
}

const struct sremfb_source_ops sremfb_kvm_ops = {
    .acquire = kvm_acquire,
    .release = kvm_release,
};

/* ------------------------------------------------------ démarrage */

static gboolean is_synthetic(const char *dev)
{
    return g_str_has_prefix(dev, "test:") || g_str_has_prefix(dev, "file:");
}

/* open + QUERYCAP + abonnement aux événements. -1 = échec (journalisé). */
static int open_capture(const char *dev, gboolean verbose)
{
    struct v4l2_capability cap;
    uint32_t caps;
    int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    /* --probe : tout le rapport sur stdout */
    void (*out)(const gchar *, ...) = verbose ? g_print : g_printerr;

    if (fd < 0) {
        out("capture : %s : %s%s\n", dev, g_strerror(errno),
                   errno == ENOENT ? " (dtoverlay=tc358743 dans config.txt ? "
                   "module bcm2835-unicam chargé ?)" : "");
        return -1;
    }
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        out("capture : %s : QUERYCAP : %s (pas un device V4L2)\n", dev,
                   g_strerror(errno));
        close(fd);
        return -1;
    }
    caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                     : cap.capabilities;
    if (verbose)
        g_print("capture : %s : driver %s, carte \"%s\", bus %s, caps 0x%08x\n",
                dev, cap.driver, cap.card, cap.bus_info, caps);
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        out("capture : %s : ni VIDEO_CAPTURE+STREAMING (caps 0x%08x) — "
                   "unicam en mode media-controller ?\n", dev, caps);
        close(fd);
        return -1;
    }
    struct v4l2_event_subscription sub = { .type = V4L2_EVENT_SOURCE_CHANGE };
    if (xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0)
        out("capture : SOURCE_CHANGE non disponible (%s) — détection "
                  "par sondage seulement\n", g_strerror(errno));
    return fd;
}

int sremfb_kvm_start(SremfbServer *srv)
{
    SremfbKvmState *st = KS(srv);

    st->srv = srv;
    st->fd = -1;
    st->prev = -1;
    if (is_synthetic(st->device)) {
        st->test = sremfb_kvm_test_new(st, st->device);
        if (!st->test)
            return -1;
    } else {
        st->fd = open_capture(st->device, FALSE);
        if (st->fd < 0)
            return -1;
        g_message("capture : %s ouvert", st->device);
    }
    edid_apply(st);                    /* échec = avertissement seulement */
    probe_signal(st);                  /* pas de signal n'est pas une erreur */
    if (st->fd >= 0)
        st->fd_watch_id = g_unix_fd_add(st->fd, G_IO_PRI, on_event, st);
    st->stats_id = g_timeout_add_seconds(KVM_STATS_S, stats_cb, st);
    return 0;
}

void sremfb_kvm_stop(SremfbServer *srv)
{
    SremfbKvmState *st = KS(srv);

    g_clear_handle_id(&st->fd_watch_id, g_source_remove);
    g_clear_handle_id(&st->poll_id, g_source_remove);
    g_clear_handle_id(&st->debounce_id, g_source_remove);
    g_clear_handle_id(&st->stats_id, g_source_remove);
    stop_stream(st);
    if (st->fd >= 0) {
        close(st->fd);
        st->fd = -1;
    }
    g_clear_pointer(&st->test, sremfb_kvm_test_free);
    g_clear_pointer(&st->master, g_free);
    g_clear_pointer(&st->tile_dirty, g_free);
}

/* ------------------------------------------------------------ --probe */

static void probe_ctrl(int fd, uint32_t id, const char *name)
{
    struct v4l2_queryctrl q = { .id = id };

    if (xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0 ||
        (q.flags & V4L2_CTRL_FLAG_DISABLED))
        g_print("    %-22s absent\n", name);
    else
        g_print("    %-22s min %d max %d défaut %d\n", name, q.minimum,
                q.maximum, q.default_value);
}

static void probe_formats(int fd, uint32_t type, const char *what)
{
    struct v4l2_fmtdesc d;
    char fs[5];

    g_print("  formats %s :", what);
    for (uint32_t i = 0;; i++) {
        memset(&d, 0, sizeof(d));
        d.index = i;
        d.type = type;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &d) < 0) {
            if (i == 0)
                g_print(" (aucun)");
            break;
        }
        g_print(" %s", fourcc_str(d.pixelformat, fs));
    }
    g_print("\n");
}

static gboolean probe_capture(SremfbKvmState *st)
{
    KvmMode m;
    const char *why = "";
    char fs[5];

    if (is_synthetic(st->device)) {
        st->test = sremfb_kvm_test_new(st, st->device);
        if (!st->test)
            return FALSE;
        gboolean sig = query_mode(st, &m, &why);
        g_print("capture : source synthétique %s : %dx%d@%u\n", st->device,
                m.w, m.h, m.fps);
        edid_apply(st);
        g_clear_pointer(&st->test, sremfb_kvm_test_free);
        return sig;
    }
    st->fd = open_capture(st->device, TRUE);
    if (st->fd < 0)
        return FALSE;
    gboolean ok = edid_apply(st);
    g_print("  EDID : %s\n", !st->edid_path || !*st->edid_path ? "non demandé" :
            ok ? "posé" : "ÉCHEC (voir ci-dessus)");
    if (query_mode(st, &m, &why)) {
        if (m.have_timings)
            g_print("  signal : %dx%d%s@%u, pixelclock %llu Hz\n", m.w, m.h,
                    m.t.bt.interlaced ? "i" : "p", m.fps,
                    (unsigned long long)m.t.bt.pixelclock);
        else
            g_print("  signal : pas de DV timings, format courant %dx%d\n",
                    m.w, m.h);
    } else {
        g_print("  signal : aucun (%s)\n", why);
    }
    probe_formats(st->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, "capture");
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (xioctl(st->fd, VIDIOC_G_FMT, &fmt) == 0)
        g_print("  format courant : %s %ux%u stride %u colorspace %u "
                "quantization %u\n", fourcc_str(fmt.fmt.pix.pixelformat, fs),
                fmt.fmt.pix.width, fmt.fmt.pix.height,
                fmt.fmt.pix.bytesperline, fmt.fmt.pix.colorspace,
                fmt.fmt.pix.quantization);
    if (m.w > 0 && m.h > 0) {
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = (uint32_t)m.w;
        fmt.fmt.pix.height = (uint32_t)m.h;
        fmt.fmt.pix.pixelformat = st->pixfmt;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (xioctl(st->fd, VIDIOC_TRY_FMT, &fmt) == 0)
            g_print("  TRY_FMT %s -> %s %ux%u\n", fourcc_str(st->pixfmt, fs),
                    fourcc_str(fmt.fmt.pix.pixelformat, fs),
                    fmt.fmt.pix.width, fmt.fmt.pix.height);
    }
    close(st->fd);
    st->fd = -1;
    return TRUE;                       /* sans signal, le device est là */
}

static gboolean probe_encoder(const char *dev)
{
    struct v4l2_capability cap;
    int fd;

    if (!dev || !*dev) {
        g_print("encodeur : non configuré\n");
        return FALSE;
    }
    fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        g_print("encodeur : %s : %s%s\n", dev, g_strerror(errno),
                errno == ENOENT ? " (module bcm2835-codec ? gpu_mem=128 ?)" : "");
        return FALSE;
    }
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        g_print("encodeur : %s : QUERYCAP : %s\n", dev, g_strerror(errno));
        close(fd);
        return FALSE;
    }
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                            : cap.capabilities;
    gboolean m2m = (caps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0;
    g_print("encodeur : %s : driver %s, carte \"%s\", caps 0x%08x%s\n", dev,
            cap.driver, cap.card, caps, m2m ? "" : " (PAS M2M_MPLANE)");
    probe_formats(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "entrée (OUTPUT)");
    probe_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "sortie (CAPTURE)");
    g_print("  contrôles :\n");
    probe_ctrl(fd, V4L2_CID_MPEG_VIDEO_BITRATE_MODE, "BITRATE_MODE");
    probe_ctrl(fd, V4L2_CID_MPEG_VIDEO_BITRATE, "BITRATE");
    probe_ctrl(fd, V4L2_CID_MPEG_VIDEO_H264_PROFILE, "H264_PROFILE");
    probe_ctrl(fd, V4L2_CID_MPEG_VIDEO_H264_LEVEL, "H264_LEVEL");
    probe_ctrl(fd, V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, "REPEAT_SEQ_HEADER");
    probe_ctrl(fd, V4L2_CID_MPEG_VIDEO_HEADER_MODE, "HEADER_MODE");
    probe_ctrl(fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, "H264_I_PERIOD");
    probe_ctrl(fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, "GOP_SIZE");
    probe_ctrl(fd, V4L2_CID_MPEG_VIDEO_B_FRAMES, "B_FRAMES");
    probe_ctrl(fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, "FORCE_KEY_FRAME");
    close(fd);
    return m2m;
}

/* Essai réel de l'encodeur lié, par l'API du cœur : une IDR, une P, une
 * IDR forcée, puis un débit hors bornes. */
static gboolean probe_encode(const char *dev)
{
    enum { W = 1920, H = 1080 };
    size_t luma = (size_t)W * H, chroma = luma / 4;
    struct SremfbEncoder *enc;
    gboolean ok = TRUE;
    uint8_t *yuv;

    if (dev && *dev)
        setenv("SREMFB_KVM_ENCODER", dev, 1);
    g_print("  essai d'encodage (encodeur %s) : %dx%d, 8000 kb/s\n",
            SREMFB_KVM_ENC, W, H);
    enc = sremfb_enc_open(W, H, 8000);
    if (!enc) {
        g_print("    ouverture : ÉCHEC (voir ci-dessus)\n");
        return FALSE;
    }
    yuv = g_malloc(luma + 2 * chroma);
    for (int f = 0; f < 3; f++) {
        static const int want_idr[3] = { 1, 0, 1 };
        uint8_t *nal = NULL;
        int idr = 0;
        /* mire en dégradé qui glisse : du vrai travail pour les P */
        for (size_t i = 0; i < luma; i++)
            yuv[i] = (uint8_t)(16 + ((i % W + i / W + (size_t)f * 8) % 220));
        memset(yuv + luma, 128, 2 * chroma);
        gint64 t0 = g_get_monotonic_time();
        int sz = sremfb_enc_encode(enc, yuv, yuv + luma, yuv + luma + chroma,
                                   want_idr[f], &nal, &idr);
        gint64 dt = g_get_monotonic_time() - t0;
        gboolean good = sz > 0 && nal && idr == want_idr[f];
        g_print("    frame %d (%s demandée) : %d octets, %s, %.1f ms%s\n", f,
                want_idr[f] ? "IDR" : "P", sz, idr ? "IDR" : "non-IDR",
                dt / 1000.0, good ? "" : "  ÉCHEC");
        ok = ok && good;
        if (sz < 0)
            break;
    }
    sremfb_enc_set_bitrate(enc, 40000);
    g_print("    débit demandé 40000 kb/s -> %d kb/s retenus\n",
            sremfb_enc_bitrate(enc));
    sremfb_enc_close(enc);
    g_free(yuv);
    return ok;
}

int sremfb_kvm_probe(SremfbKvmState *st, const char *encoder,
                     const char *const hid[3])
{
    static const char *const hid_name[3] = { "clavier", "souris", "pointeur" };
    gboolean cap_ok, enc_ok, hid_ok = TRUE;

    st->fd = -1;
    st->prev = -1;
    cap_ok = probe_capture(st);
    /* x264 (dev PC) : pas de device à inspecter, seulement l'essai */
    enc_ok = g_strcmp0(SREMFB_KVM_ENC, "x264") == 0 || probe_encoder(encoder);
    if (enc_ok)
        enc_ok = probe_encode(encoder);
    for (int i = 0; i < 3; i++) {
        if (!hid[i] || !*hid[i]) {
            g_print("HID %s : désactivé (variable vide)\n", hid_name[i]);
            continue;
        }
        int fd = open(hid[i], O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            g_print("HID %s : %s : %s%s\n", hid_name[i], hid[i],
                    g_strerror(errno), errno == ENOENT ?
                    " (gadget USB : sremfb-kvm-gadget start ?)" : "");
            hid_ok = FALSE;
        } else {
            g_print("HID %s : %s OK\n", hid_name[i], hid[i]);
            close(fd);
        }
    }
    g_print("bilan : capture %s, encodeur %s, HID %s\n",
            cap_ok ? "OK" : "ÉCHEC", enc_ok ? "OK" : "ÉCHEC",
            hid_ok ? "OK" : "ÉCHEC");
    return cap_ok && enc_ok && hid_ok ? 0 : 1;
}
