/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef SREMFB_EVDI_H
#define SREMFB_EVDI_H

#include <evdi_lib.h>

#include "core.h"

/* One EVDI DRM device. Opened on demand but then kept open (and flocked)
 * for the whole process lifetime: mutter wedges (EBUSY on reopen, then
 * ignores hotplugs on that card) when the device is closed and reopened
 * between plugs. Freed clients just leave their device back in the pool. */
typedef struct {
    evdi_handle handle;
    int card;                  /* /dev/dri/card<card> */
    int lock_fd;               /* flock: arbitration across processes */
    guint watch_id;
    SremfbClient *owner;       /* NULL = free */
    gboolean suspect;          /* mode timeout seen: mutter probably wedged
                                  on this card (EBUSY reopen bug) — only
                                  reused as a last resort */
} SremfbEvdiDevice;

/* Server-wide EVDI state, hung off SremfbServer.src. */
typedef struct {
    GPtrArray *devices;        /* SremfbEvdiDevice*, open for the whole
                                  process lifetime */
    unsigned selfheal_left;    /* fresh-device additions still allowed */
    gboolean wedge_seen;       /* a mode-timeout happened: distrust the
                                  pre-existing free devices */
} SremfbEvdiState;

/* Per-client EVDI state, hung off SremfbClient.src_ctx. */
typedef struct {
    SremfbEvdiDevice *dev;     /* pooled device, NULL when none */
    gboolean plugged;          /* EDID connected (cable "plugged in") */
    uint8_t edid[256];
    gboolean mode_valid;
    gboolean grab_registered;
    gboolean update_pending;   /* update requested, update_ready will fire */
    guint kick_id;             /* deferred next update request (g_idle) */
    guint mode_timeout_id;     /* answers SERVER_FAIL if the compositor
                                  never enables the connector */

    /* Cursor, composed here rather than by the kernel (cursor events on,
     * see evdi.c): the plane's image and position, and the grabbuf
     * pixels it currently covers so it can be lifted off again. */
    gboolean cur_on;           /* plane enabled with a usable image */
    int cur_x, cur_y;          /* plane top-left, may be negative */
    int cur_w, cur_h;
    int cur_stride;            /* in pixels */
    uint32_t *cur_pix;         /* ARGB8888, premultiplied (libevdi malloc) */
    gboolean cur_drawn;        /* blended into grabbuf at cur_rect */
    struct sremfb_rect cur_rect;
    uint32_t *cur_under;       /* grabbuf pixels under cur_rect */
    size_t cur_under_size;     /* in pixels */
} SremfbEvdiClient;

/* The EVDI frame source (acquire = claim a device + plug an EDID). */
extern const struct sremfb_source_ops sremfb_evdi_ops;

void     sremfb_evdi_reset(unsigned count);  /* best-effort fresh devices */
gboolean sremfb_evdi_probe(void);            /* any evdi device present? */
void     sremfb_evdi_close_all(SremfbServer *srv);

/* edid.c */
void sremfb_edid_build(uint8_t out[256], uint32_t width, uint32_t height,
                       uint32_t serial, const char model[13]);

#endif /* SREMFB_EVDI_H */
