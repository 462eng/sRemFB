/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * sremfb-kvm — backend de capture HDMI (TC358743 sur Raspberry Pi 4).
 *
 * Contrat entre les lots du projet IPKVM (SPEC-kvm-backend.md §4.1) :
 * capture.c / diff.c / convert_uyvy.c / testsrc.c (lot A), input_hid.c +
 * hidmap.c (lot B, entrées vers le gadget USB), encoder_v4l2.c (lot C,
 * H.264 matériel derrière l'API sremfb_enc_* de core.h). Les noms, champs
 * et signatures du §4.1 sont figés ; ce qui est ajouté au-delà est marqué
 * « ajout lot A ».
 */
#ifndef SREMFB_KVM_H
#define SREMFB_KVM_H

#include <stdint.h>
#include <stddef.h>

#include "core.h"

/* Buffers V4L2 de capture (§4.3.2) : 1 retenu comme frame précédente,
 * 3 dans le driver. Sert aussi aux buffers internes de testsrc.c. */
#define SREMFB_KVM_NBUF 4

/* Géométrie et placement d'une image source dans un flux client
 * (letterbox, échelle uniforme, centré). s == 1 et ox == oy == 0 quand les
 * géométries coïncident. Partagé par capture.c (blit) et input_hid.c
 * (projection du pointeur absolu). */
struct sremfb_kvm_fit {
    int src_w, src_h;          /* image capturée */
    int dst_w, dst_h;          /* flux (c->geom) */
    double s;                  /* échelle source -> flux */
    int ox, oy, dw, dh;        /* rectangle utile dans le flux */
};
void sremfb_kvm_fit(struct sremfb_kvm_fit *f, int sw, int sh, int dw, int dh);

struct sremfb_kvm_testsrc;     /* testsrc.c */

/* Source de frames : état serveur (SremfbServer.src). */
typedef struct {
    SremfbServer *srv;
    /* config */
    const char *device;        /* SREMFB_KVM_DEVICE, /dev/video0 */
    const char *edid_path;     /* SREMFB_KVM_EDID */
    uint32_t    pixfmt;        /* V4L2_PIX_FMT_UYVY | RGB24 (SREMFB_KVM_FORMAT) */
    int         tile;          /* SREMFB_KVM_TILE, 32 */
    enum { SREMFB_KVM_GEOM_SOURCE, SREMFB_KVM_GEOM_CLIENT } geometry;
    enum { SREMFB_KVM_RESIZE_SCALE, SREMFB_KVM_RESIZE_RECONNECT } resize;
    /* capture */
    int fd;                    /* -1 : pas de device (mode test) */
    gboolean streaming;        /* STREAMON actif */
    gboolean signal;           /* timings valides, frames attendues */
    int src_w, src_h;          /* mode courant de la source (0 sans signal) */
    unsigned src_fps;          /* informatif (timings) */
    uint32_t src_fourcc;       /* format réellement négocié */
    size_t   src_stride;
    /* buffers V4L2 (mmap), dont un retenu comme "frame précédente" */
    struct sremfb_kvm_buf { void *map; size_t len; gboolean queued; } buf[SREMFB_KVM_NBUF];
    int prev;                  /* index de la frame précédente (-1 : aucune) */
    /* image maître BGRx en géométrie source, remplie tuile par tuile */
    uint8_t *master;
    uint8_t *tile_dirty;       /* diff.c : 1 octet par tuile */
    int tiles_x, tiles_y;
    guint fd_watch_id, poll_id;
    /* source synthétique (testsrc.c) quand fd < 0 */
    struct sremfb_kvm_testsrc *test;
    /* stats */
    guint64 frames, frames_dirty, tiles_dirty_total;

    /* --- ajout lot A --- */
    int nbuf;                  /* buffers réellement obtenus (REQBUFS) */
    size_t src_size;           /* sizeimage négocié */
    gboolean full_range;       /* quantization FULL_RANGE annoncée par
                                  G_FMT : coefficients pleine échelle */
    guint frame_watch_id;      /* G_IO_IN (frames), seulement en streaming ;
                                  fd_watch_id est le G_IO_PRI (événements) */
    guint debounce_id;         /* probe_signal() différé (rafales) */
    guint stats_id;            /* journal périodique §4.11 */
    gint64 last_restream_us;   /* débounce des changements de mode */
    gboolean lost_logged;      /* « pas de signal » déjà journalisé */
} SremfbKvmState;

/* Par client (SremfbClient.src_ctx). */
typedef struct {
    gboolean streaming;
    struct sremfb_kvm_fit fit; /* recalculé à chaque changement de mode */

    /* --- ajout lot A --- */
    gboolean repaint;          /* plein cadre + UNBLANK à la prochaine frame
                                  (après un changement de mode) */
    gint64 st_since_us;        /* bases des statistiques périodiques */
    guint64 st_frames, st_frames_dirty, st_tiles;
} SremfbKvmClient;

extern const struct sremfb_source_ops sremfb_kvm_ops;
int  sremfb_kvm_start(SremfbServer *srv);     /* ouvre, EDID, 1ère détection ; 0 = OK */
void sremfb_kvm_stop(SremfbServer *srv);

/* diff.c : compare cur et prev (même format/stride), marque st->tile_dirty.
 * Renvoie le nombre de tuiles sales. prev == NULL : tout sale. */
int  sremfb_kvm_diff(SremfbKvmState *st, const uint8_t *cur, const uint8_t *prev);
/* diff.c : transforme tile_dirty en rectangles (coordonnées source),
 * au plus `max` (fusion par lignes puis bounding box). */
int  sremfb_kvm_rects(const SremfbKvmState *st, struct sremfb_rect *out, int max);

/* convert_uyvy.c : convertit la zone [x1,x2)x[y1,y2) (multiples de 2 en x)
 * de la frame capturée vers st->master (BGRx). */
void sremfb_kvm_convert_rect(const SremfbKvmState *st, const uint8_t *src,
                             const struct sremfb_rect *r);

/* input_hid.c : projection d'un pointeur absolu en pixels du flux vers
 * 0..32767 sur l'écran de la cible, en inversant le letterbox. */
void sremfb_kvm_abs_map(const struct sremfb_kvm_fit *f, int x, int y,
                        int *hx, int *hy);

/* --- ajout lot A : interfaces internes capture.c <-> testsrc.c / main --- */

/* diff.c : (ré)alloue la grille de tuiles pour src_w x src_h. */
void sremfb_kvm_tiles_alloc(SremfbKvmState *st);
/* Octets par pixel du format capturé (UYVY 2, RGB24 3). */
unsigned sremfb_kvm_src_bpp(uint32_t fourcc);

/* capture.c : une frame est prête dans st->buf[idx] (chemin commun V4L2
 * et source synthétique : diff -> convert -> fan-out). */
void sremfb_kvm_frame(SremfbKvmState *st, int idx);
/* capture.c : la source a peut-être changé (V4L2_EVENT_SOURCE_CHANGE ou
 * équivalent simulé) : probe_signal() débouncé. */
void sremfb_kvm_source_changed(SremfbKvmState *st);
/* capture.c : --probe (§4.11) ; 0 = tout est là. */
int  sremfb_kvm_probe(SremfbKvmState *st, const char *encoder,
                      const char *const hid[3]);

/* testsrc.c — SREMFB_KVM_DEVICE=test:WxH[@fps] | file:chemin@WxH[@fps] */
struct sremfb_kvm_testsrc *sremfb_kvm_test_new(SremfbKvmState *st,
                                               const char *spec);
void     sremfb_kvm_test_free(struct sremfb_kvm_testsrc *t);
/* signal présent ? mode courant dans *w, *h, *fps */
gboolean sremfb_kvm_test_query(struct sremfb_kvm_testsrc *t, int *w, int *h,
                               unsigned *fps);
/* buffers UYVY dans st->buf[], timer à la cadence ; FALSE = échec */
gboolean sremfb_kvm_test_start(struct sremfb_kvm_testsrc *t);
void     sremfb_kvm_test_stop(struct sremfb_kvm_testsrc *t);

#endif /* SREMFB_KVM_H */
