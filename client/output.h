/*
 * Pixel-sink abstraction for the client. The decode/convert path stays
 * source-agnostic and writes rows through these ops; each backend owns the
 * physical surface:
 *   - output_fb.c  : the legacy /dev/fb0 path (mmap or pwrite). Default.
 *   - output_drm.c : DRM/KMS via libdrm (opt-in, SREMFB_OUTPUT=drm). C1.2.
 *
 * Centering and clipping of the stream into the panel are done by the
 * caller (it knows the stream geometry); a backend only writes an already
 * clipped, panel-pixfmt row at a panel pixel position, and publishes the
 * frame with present() (a no-op for fb, a page-flip for drm).
 */
#ifndef SREMFB_OUTPUT_H
#define SREMFB_OUTPUT_H

#include <stdint.h>

struct sremfb_output;

struct sremfb_output_ops {
    const char *name;

    /* Open the sink. Fills the panel geometry, the wire pixel format it
     * wants (SREMFB_PIX_*, announced in the client hello) and its bytes
     * per pixel. Returns 0, or -1 on failure. */
    int  (*open)(struct sremfb_output *o, unsigned *w, unsigned *h,
                 uint8_t *pixfmt, unsigned *bytespp);

    /* Write `npix` pixels (already in the panel pixfmt) at panel position
     * (dx,dy) into the current back buffer. The caller has clipped it to
     * [0,w) x [0,h). */
    void (*write_row)(struct sremfb_output *o, unsigned dx, unsigned dy,
                      const uint8_t *src, unsigned npix);

    /* Publish what was written since the last present (drm: page-flip;
     * fb: no-op — writes are already visible). Called after a full frame
     * for H.264, and may be called per damage batch for RAW. */
    void (*present)(struct sremfb_output *o);

    void (*clear)(struct sremfb_output *o);            /* paint black */

    /* Panel power: blanked with no signal and on a forwarded DPMS off. */
    void (*blank)(struct sremfb_output *o, int on);

    /* Panel connected? 1 = yes, 0 = unplugged, -1 = no watcher/unknown.
     * Drives the hotplug teardown (panel cable pulled). */
    int  (*panel_present)(struct sremfb_output *o);

    /* Take/hand back the console: VT to graphics + foreground (fb and
     * drm), plus DRM master for drm. */
    void (*grab)(struct sremfb_output *o);
    void (*ungrab)(struct sremfb_output *o);

    void (*close)(struct sremfb_output *o);
};

struct sremfb_output {
    const struct sremfb_output_ops *ops;
    void *priv;                 /* backend-private state */
    unsigned w, h;              /* panel geometry (filled by open) */
    uint8_t  pixfmt;            /* SREMFB_PIX_* */
    unsigned bytespp;
};

/* Selected by SREMFB_OUTPUT (fb default; drm opt-in, forced if multi-head). */
extern const struct sremfb_output_ops sremfb_output_fb;
extern const struct sremfb_output_ops sremfb_output_drm;

#endif /* SREMFB_OUTPUT_H */
