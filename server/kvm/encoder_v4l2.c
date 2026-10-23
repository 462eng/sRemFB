/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * Encodeur H.264 matériel du Raspberry Pi 4 (bcm2835-codec, rôle encode,
 * /dev/video11) derrière l'API sremfb_enc_* de core.h — même contrat que
 * core/encoder.c (x264) : le cœur ne sait pas qu'il parle au matériel
 * (doc/SPEC-kvm-backend.md §4.6).
 *
 * Interface V4L2 « stateful encoder », ioctls bruts (modèle :
 * client/v4l2dec.c, dans l'autre sens) :
 *
 *   open -> QUERYCAP (M2M_MPLANE) -> ENUM_FMT (YUV420 en entrée, H264 en
 *   sortie) -> S_FMT(CAPTURE)=H264 -> S_FMT(OUTPUT)=YUV420 (le driver
 *   arrondit bytesperline à 64 en encode : on relit la géométrie) ->
 *   S_PARM 60 i/s -> REQBUFS CAPTURE puis OUTPUT (le composant MMAL naît
 *   au premier REQBUFS) -> contrôles (après REQBUFS : le driver ne les
 *   pousse au firmware qu'une fois le composant créé, et on veut voir
 *   ceux qu'il refuse) -> QUERYBUF/mmap -> QBUF de tous les CAPTURE ->
 *   STREAMON OUTPUT et CAPTURE.
 *
 * Encodage synchrone, comme x264 : la frame I420 est copiée dans un
 * buffer OUTPUT (ligne à ligne si le stride du driver diffère), QBUF,
 * poll() jusqu'à l'access unit, DQBUF CAPTURE. Le buffer CAPTURE rendu à
 * l'appelant reste à lui jusqu'à l'appel suivant (« valid until the next
 * call ») et n'est remis en file qu'à ce moment-là. Une AU plus grande
 * qu'un buffer CAPTURE arrive en morceaux (même timestamp) : on les
 * recolle dans un tampon à nous.
 *
 * IDR seulement à la demande (force_idr -> FORCE_KEY_FRAME), SPS/PPS
 * répétés devant chaque IDR (REPEAT_SEQ_HEADER) : chaque épisode s'ouvre
 * sur une AU que le décodeur avale seul. Le VUI écrit par le firmware
 * n'est pas maîtrisé : par convention de protocole le flux est BT.601
 * limited, quoi qu'il dise.
 *
 * Débit : CBR du firmware, borné à 25 kb/s–25 Mb/s (limites du
 * contrôle). Il n'y a pas de VBV réglable comme chez x264 : une IDR peut
 * dépasser les ~100 ms de débit que x264 garantit.
 */
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "core.h"

#define ENC_DEFAULT_DEVICE "/dev/video11"
#define ENC_OUT_NBUF   2              /* entrée : une en vol, une à remplir */
#define ENC_CAP_NBUF   4
#define ENC_CAP_MAX    32             /* le driver peut en imposer plus */
#define ENC_CAP_MIN_SIZE (1u << 20)   /* 1 Mio, au moins w*h octets */
#define ENC_KBPS_MIN   25             /* bornes du contrôle BITRATE */
#define ENC_KBPS_MAX   25000
#define ENC_FPS        60             /* comme x264 : n'échelonne que le RC */
#define ENC_TIMEOUT_MS 200            /* par frame (§4.6) */
#define ENC_TIMEOUT_FIRST_MS 1000     /* 1re frame : démarrage du firmware */

struct enc_buf {
    uint8_t *map;
    size_t len;
    gboolean queued;
};

struct SremfbEncoder {
    int fd;
    char *dev;
    int w, h;
    int kbps;                  /* débit effectif (borné) */
    /* géométrie OUTPUT imposée par le driver */
    uint32_t bpl;              /* stride luma ; chroma = bpl / 2 */
    uint32_t plane_h;          /* hauteur des plans (>= h) */
    uint32_t out_size;         /* sizeimage */
    struct enc_buf out[ENC_OUT_NBUF];
    int n_out;
    struct enc_buf cap[ENC_CAP_MAX];
    int n_cap;
    int held;                  /* CAPTURE rendu à l'appelant (-1 : aucun) */
    uint8_t *agg;              /* AU recollée (fragments) */
    size_t agg_len, agg_size;
    gboolean on_out, on_cap;   /* STREAMON faits */
    guint64 frames;
    guint64 seq;               /* timestamp de la frame en cours (µs fictives) */
    gboolean warned_frag;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;

    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

static int clamp_kbps(int kbps)
{
    return CLAMP(kbps, ENC_KBPS_MIN, ENC_KBPS_MAX);
}

static gboolean has_format(int fd, uint32_t type, uint32_t fourcc)
{
    struct v4l2_fmtdesc d;

    for (uint32_t i = 0;; i++) {
        memset(&d, 0, sizeof(d));
        d.index = i;
        d.type = type;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &d) < 0)
            return FALSE;
        if (d.pixelformat == fourcc)
            return TRUE;
    }
}

static gboolean set_ctrl(struct SremfbEncoder *e, uint32_t id, int32_t val,
                         const char *name)
{
    struct v4l2_control c = { .id = id, .value = val };

    if (xioctl(e->fd, VIDIOC_S_CTRL, &c) < 0) {
        g_warning("encodeur %s : contrôle %s=%d refusé : %s", e->dev, name,
                  val, g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

/* Premier choix accepté d'une liste ; renvoie l'index, -1 si aucun. */
static int set_ctrl_first(struct SremfbEncoder *e, uint32_t id,
                          const int32_t *vals, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        struct v4l2_control c = { .id = id, .value = vals[i] };
        if (xioctl(e->fd, VIDIOC_S_CTRL, &c) == 0)
            return i;
    }
    g_warning("encodeur %s : aucune valeur de %s acceptée, défaut du driver",
              e->dev, name);
    return -1;
}

static void set_controls(struct SremfbEncoder *e)
{
    static const int32_t profiles[] = {
        V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
        V4L2_MPEG_VIDEO_H264_PROFILE_MAIN,
        V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE,
    };
    static const char *const profile_names[] = { "High", "Main",
                                                  "Constrained Baseline" };
    /* 4.2 couvre 1080p60 (ce que x264 annonce déjà avec fps = 60) */
    static const int32_t levels[] = {
        V4L2_MPEG_VIDEO_H264_LEVEL_4_2,
        V4L2_MPEG_VIDEO_H264_LEVEL_4_1,
        V4L2_MPEG_VIDEO_H264_LEVEL_4_0,
    };
    static const char *const level_names[] = { "4.2", "4.1", "4.0" };
    struct v4l2_queryctrl q = { .id = V4L2_CID_MPEG_VIDEO_H264_I_PERIOD };
    int32_t iperiod = INT32_MAX;
    int p, l;

    set_ctrl(e, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
             V4L2_MPEG_VIDEO_BITRATE_MODE_CBR, "BITRATE_MODE");
    set_ctrl(e, V4L2_CID_MPEG_VIDEO_BITRATE, e->kbps * 1000, "BITRATE");
    p = set_ctrl_first(e, V4L2_CID_MPEG_VIDEO_H264_PROFILE, profiles,
                       G_N_ELEMENTS(profiles), "H264_PROFILE");
    l = set_ctrl_first(e, V4L2_CID_MPEG_VIDEO_H264_LEVEL, levels,
                       G_N_ELEMENTS(levels), "H264_LEVEL");
    set_ctrl(e, V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 1, "REPEAT_SEQ_HEADER");
    set_ctrl(e, V4L2_CID_MPEG_VIDEO_HEADER_MODE,
             V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME, "HEADER_MODE");
    /* pas d'IDR périodique : le plus grand intervalle accepté */
    if (xioctl(e->fd, VIDIOC_QUERYCTRL, &q) == 0 && q.maximum > 0)
        iperiod = q.maximum;
    set_ctrl(e, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, iperiod, "H264_I_PERIOD");
    set_ctrl(e, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, "B_FRAMES");

    g_message("encodeur %s : profil %s, level %s, I_PERIOD %d", e->dev,
              p >= 0 ? profile_names[p] : "défaut",
              l >= 0 ? level_names[l] : "défaut", iperiod);
}

static gboolean map_buffers(struct SremfbEncoder *e, uint32_t type,
                            struct enc_buf *bufs, int n)
{
    for (int i = 0; i < n; i++) {
        struct v4l2_plane pl[VIDEO_MAX_PLANES];
        struct v4l2_buffer b = {
            .type = type, .memory = V4L2_MEMORY_MMAP, .index = (uint32_t)i,
            .m.planes = pl, .length = VIDEO_MAX_PLANES,
        };
        memset(pl, 0, sizeof(pl));
        if (xioctl(e->fd, VIDIOC_QUERYBUF, &b) < 0 || b.length < 1) {
            g_warning("encodeur %s : QUERYBUF : %s", e->dev, g_strerror(errno));
            return FALSE;
        }
        void *m = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                       e->fd, pl[0].m.mem_offset);
        if (m == MAP_FAILED) {
            g_warning("encodeur %s : mmap : %s", e->dev, g_strerror(errno));
            return FALSE;
        }
        bufs[i].map = m;
        bufs[i].len = pl[0].length;
        bufs[i].queued = FALSE;
    }
    return TRUE;
}

static int reqbufs(struct SremfbEncoder *e, uint32_t type, int count)
{
    struct v4l2_requestbuffers rb = {
        .count = (uint32_t)count, .type = type, .memory = V4L2_MEMORY_MMAP,
    };

    if (xioctl(e->fd, VIDIOC_REQBUFS, &rb) < 0)
        return -1;
    return (int)rb.count;
}

static gboolean qbuf_cap(struct SremfbEncoder *e, int i)
{
    struct v4l2_plane pl[1];
    struct v4l2_buffer b = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP,
        .index = (uint32_t)i, .m.planes = pl, .length = 1,
    };

    memset(pl, 0, sizeof(pl));
    pl[0].length = (uint32_t)e->cap[i].len;
    if (xioctl(e->fd, VIDIOC_QBUF, &b) < 0) {
        g_warning("encodeur %s : QBUF CAPTURE %d : %s", e->dev, i,
                  g_strerror(errno));
        return FALSE;
    }
    e->cap[i].queued = TRUE;
    return TRUE;
}

/* Récupère les buffers OUTPUT consommés, sans bloquer. */
static void reclaim_out(struct SremfbEncoder *e)
{
    for (;;) {
        struct v4l2_plane pl[1];
        struct v4l2_buffer b = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
            .memory = V4L2_MEMORY_MMAP, .m.planes = pl, .length = 1,
        };
        memset(pl, 0, sizeof(pl));
        if (xioctl(e->fd, VIDIOC_DQBUF, &b) < 0)
            return;                    /* EAGAIN : rien de prêt */
        if (b.index < (uint32_t)e->n_out)
            e->out[b.index].queued = FALSE;
    }
}

static void stream(struct SremfbEncoder *e, uint32_t type, gboolean on,
                   gboolean *state)
{
    int t = (int)type;

    if (*state == on)
        return;
    if (xioctl(e->fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &t) < 0) {
        g_warning("encodeur %s : %s %s : %s", e->dev,
                  on ? "STREAMON" : "STREAMOFF",
                  type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? "OUTPUT"
                                                             : "CAPTURE",
                  g_strerror(errno));
        if (on)
            return;
    }
    *state = on;
}

void sremfb_enc_close(struct SremfbEncoder *e)
{
    if (!e)
        return;
    if (e->fd >= 0) {
        stream(e, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, FALSE, &e->on_out);
        stream(e, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, FALSE, &e->on_cap);
    }
    for (int i = 0; i < e->n_out; i++)
        if (e->out[i].map)
            munmap(e->out[i].map, e->out[i].len);
    for (int i = 0; i < e->n_cap; i++)
        if (e->cap[i].map)
            munmap(e->cap[i].map, e->cap[i].len);
    if (e->fd >= 0) {
        if (e->n_out)
            reqbufs(e, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0);
        if (e->n_cap)
            reqbufs(e, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0);
        close(e->fd);
    }
    if (e->frames)
        g_message("encodeur %s fermé après %" G_GUINT64_FORMAT " frames",
                  e->dev, e->frames);
    g_free(e->agg);
    g_free(e->dev);
    g_free(e);
}

struct SremfbEncoder *sremfb_enc_open(int width, int height, int kbps)
{
    struct SremfbEncoder *e = g_new0(struct SremfbEncoder, 1);
    const char *dev = getenv("SREMFB_KVM_ENCODER");
    struct v4l2_capability cap;
    struct v4l2_format f;
    uint32_t caps, want_cap;
    int n;

    e->fd = -1;
    e->held = -1;
    e->dev = g_strdup(dev && *dev ? dev : ENC_DEFAULT_DEVICE);
    e->w = width;
    e->h = height;
    e->kbps = clamp_kbps(kbps);

    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
        g_warning("encodeur %s : %dx%d impossible en 4:2:0 (dimensions "
                  "paires requises)", e->dev, width, height);
        goto fail;
    }
    e->fd = open(e->dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (e->fd < 0) {
        g_warning("encodeur %s : %s%s — H.264 indisponible, RAW/LZ4 seul",
                  e->dev, g_strerror(errno), errno == ENOENT ?
                  " (module bcm2835-codec ? gpu_mem ?)" : "");
        goto fail;
    }
    memset(&cap, 0, sizeof(cap));
    if (xioctl(e->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        g_warning("encodeur %s : QUERYCAP : %s (pas un device V4L2 ?)", e->dev,
                  g_strerror(errno));
        goto fail;
    }
    caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                     : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_M2M_MPLANE) || !(caps & V4L2_CAP_STREAMING)) {
        g_warning("encodeur %s (%s) : pas un M2M multiplan (caps 0x%08x)",
                  e->dev, cap.card, caps);
        goto fail;
    }
    if (!has_format(e->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                    V4L2_PIX_FMT_H264) ||
        !has_format(e->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                    V4L2_PIX_FMT_YUV420)) {
        g_warning("encodeur %s (%s) : pas d'encodage YUV420 -> H264 "
                  "(mauvais nœud ? l'encodeur bcm2835 est /dev/video11)",
                  e->dev, cap.card);
        goto fail;
    }

    /* CAPTURE d'abord (spec stateful encoder) ; on demande des buffers plus
     * grands que les 768 Kio par défaut du driver en 1080p : une IDR d'un
     * écran chargé à 25 Mb/s peut les dépasser */
    want_cap = MAX(ENC_CAP_MIN_SIZE, (uint32_t)width * (uint32_t)height);
    want_cap = (want_cap + 4095) & ~4095u;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.width = (uint32_t)width;
    f.fmt.pix_mp.height = (uint32_t)height;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = want_cap;
    if (xioctl(e->fd, VIDIOC_S_FMT, &f) < 0) {
        g_warning("encodeur %s : S_FMT CAPTURE H264 %dx%d : %s", e->dev, width,
                  height, g_strerror(errno));
        goto fail;
    }
    if (f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_H264 ||
        f.fmt.pix_mp.width != (uint32_t)width ||
        f.fmt.pix_mp.height != (uint32_t)height) {
        g_warning("encodeur %s : %dx%d refusé (le driver propose %ux%u)",
                  e->dev, width, height, f.fmt.pix_mp.width,
                  f.fmt.pix_mp.height);
        goto fail;
    }

    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.width = (uint32_t)width;
    f.fmt.pix_mp.height = (uint32_t)height;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.colorspace = V4L2_COLORSPACE_SMPTE170M;
    f.fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_601;
    f.fmt.pix_mp.quantization = V4L2_QUANTIZATION_LIM_RANGE;
    f.fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_709;
    if (xioctl(e->fd, VIDIOC_S_FMT, &f) < 0) {
        g_warning("encodeur %s : S_FMT OUTPUT YUV420 %dx%d : %s", e->dev, width,
                  height, g_strerror(errno));
        goto fail;
    }
    e->bpl = f.fmt.pix_mp.plane_fmt[0].bytesperline;
    e->plane_h = f.fmt.pix_mp.height;
    e->out_size = f.fmt.pix_mp.plane_fmt[0].sizeimage;
    /* un seul plan mémoire : Y (bpl x plane_h) puis U et V (bpl/2 x
     * plane_h/2), c'est ainsi que le driver dimensionne sizeimage */
    if (f.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_YUV420 ||
        f.fmt.pix_mp.num_planes != 1 ||
        f.fmt.pix_mp.width != (uint32_t)width ||
        e->plane_h < (uint32_t)height || (e->plane_h & 1) ||
        e->bpl < (uint32_t)width || (e->bpl & 1) ||
        (size_t)e->out_size < (size_t)e->bpl * e->plane_h * 3 / 2) {
        g_warning("encodeur %s : géométrie OUTPUT inattendue (%ux%u, %u plan, "
                  "stride %u, sizeimage %u)", e->dev, f.fmt.pix_mp.width,
                  f.fmt.pix_mp.height, f.fmt.pix_mp.num_planes, e->bpl,
                  e->out_size);
        goto fail;
    }

    {
        struct v4l2_streamparm sp = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
        sp.parm.output.timeperframe.numerator = 1;
        sp.parm.output.timeperframe.denominator = ENC_FPS;
        if (xioctl(e->fd, VIDIOC_S_PARM, &sp) < 0)
            g_warning("encodeur %s : S_PARM %d i/s : %s", e->dev, ENC_FPS,
                      g_strerror(errno));
    }

    n = reqbufs(e, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, ENC_CAP_NBUF);
    if (n < 1 || n > ENC_CAP_MAX) {
        g_warning("encodeur %s : REQBUFS CAPTURE : %s", e->dev,
                  n < 0 ? g_strerror(errno) : "nombre de buffers inattendu");
        goto fail;
    }
    e->n_cap = n;
    n = reqbufs(e, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, ENC_OUT_NBUF);
    if (n < 1) {
        g_warning("encodeur %s : REQBUFS OUTPUT : %s", e->dev,
                  n < 0 ? g_strerror(errno) : "aucun buffer");
        goto fail;
    }
    e->n_out = MIN(n, ENC_OUT_NBUF);   /* les surnuméraires restent inutilisés */

    set_controls(e);

    if (!map_buffers(e, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, e->cap, e->n_cap) ||
        !map_buffers(e, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, e->out, e->n_out))
        goto fail;
    for (int i = 0; i < e->n_out; i++)
        if (e->out[i].len < e->out_size) {
            g_warning("encodeur %s : buffer OUTPUT %zu < sizeimage %u", e->dev,
                      e->out[i].len, e->out_size);
            goto fail;
        }
    for (int i = 0; i < e->n_cap; i++)
        if (!qbuf_cap(e, i))
            goto fail;

    stream(e, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, TRUE, &e->on_out);
    stream(e, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, TRUE, &e->on_cap);
    if (!e->on_out || !e->on_cap)
        goto fail;

    g_message("encodeur %s (%s) : %dx%d, stride %u x %u lignes, %d+%d "
              "buffers, CAPTURE %zu octets, %d kb/s CBR", e->dev, cap.card,
              width, height, e->bpl, e->plane_h, e->n_out, e->n_cap,
              e->cap[0].len, e->kbps);
    return e;

fail:
    sremfb_enc_close(e);
    return NULL;
}

void sremfb_enc_set_bitrate(struct SremfbEncoder *e, int kbps)
{
    if (!e)
        return;
    kbps = clamp_kbps(kbps);
    if (kbps == e->kbps)
        return;
    if (set_ctrl(e, V4L2_CID_MPEG_VIDEO_BITRATE, kbps * 1000, "BITRATE"))
        e->kbps = kbps;
}

int sremfb_enc_bitrate(const struct SremfbEncoder *e)
{
    return e ? e->kbps : 0;
}

/* Copie un plan (w x h octets utiles) au stride du driver. */
static void copy_plane(uint8_t *dst, size_t dst_stride, const uint8_t *src,
                       size_t w, size_t h)
{
    if (dst_stride == w) {
        memcpy(dst, src, w * h);
        return;
    }
    for (size_t row = 0; row < h; row++)
        memcpy(dst + row * dst_stride, src + row * w, w);
}

/* Parcourt les NAL Annex B : *vcl si une tranche est présente, *idr si une
 * tranche IDR (type 5) l'est. */
static void scan_nals(const uint8_t *p, size_t n, gboolean *vcl, gboolean *idr)
{
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1)
            continue;
        unsigned t = p[i + 3] & 0x1f;
        if (t == 5)
            *idr = TRUE;
        if (t >= 1 && t <= 5) {
            *vcl = TRUE;
            return;                    /* le reste de l'AU est de la tranche */
        }
        i += 3;
    }
}

/* Le buffer ne contient que des en-têtes : SEI, SPS, PPS, AUD (types 6
 * à 9), en Annex B dès le premier octet. */
static gboolean only_param_sets(const uint8_t *p, size_t n)
{
    gboolean seen = FALSE;

    if (n < 4 || p[0] != 0 || p[1] != 0)
        return FALSE;
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1)
            continue;
        unsigned t = p[i + 3] & 0x1f;
        if (t < 6 || t > 9)
            return FALSE;
        seen = TRUE;
        i += 3;
    }
    return seen;
}

static void agg_append(struct SremfbEncoder *e, const uint8_t *p, size_t n)
{
    if (e->agg_len + n > e->agg_size) {
        size_t sz = MAX(e->agg_size * 2, e->agg_len + n);
        e->agg = g_realloc(e->agg, sz);
        e->agg_size = sz;
    }
    memcpy(e->agg + e->agg_len, p, n);
    e->agg_len += n;
}

/* Encode une frame I420 complète (plans y/u/v de w x h, chroma w/2 x h/2).
 * Renvoie la taille de l'AU (<0 : erreur, le cœur repasse en RAW).
 * *nal_out pointe dans un buffer CAPTURE (ou notre tampon de recollage),
 * valable jusqu'à l'appel suivant. */
int sremfb_enc_encode(struct SremfbEncoder *e, uint8_t *y, uint8_t *u,
                      uint8_t *v, int force_idr, uint8_t **nal_out,
                      int *is_idr)
{
    size_t cw = (size_t)e->w / 2, ch = (size_t)e->h / 2;
    size_t uoff = (size_t)e->bpl * e->plane_h;
    size_t voff = uoff + (size_t)(e->bpl / 2) * (e->plane_h / 2);
    int timeout = e->frames ? ENC_TIMEOUT_MS : ENC_TIMEOUT_FIRST_MS;
    gint64 deadline;
    gboolean got_idr = FALSE, got_vcl = FALSE, done = FALSE;
    int last = -1, pieces = 0;
    const uint8_t *last_p = NULL;
    size_t last_len = 0;
    int bi = -1;

    /* l'AU précédente est partie : son buffer retourne au driver */
    if (e->held >= 0) {
        int h = e->held;
        e->held = -1;
        if (!qbuf_cap(e, h))
            return -1;
    }

    reclaim_out(e);
    for (int i = 0; i < e->n_out && bi < 0; i++)
        if (!e->out[i].queued)
            bi = i;
    if (bi < 0) {
        /* les deux en vol : seulement après un timeout, le firmware est
         * perdu */
        g_warning("encodeur %s : aucun buffer OUTPUT libre", e->dev);
        return -1;
    }

    uint8_t *dst = e->out[bi].map;
    copy_plane(dst, e->bpl, y, (size_t)e->w, (size_t)e->h);
    copy_plane(dst + uoff, e->bpl / 2, u, cw, ch);
    copy_plane(dst + voff, e->bpl / 2, v, cw, ch);

    /* (aussi sur la 1re frame : inoffensif, et ne suppose rien du firmware) */
    if (force_idr &&
        !set_ctrl(e, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1, "FORCE_KEY_FRAME"))
        return -1;

    e->seq += G_USEC_PER_SEC / ENC_FPS;
    {
        struct v4l2_plane pl[1];
        struct v4l2_buffer b = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
            .memory = V4L2_MEMORY_MMAP, .index = (uint32_t)bi,
            .m.planes = pl, .length = 1, .field = V4L2_FIELD_NONE,
        };
        memset(pl, 0, sizeof(pl));
        pl[0].bytesused = e->out_size;
        pl[0].length = (uint32_t)e->out[bi].len;
        b.timestamp.tv_sec = (time_t)(e->seq / G_USEC_PER_SEC);
        b.timestamp.tv_usec = (suseconds_t)(e->seq % G_USEC_PER_SEC);
        if (xioctl(e->fd, VIDIOC_QBUF, &b) < 0) {
            g_warning("encodeur %s : QBUF OUTPUT : %s", e->dev,
                      g_strerror(errno));
            return -1;
        }
        e->out[bi].queued = TRUE;
    }

    /* l'AU : un buffer CAPTURE en général ; plusieurs (même timestamp)
     * si elle déborde, ou si le firmware sort les en-têtes à part */
    e->agg_len = 0;
    deadline = g_get_monotonic_time() + (gint64)timeout * 1000;
    while (!done) {
        struct pollfd pfd = { .fd = e->fd, .events = POLLIN };
        gint64 left = (deadline - g_get_monotonic_time()) / 1000;
        struct v4l2_plane pl[1];
        struct v4l2_buffer b = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory = V4L2_MEMORY_MMAP, .m.planes = pl, .length = 1,
        };

        if (left <= 0 && got_vcl) {
            break;                     /* AU finie pile au bord d'un buffer */
        }
        if (left <= 0) {
            g_warning("encodeur %s : pas d'AU après %d ms (frame %" G_GUINT64_FORMAT
                      ")", e->dev, timeout, e->frames);
            goto err;
        }
        int r = poll(&pfd, 1, (int)left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            g_warning("encodeur %s : poll : %s", e->dev, g_strerror(errno));
            goto err;
        }
        if (r == 0)
            continue;                  /* le délai est revérifié en tête */
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            g_warning("encodeur %s : poll : erreur sur le device (0x%x)",
                      e->dev, pfd.revents);
            goto err;
        }
        if (!(pfd.revents & POLLIN))
            continue;
        memset(pl, 0, sizeof(pl));
        if (xioctl(e->fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN)
                continue;
            g_warning("encodeur %s : DQBUF CAPTURE : %s", e->dev,
                      g_strerror(errno));
            goto err;
        }
        if (b.index >= (uint32_t)e->n_cap)
            goto err;
        e->cap[b.index].queued = FALSE;
        guint64 ts = (guint64)b.timestamp.tv_sec * G_USEC_PER_SEC +
                     (guint64)b.timestamp.tv_usec;
        size_t off = MIN(pl[0].data_offset, pl[0].bytesused);
        size_t len = pl[0].bytesused - off;
        const uint8_t *p = e->cap[b.index].map + off;

        if (b.flags & V4L2_BUF_FLAG_ERROR) {
            g_warning("encodeur %s : buffer CAPTURE en erreur", e->dev);
            qbuf_cap(e, (int)b.index);
            goto err;
        }
        if (b.flags & V4L2_BUF_FLAG_LAST) {
            g_warning("encodeur %s : fin de flux inattendue", e->dev);
            qbuf_cap(e, (int)b.index);
            goto err;
        }
        gboolean pvcl = FALSE, pidr = FALSE;
        scan_nals(p, len, &pvcl, &pidr);
        /* buffer vide, ou d'une autre frame (le premier buffer du
         * firmware peut être du rebut à timestamp nul, cf. ustreamer) :
         * rendu au driver. Seule exception : des en-têtes seuls, que le
         * firmware pourrait sortir à part sans recopier le pts. */
        if (len == 0 ||
            (ts != e->seq && (got_vcl || !only_param_sets(p, len)))) {
            if (!qbuf_cap(e, (int)b.index))
                goto err;
            continue;
        }
        if (b.flags & V4L2_BUF_FLAG_KEYFRAME)
            got_idr = TRUE;
        got_idr = got_idr || pidr;
        got_vcl = got_vcl || pvcl;
        /* complète : une tranche vue et un buffer pas plein (un buffer
         * plein peut avoir une suite) */
        done = got_vcl && len < e->cap[b.index].len;

        if (done && pieces == 0) {
            last = (int)b.index;       /* cas courant : zéro copie */
            last_p = p;
            last_len = len;
        } else {
            if (pieces == 0 && !e->warned_frag) {
                g_message("encodeur %s : AU en plusieurs buffers CAPTURE "
                          "(%zu octets dans le premier), recollage", e->dev, len);
                e->warned_frag = TRUE;
            }
            agg_append(e, p, len);
            if (!qbuf_cap(e, (int)b.index))
                goto err;
        }
        pieces++;
        if (!done && got_vcl)          /* buffer plein : suite éventuelle */
            deadline = MIN(deadline, g_get_monotonic_time() + 50 * 1000);
    }

    reclaim_out(e);
    e->frames++;
    if (last >= 0)
        e->held = last;                /* rendu au driver à l'appel suivant */
    if (force_idr && !got_idr) {
        /* un épisode ne peut pas s'ouvrir sur une P : le décodeur la
         * prédirait d'une image périmée. RAW plutôt qu'une image fausse. */
        g_warning("encodeur %s : IDR demandée, frame non-IDR reçue", e->dev);
        return -1;
    }
    *is_idr = got_idr;
    if (last >= 0) {
        *nal_out = (uint8_t *)last_p;
        return (int)last_len;
    }
    *nal_out = e->agg;
    return (int)e->agg_len;

err:
    reclaim_out(e);
    return -1;
}
