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

/* One entry of the plug history (evdi.c, "plug gate"): every decision
 * about plugging — self-heal budget, backoff, hourly caps — is derived
 * from these timestamps, nothing else is remembered. */
typedef struct {
    gint64 t;                  /* g_get_monotonic_time() */
    uint8_t kind;              /* SREMFB_PLUG_EV_* (evdi.c) */
} SremfbPlugEvent;

/* Server-wide EVDI state, hung off SremfbServer.src. */
typedef struct {
    GPtrArray *devices;        /* SremfbEvdiDevice*, open for the whole
                                  process lifetime */
    gboolean wedge_seen;       /* a mode-timeout happened: distrust the
                                  pre-existing free devices */
    GArray *plug_events;       /* SremfbPlugEvent, chronological; the
                                  last hour plus the failures since the
                                  last connector that lit up */
    pid_t plug_shell;          /* gnome-shell the history refers to */
    int gate_logged;           /* last gate verdict written to the
                                  journal (log de-duplication only) */
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
    gint64 ps_seen;            /* last mode-timer tick that found mutter in
                                  power save (0 = none during this wait) */
    gboolean usb_early;        /* usb peer announced while parked */

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

/* Verdict of one tick of the 10 s mode timer (evdi.c, "power save"):
 * keep the connector plugged and wait (PARK), or give up on the
 * compositor (FAIL: quarantine, self-heal, SERVER_FAIL). power_save is
 * mutter's DisplayConfig.PowerSaveMode (0 on, 1-3 standby/suspend/off,
 * < 0 unknown); ps_seen when a tick last saw power save, 0 = never during
 * this wait. A session that just woke up gets SREMFB_PS_GRACE_US to
 * modeset before its silence counts as a failure again; if mutter cannot
 * even be read right after a power save (busy replaying its modesets),
 * the grace stretches to SREMFB_PS_UNKNOWN_GRACE_US. The grace counts
 * from the last tick that saw power save, so it is 10-20 s (resp.
 * 50-60 s) after the actual wake-up. */
enum { SREMFB_MODE_WAIT_FAIL = 0, SREMFB_MODE_WAIT_PARK };
#define SREMFB_PS_GRACE_US          ((gint64)15 * G_USEC_PER_SEC)
#define SREMFB_PS_UNKNOWN_GRACE_US  ((gint64)60 * G_USEC_PER_SEC)

static inline int sremfb_mode_wait_verdict(int power_save, gint64 now,
                                           gint64 ps_seen)
{
    if (power_save > 0)
        return SREMFB_MODE_WAIT_PARK;
    if (ps_seen > 0 && now - ps_seen < (power_save < 0
                                        ? SREMFB_PS_UNKNOWN_GRACE_US
                                        : SREMFB_PS_GRACE_US))
        return SREMFB_MODE_WAIT_PARK;
    return SREMFB_MODE_WAIT_FAIL;
}

/* The EVDI frame source (admit = plug gate, acquire = claim a device +
 * plug an EDID). */
extern const struct sremfb_source_ops sremfb_evdi_ops;

void     sremfb_evdi_reset(unsigned count);  /* best-effort fresh devices */
gboolean sremfb_evdi_probe(void);            /* any evdi device present? */
void     sremfb_evdi_close_all(SremfbServer *srv);

/* edid.c */
void sremfb_edid_build(uint8_t out[256], uint32_t width, uint32_t height,
                       uint32_t serial, const char model[13]);

#endif /* SREMFB_EVDI_H */
