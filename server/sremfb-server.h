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

/* Transmit path (per client), driven by the pressure controller. */
typedef enum {
    SREMFB_XMIT_RAW = 0,          /* damage rects, RAW/LZ4 (the v2 path) */
    SREMFB_XMIT_H264,             /* full-frame H.264 access units */
} SremfbXmitMode;

/* Pressure controller states (control.c). */
typedef enum {
    SREMFB_CTL_RAW_STEADY = 0,
    SREMFB_CTL_RAW_SUSPECT,       /* pressure seen, waiting for confirmation */
    SREMFB_CTL_H264_ACTIVE,
    SREMFB_CTL_H264_COOLDOWN,     /* calm; waiting before going back to RAW */
} SremfbCtlState;

struct SremfbEncoder;             /* encoder.c, keeps x264.h out of here */

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

    /* negotiated feature bits (v2 + hello flags) */
    gboolean feedback;         /* client echoes PING as PONG */
    gboolean h264_cap;         /* client decodes H.264 */

    /* upstream (PONG) reassembly */
    uint8_t recvbuf[64];
    size_t recvlen;
    unsigned recv_garbage;     /* bytes skipped hunting for the magic */

    /* pressure controller (control.c) */
    SremfbXmitMode xmit_mode;
    SremfbCtlState ctl_state;
    gint64 ctl_since_us;       /* entered current state */
    gint64 ctl_calm_us;        /* start of the current calm streak (0 = none) */
    guint ping_timer_id;
    gint64 last_ping_us;       /* when the last ping left */
    gint64 last_pong_us;       /* when the last echo came back (watchdog) */
    unsigned pings_outstanding;
    guint64 ping_wire_mark;    /* st lifetime wire bytes at last ping */
    guint64 wire_total;        /* lifetime wire bytes (never reset) */
    double delay_ewma_us;      /* echo delay, the pressure signal */
    double sendstall_ewma_us;  /* time blocked inside net_send_all */
    double capacity_Bps;       /* wire rate observed while saturated */
    double lz4_ratio_ewma;     /* raw/wire, learned in RAW mode */
    double raw_rate_Bps;       /* damage byte rate (pre-compression) */
    double grab_fps;           /* delivered frame rate (sends block) */
    gint64 last_grab_us;
    gint64 damage_cont_us;     /* start of the current continuous-damage run */
    gint64 cap_win_start_us;   /* rate sampling window */
    guint64 cap_win_bytes;     /* wire bytes in the window */
    guint64 raw_win_bytes;     /* pre-compression bytes in the window */

    /* H.264 encoder (encoder.c) */
    struct SremfbEncoder *enc;
    uint8_t *yuvbuf;           /* I420: w*h luma + 2 quarter chroma planes */
    gboolean h264_failed;      /* encoder init failed: RAW forever */
    gboolean force_idr;        /* next encode opens an episode */
    gint64 last_enc_us;        /* paces the encode rate under pressure */

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

    unsigned selfheal_left;    /* fresh-device additions still allowed */
    gboolean wedge_seen;       /* a mode-timeout happened: distrust the
                                  pre-existing free devices */
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
                               uint8_t pixfmt, uint16_t status,
                               uint8_t flags);
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
void sremfb_convert_bgrx_to_i420(uint8_t *yp, uint8_t *up, uint8_t *vp,
                                 const uint8_t *bgrx, uint32_t w, uint32_t h);

/* encoder.c */
struct SremfbEncoder *sremfb_enc_open(int width, int height, int kbps);
void sremfb_enc_set_bitrate(struct SremfbEncoder *e, int kbps);
int  sremfb_enc_bitrate(const struct SremfbEncoder *e);
int  sremfb_enc_encode(struct SremfbEncoder *e, uint8_t *y, uint8_t *u,
                       uint8_t *v, int force_idr, uint8_t **nal_out,
                       int *is_idr);
void sremfb_enc_close(struct SremfbEncoder *e);

/* control.c — pressure measurement + adaptive-mode decisions */
void        sremfb_ctl_start(SremfbClient *c);    /* at STREAMING entry */
void        sremfb_ctl_stop(SremfbClient *c);     /* at teardown */
void        sremfb_ctl_on_pong(SremfbClient *c, uint64_t t_echo_us);
void        sremfb_ctl_on_grab(SremfbClient *c, size_t raw_bytes,
                               size_t wire_bytes);
int         sremfb_ctl_initial_kbps(const SremfbClient *c);
gboolean    sremfb_ctl_skip_encode(SremfbClient *c);
const char *sremfb_ctl_state_name(const SremfbClient *c);

/* evdi.c — mode switching needs these */
void sremfb_xmit_set_mode(SremfbClient *c, SremfbXmitMode mode);

#endif /* SREMFB_SERVER_H */
