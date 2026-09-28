/*
 * sremfb-view — shared state between the network thread (net.c) and the
 * SDL main thread (sremfb-view.c).
 *
 * The contract is a single "latest picture" mailbox, never a queue:
 *
 *   - The network thread owns the socket. It reads every message as soon
 *     as it arrives, decodes it (LZ4 today, JPEG tiles tomorrow) into a
 *     private scratch buffer, then — under fb.lock — copies the pixels
 *     into the shared framebuffer and appends the rect to the pending
 *     damage list. When it has caught up with the socket (or a batch has
 *     been open for a couple of ms) it publishes the batch and wakes the
 *     main thread with one coalesced SDL event.
 *
 *   - The main thread uploads only the pending damage to its streaming
 *     texture and presents. Whatever arrived meanwhile is simply merged
 *     into the next upload: the display always shows the newest pixels
 *     and a slow present can never back up the network.
 *
 * The framebuffer holds pixels in the stream's wire format (pixfmt), so
 * any future decoder only has to produce that format; the texture format
 * is derived from it in one place (view_sdl_format()).
 */
#ifndef SREMFB_VIEW_H
#define SREMFB_VIEW_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "protocol.h"

#define VIEW_DAMAGE_MAX 32          /* pending rects before collapsing to
                                       their bounding box */

struct view_rect {
    uint16_t x, y, w, h;
};

/* The shared picture. Everything below `lock` is protected by it. */
struct view_fb {
    pthread_mutex_t lock;

    unsigned gen;               /* bumped on every (re)connect: geometry or
                                   format may have changed, full upload */
    int connected;              /* a stream is up */
    int blanked;                /* server forwarded DPMS off */
    unsigned w, h;              /* stream geometry */
    uint8_t pixfmt;             /* enum sremfb_pixfmt */
    unsigned bytespp;
    size_t stride;              /* w * bytespp */
    uint8_t *pixels;            /* w * h * bytespp, or NULL */

    int input;                  /* the server confirmed INPUT: events
                                   may be sent on this connection */

    struct view_rect damage[VIEW_DAMAGE_MAX];
    int damage_n;
    int damage_all;             /* upload everything (overflow, new gen) */
    unsigned batches;           /* published batches, for the dump logic */

    /* --latency-test probe: while armed, the network thread timestamps the
     * first decoded rect that changes the pixel at (probe_x, probe_y) away
     * from probe_base — the input->pixels delay on the local clock */
    int probe_armed;
    unsigned probe_x, probe_y;
    uint32_t probe_base;
    uint64_t probe_hit_ns;      /* CLOCK_MONOTONIC, 0 = not yet */
};

/* Pixel value at (x, y) in the wire format. Caller holds fb->lock and
 * checked fb->pixels. */
static inline uint32_t view_fb_pixel(const struct view_fb *fb, unsigned x,
                                     unsigned y)
{
    const uint8_t *p = fb->pixels + (size_t)y * fb->stride +
                       (size_t)x * fb->bytespp;
    if (fb->bytespp == 2)
        return (uint32_t)p[0] | (uint32_t)p[1] << 8;
    return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16);
}

/* Counters for --stats and the window title. Written by one thread
 * each, read by the main thread: relaxed atomics are plenty. */
struct view_stats {
    /* network thread */
    atomic_ullong rx_batches;       /* published pixel batches ("frames") */
    atomic_ullong rx_rects;
    atomic_ullong rx_bytes;         /* wire bytes, headers included */
    atomic_ullong decode_ns;        /* decode + copy into the shared fb */
    atomic_ullong decode_n;
    atomic_ullong pings;
    atomic_ullong ping_queue_us;    /* sum over the pings of the queueing
                                       excess (see net.c) */
    atomic_uint tcp_rtt_us;         /* kernel smoothed RTT (TCP_INFO) */
    /* main thread */
    atomic_ullong presents;
    atomic_ullong present_ns;       /* texture upload + render + present */
};

struct view_opts {
    const char *server;
    const char *port;
    unsigned req_w, req_h;          /* --size */
    uint8_t pixfmt;                 /* requested wire format */
    int no_lz4;
    int no_input;                   /* never offer INPUT (view only) */
    uint8_t mac[6];
    char model[13];                 /* not NUL-terminated when full */
};

/* Network thread API (net.c). */
struct view_net;

/* Callback from the network thread: something changed (pixels published
 * or connection state). Must be cheap and thread-safe. */
typedef void (*view_wake_fn)(void);

struct view_net *view_net_start(const struct view_opts *o,
                                struct view_fb *fb, struct view_stats *st,
                                view_wake_fn wake);
void view_net_stop(struct view_net *n);

/* Upstream messages from any thread (PONG, INPUT). Returns -1 when there
 * is no live connection. Serialized with the network thread's own
 * writes. */
int view_net_send(struct view_net *n, const void *msg, size_t len);

/* SDL scancode -> evdev KEY_* code, 0 = no equivalent (keymap.c). */
int view_scancode_to_evdev(int scancode);

/* Logging, same shape as the SBC client. */
void view_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* SREMFB_VIEW_H */
