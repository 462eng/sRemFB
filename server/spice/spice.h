#ifndef SREMFB_SPICE_H
#define SREMFB_SPICE_H

#include <spice-client.h>

#include "core.h"

typedef enum {
    SREMFB_RESIZE_AGENT = 0,   /* drive the guest resolution via vdagent */
    SREMFB_RESIZE_SCALE,       /* never touch the guest, always scale */
    SREMFB_RESIZE_OFF,         /* refuse (BAD_HELLO) on geometry mismatch */
} SremfbResizeMode;

typedef struct {
    char *host;                /* SREMFB_SPICE_HOST (required) */
    int   port;                /* SREMFB_SPICE_PORT (required) */
    char *password;            /* SREMFB_SPICE_PASSWORD */
    gboolean tls;              /* SREMFB_SPICE_TLS */
    char *ca_file;             /* SREMFB_SPICE_CA_FILE */
    int   display_id;          /* SREMFB_SPICE_DISPLAY (default 0) */
    SremfbResizeMode resize;   /* SREMFB_RESIZE */
} SremfbSpiceConfig;

/* Server-wide SPICE state, hung off SremfbServer.src. One SpiceSession =
 * one VM; every sremfb client mirrors the same canvas (fan-out). */
typedef struct {
    SremfbServer *srv;
    SremfbSpiceConfig cfg;

    SpiceSession       *session;
    SpiceMainChannel   *main;
    SpiceChannel       *display;   /* the configured display channel */

    /* primary surface snapshot (data owned by spice-glib, valid between
     * display-primary-create and display-primary-destroy) */
    gboolean have_primary;
    gboolean marked;               /* display-mark gate */
    int      fmt;                  /* SpiceSurfaceFmt */
    int      width, height, stride;
    const guint8 *data;

    guint    reconnect_id;         /* backoff timer */
    int      backoff_s;            /* current reconnect backoff (1..30) */
} SremfbSpiceState;

/* Per-client SPICE state, hung off SremfbClient.src_ctx. */
typedef struct {
    gboolean streaming;
    gboolean scale;                /* stream geometry != primary geometry */
} SremfbSpiceClient;

extern const struct sremfb_source_ops sremfb_spice_ops;

/* Build the session and start connecting (call before sremfb_serve). */
void sremfb_spice_start(SremfbServer *srv);

#endif /* SREMFB_SPICE_H */
