/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * sremfb-view network thread: connect, hello, receive + decode, PONG.
 *
 * Speaks protocol v2 exactly like the SBC client (see PROTOCOL.md), minus
 * the H.264 capability, which is never advertised: this viewer is meant
 * for latency-critical use and stays on the damage-rect RAW/LZ4 path.
 * It offers INPUT (unless --no-input) and AUDIO (unless --no-audio: the
 * sound then runs on its own UDP flow and thread, audio.c); the main
 * thread only sends input
 * events once the server hello confirmed it (fb->input).
 *
 * Everything that can block — connect, reads, the reconnect backoff —
 * polls the socket together with an eventfd, so view_net_stop() returns
 * promptly whatever the thread is doing.
 *
 * Batching: the server sends each grabbed frame as up to 16 rects back to
 * back, with no end-of-frame marker. Rects land in the shared framebuffer
 * as soon as they are decoded, but the main thread is only woken once the
 * thread has caught up with the socket (no complete header waiting) or a
 * batch has been open for BATCH_MAX_NS — so a frame is presented whole
 * when the link allows it, and never held back longer than that.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <lz4.h>

#include "view.h"

#define BATCH_MAX_NS      2000000ull  /* publish a batch after 2 ms anyway */
#define CONNECT_TIMEOUT   5000        /* ms */
#define HELLO_TIMEOUT     15000       /* ms: the server waits up to 10 s
                                         for the compositor's mode set */
#define SILENCE_TIMEOUT   6000        /* ms, only when PING is negotiated
                                         (the server heartbeats every ~2 s) */
#define SEND_TIMEOUT      2000        /* ms, upstream messages are tiny */
#define BACKOFF_MAX       5           /* s */

struct view_net {
    struct view_opts o;
    struct view_fb *fb;
    struct view_stats *st;
    view_wake_fn wake;

    pthread_t thread;
    int stop_fd;                /* eventfd, readable once stopping */
    atomic_int stop;

    pthread_mutex_t wlock;      /* serializes upstream writes; guards fd */
    int fd;                     /* -1 while not streaming */

    /* current connection */
    int sock;
    uint8_t srv_flags;
    int64_t ping_min_off_us;    /* min(arrival - server clock) seen */
    int have_ping_off;
    uint16_t audio_token;
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Waits for `events` on fd, or for the stop eventfd. Returns 1 ready,
 * 0 timeout, -1 stop/error. timeout_ms < 0 = forever. */
static int wait_fd(struct view_net *n, int fd, short events, int timeout_ms)
{
    struct pollfd pfd[2] = {
        { .fd = n->stop_fd, .events = POLLIN },
        { .fd = fd, .events = events },
    };
    for (;;) {
        int nf = fd >= 0 ? 2 : 1;
        int r = poll(pfd, (nfds_t)nf, timeout_ms);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (atomic_load(&n->stop) || pfd[0].revents)
            return -1;
        if (r == 0)
            return 0;
        return 1;
    }
}

static void sleep_s(struct view_net *n, unsigned s)
{
    wait_fd(n, -1, 0, (int)s * 1000);
}

/* Reads exactly len bytes. timeout_ms bounds each silence, not the whole
 * read. Returns 0, or -1 on EOF/error/stop/timeout (logged). */
static int readn(struct view_net *n, void *buf, size_t len, int timeout_ms)
{
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t r = recv(n->sock, p, len, MSG_DONTWAIT);
        if (r > 0) {
            p += r;
            len -= (size_t)r;
            continue;
        }
        if (r == 0) {
            view_log("server closed the connection");
            return -1;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            view_log("receive failed: %s", strerror(errno));
            return -1;
        }
        int w = wait_fd(n, n->sock, POLLIN, timeout_ms);
        if (w < 0)
            return -1;
        if (w == 0) {
            view_log("server silent for %d s — dropping to reconnect",
                     timeout_ms / 1000);
            return -1;
        }
    }
    return 0;
}

static int writen_locked(struct view_net *n, int fd, const void *buf,
                         size_t len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t r = send(fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (r > 0) {
            p += r;
            len -= (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(n, fd, POLLOUT, SEND_TIMEOUT) == 1)
                continue;
        }
        return -1;
    }
    return 0;
}

int view_net_send(struct view_net *n, const void *msg, size_t len)
{
    int rc = -1;
    pthread_mutex_lock(&n->wlock);
    if (n->fd >= 0)
        rc = writen_locked(n, n->fd, msg, len);
    pthread_mutex_unlock(&n->wlock);
    return rc;
}

/* ---------------------------------------------------------- connect */

static int tcp_connect(struct view_net *n)
{
    struct addrinfo hints = {0}, *res = NULL, *ai;
    int fd = -1;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(n->o.server, n->o.port, &hints, &res);
    if (rc != 0) {
        view_log("resolve %s: %s", n->o.server, gai_strerror(rc));
        return -1;
    }
    for (ai = res; ai && !atomic_load(&n->stop); ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK |
                    SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        if (errno == EINPROGRESS &&
            wait_fd(n, fd, POLLOUT, CONNECT_TIMEOUT) == 1) {
            int err = 0;
            socklen_t el = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 &&
                err == 0)
                break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        /* same tuning as the SBC client and the server: no Nagle, dead
         * peers noticed in seconds rather than hours */
        int one = 1, idle = 10, intvl = 5, cnt = 3, user_to = 6000;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
        setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_to,
                   sizeof(user_to));
    }
    return fd;
}

static int hello_exchange(struct view_net *n, unsigned *w, unsigned *h)
{
    struct sremfb_client_hello ch = {0};
    struct sremfb_server_hello sh;

    ch.magic = SREMFB_MAGIC;
    ch.proto_ver = SREMFB_PROTO_VER;
    if (!n->o.no_lz4)
        ch.flags |= SREMFB_HELLO_FLAG_LZ4;
    ch.flags |= SREMFB_HELLO_FLAG_FEEDBACK;
    if (!n->o.no_input)
        ch.flags |= SREMFB_HELLO_FLAG_INPUT;
    if (!n->o.no_audio)
        ch.flags |= SREMFB_HELLO_FLAG_AUDIO;
    /* never SREMFB_HELLO_FLAG_H264: no inter-frame codec in this viewer */
    ch.xres = (uint16_t)n->o.req_w;
    ch.yres = (uint16_t)n->o.req_h;
    ch.pixfmt = n->o.pixfmt;
    if (n->o.pixfmt == SREMFB_PIX_RGB565) {
        ch.bpp = 16;
        ch.red_off = 11;  ch.red_len = 5;
        ch.green_off = 5; ch.green_len = 6;
        ch.blue_off = 0;  ch.blue_len = 5;
    } else {
        ch.bpp = 32;
        ch.red_off = 16;  ch.red_len = 8;
        ch.green_off = 8; ch.green_len = 8;
        ch.blue_off = 0;  ch.blue_len = 8;
    }
    memcpy(ch.mac, n->o.mac, 6);
    memcpy(ch.model, n->o.model, sizeof(ch.model));

    pthread_mutex_lock(&n->wlock);
    int rc = writen_locked(n, n->sock, &ch, sizeof(ch));
    pthread_mutex_unlock(&n->wlock);
    if (rc < 0) {
        view_log("failed to send hello: %s", strerror(errno));
        return -1;
    }
    if (readn(n, &sh, sizeof(sh), HELLO_TIMEOUT) < 0) {
        view_log("no server hello");
        return -1;
    }
    if (sh.magic != SREMFB_MAGIC || sh.proto_ver != SREMFB_PROTO_VER) {
        view_log("bad server hello (magic/version mismatch)");
        return -1;
    }
    if (sh.status != SREMFB_STATUS_OK) {
        view_log("server refused: status %u%s", sh.status,
                 sh.status == SREMFB_STATUS_NO_DEVICE ?
                 " (no free screen on the server)" :
                 sh.status == SREMFB_STATUS_SERVER_FAIL ?
                 " (the compositor never lit the screen)" : "");
        return -1;
    }
    if (sh.pixfmt != n->o.pixfmt) {
        view_log("server offers pixfmt %u, we asked for %u", sh.pixfmt,
                 n->o.pixfmt);
        return -1;
    }
    if (sh.width == 0 || sh.height == 0) {
        view_log("server announced an empty stream");
        return -1;
    }
    *w = sh.width;
    *h = sh.height;
    n->srv_flags = sh.flags;
    n->audio_token = sh.audio_token;
    view_log("connected to %s: stream %ux%u %s (mac %02x:%02x:%02x:%02x:"
             "%02x:%02x)%s", n->o.server, *w, *h,
             sh.pixfmt == SREMFB_PIX_RGB565 ? "RGB565" : "XRGB8888",
             ch.mac[0], ch.mac[1], ch.mac[2], ch.mac[3], ch.mac[4], ch.mac[5],
             (sh.flags & SREMFB_SRV_FLAG_PING) ? "" :
             " — old server without PING, liveness via TCP keepalive");
    if (!n->o.no_input)
        view_log("input %s", (sh.flags & SREMFB_SRV_FLAG_INPUT) ?
                 "accepted by the server" :
                 "not accepted by the server (SREMFB_INPUT off or older "
                 "server): view only");
    if (!n->o.no_audio)
        view_log("audio %s", (sh.flags & SREMFB_SRV_FLAG_AUDIO) ?
                 "offered by the server" :
                 "not offered by the server (SREMFB_AUDIO=0, no PipeWire "
                 "or older server)");
    return 0;
}

/* ------------------------------------------------------ shared state */

static void fb_set_stream(struct view_net *n, unsigned w, unsigned h,
                          uint8_t *pixels)
{
    struct view_fb *fb = n->fb;

    pthread_mutex_lock(&fb->lock);
    free(fb->pixels);
    fb->pixels = pixels;
    fb->w = w;
    fb->h = h;
    fb->pixfmt = n->o.pixfmt;
    fb->bytespp = n->o.pixfmt == SREMFB_PIX_RGB565 ? 2 : 4;
    fb->stride = (size_t)w * fb->bytespp;
    fb->connected = pixels != NULL;
    fb->input = pixels != NULL && (n->srv_flags & SREMFB_SRV_FLAG_INPUT);
    fb->probe_armed = 0;
    fb->blanked = 0;
    fb->damage_n = 0;
    fb->damage_all = 1;
    fb->gen++;
    pthread_mutex_unlock(&fb->lock);
    n->wake();
}

/* Caller holds fb->lock. */
static void damage_add(struct view_fb *fb, unsigned x, unsigned y,
                       unsigned w, unsigned h)
{
    if (fb->damage_all)
        return;
    if (fb->damage_n == VIEW_DAMAGE_MAX) {
        /* overflow: collapse to the bounding box of everything pending */
        unsigned x0 = x, y0 = y, x1 = x + w, y1 = y + h;
        for (int i = 0; i < fb->damage_n; i++) {
            const struct view_rect *r = &fb->damage[i];
            if (r->x < x0) x0 = r->x;
            if (r->y < y0) y0 = r->y;
            if ((unsigned)r->x + r->w > x1) x1 = (unsigned)r->x + r->w;
            if ((unsigned)r->y + r->h > y1) y1 = (unsigned)r->y + r->h;
        }
        fb->damage_n = 0;
        x = x0; y = y0; w = x1 - x0; h = y1 - y0;
    }
    fb->damage[fb->damage_n++] = (struct view_rect){
        (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h };
}

/* Decoded rect (tightly packed rows) -> shared framebuffer + damage. */
static void fb_put_rect(struct view_net *n, const struct sremfb_frame_hdr *hd,
                        const uint8_t *src)
{
    struct view_fb *fb = n->fb;
    size_t row = (size_t)hd->w * fb->bytespp;

    pthread_mutex_lock(&fb->lock);
    uint8_t *dst = fb->pixels + (size_t)hd->y * fb->stride +
                   (size_t)hd->x * fb->bytespp;
    if (row == fb->stride) {
        memcpy(dst, src, row * hd->h);
    } else {
        for (unsigned y = 0; y < hd->h; y++)
            memcpy(dst + y * fb->stride, src + y * row, row);
    }
    damage_add(fb, hd->x, hd->y, hd->w, hd->h);
    if (fb->probe_armed && fb->probe_x >= hd->x && fb->probe_y >= hd->y &&
        fb->probe_x < (unsigned)hd->x + hd->w &&
        fb->probe_y < (unsigned)hd->y + hd->h &&
        view_fb_pixel(fb, fb->probe_x, fb->probe_y) != fb->probe_base) {
        fb->probe_hit_ns = now_ns();
        fb->probe_armed = 0;
    }
    pthread_mutex_unlock(&fb->lock);
}

static void publish(struct view_net *n)
{
    atomic_fetch_add_explicit(&n->st->rx_batches, 1, memory_order_relaxed);
    pthread_mutex_lock(&n->fb->lock);
    n->fb->batches++;
    pthread_mutex_unlock(&n->fb->lock);
    n->wake();
}

static void sample_tcp_rtt(struct view_net *n)
{
    struct tcp_info ti;
    socklen_t tl = sizeof(ti);
    if (getsockopt(n->sock, IPPROTO_TCP, TCP_INFO, &ti, &tl) == 0)
        atomic_store_explicit(&n->st->tcp_rtt_us, ti.tcpi_rtt,
                              memory_order_relaxed);
}

/* ----------------------------------------------------------- stream */

/* Publishes the open batch unless `need` more bytes are already waiting
 * in the socket: whatever comes next (a PING, a rect still in flight)
 * must never hold decoded pixels back. */
static void batch_flush_unless(struct view_net *n, uint64_t *batch_t0,
                               size_t need)
{
    int avail = 0;

    if (!*batch_t0)
        return;
    if (ioctl(n->sock, FIONREAD, &avail) < 0 || (size_t)avail < need ||
        now_ns() - *batch_t0 >= BATCH_MAX_NS) {
        publish(n);
        *batch_t0 = 0;
    }
}

/* PING: echo it back right away. Everything queued ahead of it has
 * already been decoded into the shared framebuffer, which is what the
 * protocol asks ("applied"); presentation runs on its own and is not
 * included on purpose — a blocked present (hidden window, vsync) must
 * never stop the heartbeat.
 *
 * Client-side delay estimate for --stats: (arrival - server clock) is
 * the unknown clock offset plus the one-way delay plus the queueing in
 * front of the PING. Its minimum over the connection approximates offset
 * + base delay, so the excess over it is the queueing delay. */
static int on_ping(struct view_net *n, uint64_t t_srv_us)
{
    struct sremfb_client_msg pong = {0};
    pong.magic = SREMFB_MAGIC;
    pong.type = SREMFB_CMSG_PONG;
    pong.t_echo_us = t_srv_us;
    if (view_net_send(n, &pong, sizeof(pong)) < 0) {
        view_log("failed to send PONG");
        return -1;
    }

    int64_t off = (int64_t)(now_ns() / 1000) - (int64_t)t_srv_us;
    if (!n->have_ping_off || off < n->ping_min_off_us) {
        n->ping_min_off_us = off;
        n->have_ping_off = 1;
    }
    atomic_fetch_add_explicit(&n->st->pings, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&n->st->ping_queue_us,
                              (unsigned long long)(off - n->ping_min_off_us),
                              memory_order_relaxed);
    sample_tcp_rtt(n);
    return 0;
}

static void stream_loop(struct view_net *n, unsigned w, unsigned h)
{
    unsigned bytespp = n->o.pixfmt == SREMFB_PIX_RGB565 ? 2 : 4;
    size_t frame_cap = (size_t)w * h * bytespp;
    size_t comp_cap = (size_t)LZ4_compressBound((int)frame_cap);
    uint8_t *scratch = malloc(frame_cap);
    uint8_t *comp = n->o.no_lz4 ? NULL : malloc(comp_cap);
    uint8_t *pixels = calloc(frame_cap, 1);
    int timeout = (n->srv_flags & SREMFB_SRV_FLAG_PING) ? SILENCE_TIMEOUT : -1;
    uint64_t batch_t0 = 0;           /* 0 = no unpublished rect */

    if (!scratch || !pixels || (!n->o.no_lz4 && !comp)) {
        view_log("out of memory");
        free(scratch);
        free(comp);
        free(pixels);
        return;
    }
    n->have_ping_off = 0;
    fb_set_stream(n, w, h, pixels);     /* fb owns `pixels` now */
    pthread_mutex_lock(&n->wlock);
    n->fd = n->sock;
    pthread_mutex_unlock(&n->wlock);
    sample_tcp_rtt(n);

    /* sound: its own UDP flow and thread, to the same server address */
    struct view_audio *audio = NULL;
    if (!n->o.no_audio && (n->srv_flags & SREMFB_SRV_FLAG_AUDIO)) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        if (getpeername(n->sock, (struct sockaddr *)&ss, &sl) == 0)
            audio = view_audio_start((struct sockaddr *)&ss, sl,
                                     n->audio_token, &n->o, n->st,
                                     &n->fb->aprobe);
    }

    while (!atomic_load(&n->stop)) {
        struct sremfb_frame_hdr hd;
        batch_flush_unless(n, &batch_t0, sizeof(hd));
        if (readn(n, &hd, sizeof(hd), timeout) < 0)
            break;
        if (hd.magic != SREMFB_MAGIC) {
            view_log("bad frame magic 0x%08x, dropping connection", hd.magic);
            break;
        }
        atomic_fetch_add_explicit(&n->st->rx_bytes,
                                  sizeof(hd) + hd.payload_len,
                                  memory_order_relaxed);

        if (hd.encoding != SREMFB_ENC_RAW && hd.encoding != SREMFB_ENC_LZ4 &&
            batch_t0) {                 /* control message: pixels first */
            publish(n);
            batch_t0 = 0;
        }
        if (hd.encoding == SREMFB_ENC_BLANK ||
            hd.encoding == SREMFB_ENC_UNBLANK) {
            if (hd.payload_len != 0) {
                view_log("bad control message payload len %u",
                         hd.payload_len);
                break;
            }
            pthread_mutex_lock(&n->fb->lock);
            n->fb->blanked = hd.encoding == SREMFB_ENC_BLANK;
            pthread_mutex_unlock(&n->fb->lock);
            view_log("server %s the screen",
                     hd.encoding == SREMFB_ENC_BLANK ? "blanked" : "unblanked");
            n->wake();
            continue;
        }

        if (hd.encoding == SREMFB_ENC_PING) {
            uint64_t t;
            if (hd.payload_len != sizeof(t)) {
                view_log("bad PING payload len %u", hd.payload_len);
                break;
            }
            if (readn(n, &t, sizeof(t), timeout) < 0 || on_ping(n, t) < 0)
                break;
            continue;
        }

        if (hd.encoding != SREMFB_ENC_RAW && hd.encoding != SREMFB_ENC_LZ4) {
            /* H.264 is never negotiated: anything else is a protocol
             * error (or a newer server we don't understand) */
            view_log("unsupported encoding %u, dropping connection",
                     hd.encoding);
            break;
        }
        if (hd.w == 0 || hd.h == 0 ||
            (unsigned)hd.x + hd.w > w || (unsigned)hd.y + hd.h > h) {
            view_log("bad frame rect %ux%u+%u+%u", hd.w, hd.h, hd.x, hd.y);
            break;
        }
        size_t rect_bytes = (size_t)hd.w * hd.h * bytespp;
        /* a batch ages from the moment its first rect started arriving:
         * a rect that took longer than BATCH_MAX_NS to receive (a full
         * frame on a saturated link) is published at once instead of
         * waiting for the next one */
        batch_flush_unless(n, &batch_t0, hd.payload_len);
        if (!batch_t0)
            batch_t0 = now_ns();

        if (hd.encoding == SREMFB_ENC_RAW) {
            if (hd.payload_len != rect_bytes) {
                view_log("bad RAW payload len %u (rect %zu)", hd.payload_len,
                         rect_bytes);
                break;
            }
            if (readn(n, scratch, rect_bytes, timeout) < 0)
                break;
        } else {
            if (!comp || hd.payload_len == 0 || hd.payload_len > comp_cap) {
                view_log("bad LZ4 payload len %u", hd.payload_len);
                break;
            }
            if (readn(n, comp, hd.payload_len, timeout) < 0)
                break;
            uint64_t t0 = now_ns();
            int d = LZ4_decompress_safe((const char *)comp, (char *)scratch,
                                        (int)hd.payload_len, (int)rect_bytes);
            if (d != (int)rect_bytes) {
                view_log("LZ4 decode failed (%d, expected %zu)", d,
                         rect_bytes);
                break;
            }
            atomic_fetch_add_explicit(&n->st->decode_ns, now_ns() - t0,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&n->st->decode_n, 1,
                                      memory_order_relaxed);
        }
        fb_put_rect(n, &hd, scratch);
        atomic_fetch_add_explicit(&n->st->rx_rects, 1, memory_order_relaxed);

        /* published at the loop top when caught up (no complete header
         * waiting), before a payload still in flight, or before a
         * control message */
    }

    view_audio_stop(audio);
    pthread_mutex_lock(&n->wlock);
    n->fd = -1;
    pthread_mutex_unlock(&n->wlock);
    fb_set_stream(n, 0, 0, NULL);       /* no signal */
    free(scratch);
    free(comp);
}

static void *net_thread(void *arg)
{
    struct view_net *n = arg;
    unsigned backoff = 1;

    while (!atomic_load(&n->stop)) {
        n->sock = tcp_connect(n);
        if (n->sock < 0) {
            if (atomic_load(&n->stop))
                break;
            view_log("connect to %s:%s failed, retrying in %us",
                     n->o.server, n->o.port, backoff);
            sleep_s(n, backoff);
            if (backoff < BACKOFF_MAX)
                backoff++;
            continue;
        }

        unsigned w, h;
        if (hello_exchange(n, &w, &h) == 0) {
            backoff = 1;
            stream_loop(n, w, h);
            if (!atomic_load(&n->stop))
                view_log("disconnected, reconnecting in %us", backoff);
        } else if (backoff < BACKOFF_MAX) {
            backoff++;                  /* refused: don't hammer the server */
        }
        close(n->sock);
        n->sock = -1;
        if (!atomic_load(&n->stop))
            sleep_s(n, backoff);
    }
    return NULL;
}

struct view_net *view_net_start(const struct view_opts *o,
                                struct view_fb *fb, struct view_stats *st,
                                view_wake_fn wake)
{
    struct view_net *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->o = *o;
    n->fb = fb;
    n->st = st;
    n->wake = wake;
    n->fd = -1;
    n->sock = -1;
    pthread_mutex_init(&n->wlock, NULL);
    n->stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (n->stop_fd < 0 ||
        pthread_create(&n->thread, NULL, net_thread, n) != 0) {
        view_log("cannot start the network thread: %s", strerror(errno));
        if (n->stop_fd >= 0)
            close(n->stop_fd);
        free(n);
        return NULL;
    }
    return n;
}

void view_net_stop(struct view_net *n)
{
    uint64_t one = 1;

    if (!n)
        return;
    atomic_store(&n->stop, 1);
    if (write(n->stop_fd, &one, sizeof(one)) < 0)
        view_log("cannot signal the network thread: %s", strerror(errno));
    pthread_join(n->thread, NULL);
    close(n->stop_fd);
    pthread_mutex_destroy(&n->wlock);
    free(n);
}
