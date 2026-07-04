#ifndef SREMFB_SERVER_H
#define SREMFB_SERVER_H

#include <glib.h>
#include <evdi_lib.h>
#include <netinet/in.h>

#include "protocol.h"

typedef struct SremfbServer SremfbServer;
typedef struct SremfbClient SremfbClient;

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

typedef enum {
    SREMFB_CLIENT_MODE_WAIT = 0,  /* EDID plugged, waiting for the
                                     compositor to light up the connector */
    SREMFB_CLIENT_STREAMING,      /* mode set, damage-driven frames flowing */
} SremfbClientState;

/* One connected client = one TCP socket + one EVDI connector. The MAC
 * address sent in the hello identifies the physical client: it drives the
 * EDID serial, so GNOME remembers one layout position per client. */
struct SremfbClient {
    SremfbServer *srv;
    SremfbClientState state;

    int fd;
    guint watch_id;
    struct sremfb_client_hello hello;
    gboolean lz4;
    char peer[64];             /* "ip:port" for logs */
    char macstr[18];
    guint lost_id;             /* deferred sremfb_client_lost (g_idle) */

    /* evdi virtual connector */
    SremfbEvdiDevice *dev;     /* pooled device, NULL when none */
    gboolean plugged;          /* EDID connected (cable "plugged in") */
    uint8_t edid[128];
    struct evdi_mode mode;
    gboolean mode_valid;
    gboolean grab_registered;
    uint8_t *grabbuf;          /* BGRx buffer registered with evdi */
    gboolean update_pending;   /* update requested, update_ready will fire */
    guint kick_id;             /* deferred next update request (g_idle) */
    guint mode_timeout_id;     /* answers SERVER_FAIL if the compositor
                                  never enables the connector */

    /* wire buffers */
    uint8_t *rectbuf;          /* one converted rect, client pixel format */
    uint8_t *sendbuf;          /* sremfb_frame_hdr + payload for one rect */
    size_t rectbuf_size, sendbuf_size;

    /* periodic stats */
    guint64 st_grabs, st_rects, st_wire_bytes, st_raw_bytes;
    gint64 st_since_us;
};

/* IPv4 CIDR allowlist entry (network-byte-order address and mask). */
typedef struct {
    uint32_t addr;
    uint32_t mask;
} SremfbAllowNet;

struct SremfbServer {
    GMainLoop *loop;

    uint16_t port;
    int listen_fd;
    guint listen_watch_id;

    GPtrArray *clients;        /* SremfbClient* */
    GPtrArray *devices;        /* SremfbEvdiDevice*, open for the whole
                                  process lifetime */

    SremfbAllowNet *allow;     /* empty = allow everyone */
    unsigned n_allow;
};

#define SREMFB_MAX_CLIENTS 8

/* main.c */
void sremfb_client_lost(SremfbClient *c);
void sremfb_schedule_client_lost(SremfbClient *c);

/* net.c */
int      net_listen(uint16_t port);
int      net_accept_and_hello(SremfbServer *srv, int listen_fd,
                              struct sremfb_client_hello *hello,
                              char *peer, size_t peer_len);
gboolean net_send_all(int fd, const void *buf, size_t len);
gboolean net_send_server_hello(int fd, uint16_t width, uint16_t height,
                               uint8_t pixfmt, uint16_t status);
gboolean net_allow_parse(SremfbServer *srv, const char *spec);

/* edid.c */
void sremfb_edid_build(uint8_t out[128], uint32_t width, uint32_t height,
                       uint32_t serial, const char model[13]);

/* evdi.c */
void     sremfb_evdi_reset(unsigned count);  /* best-effort fresh devices */
gboolean sremfb_evdi_probe(void);            /* any evdi device present? */
gboolean sremfb_evdi_acquire(SremfbClient *c);
void     sremfb_evdi_release(SremfbClient *c);   /* back to the pool */
void     sremfb_evdi_close_all(SremfbServer *srv);
void     sremfb_evdi_plug(SremfbClient *c);
void     sremfb_evdi_unplug(SremfbClient *c);

/* convert.c */
void sremfb_convert_bgrx_row(uint8_t *dst, const uint8_t *src, uint32_t width,
                             uint8_t pixfmt, uint32_t x0, uint32_t y);

#endif /* SREMFB_SERVER_H */
