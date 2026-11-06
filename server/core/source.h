/* SPDX-License-Identifier: EUPL-1.2 */
#ifndef SREMFB_SOURCE_H
#define SREMFB_SOURCE_H

/*
 * Frame-source abstraction. The core (session, transmit, encode) is
 * independent of where pixels come from: an EVDI virtual monitor on a
 * GNOME/Wayland host, a SPICE display of a QEMU/KVM VM, ... Each backend
 * implements sremfb_source_ops and drives acquisition; damage and
 * lifecycle are pushed into the core through the plain core entry points
 * (sremfb_xmit_damage, the BLANK/UNBLANK control path, client_lost).
 */

typedef struct SremfbClient SremfbClient;
typedef struct SremfbServer SremfbServer;

/* Damage rectangle, [x1,x2) x [y1,y2). The one shape every source
 * reports damage in — mirrors struct evdi_rect field for field so the
 * EVDI backend hands its rects straight through. */
struct sremfb_rect {
    int x1, y1, x2, y2;
};

/* Stream geometry the core builds frames at (the client's resolution). */
struct sremfb_geom {
    int width, height;
};

/*
 * One frame source per client. Acquisition may complete asynchronously
 * (EVDI waits for the compositor to set a mode): the source transitions
 * the client to SREMFB_CLIENT_STREAMING and sends the server hello once
 * pixels actually flow.
 */
struct sremfb_source_ops {
    /* Begin acquisition for a freshly accepted client (hello parsed).
     * Returns SREMFB_STATUS_OK when acquisition has started, or a
     * SREMFB_STATUS_* on immediate failure (the core answers that status
     * and drops the client). Must clean up its own state on failure. */
    int  (*acquire)(SremfbClient *c);

    /* Optional, may be NULL. Asked before a new client is even logged:
     * SREMFB_STATUS_OK to go on to acquire(), or a SREMFB_STATUS_* the
     * core answers silently before dropping the connection. Lets a
     * source turn clients away cheaply — and without a journal line per
     * attempt — while it knows acquisition cannot work (EVDI: the
     * compositor stopped lighting connectors). The source logs its own
     * state changes. */
    int  (*admit)(SremfbServer *srv);

    /* Release everything tied to the client (teardown / replacement).
     * Safe to call whether or not acquire() succeeded. */
    void (*release)(SremfbClient *c);
};

#endif /* SREMFB_SOURCE_H */
