/*
 * sremfb-view audio: the server's desktop sound, received as raw PCM over
 * UDP (see protocol.h, SREMFB_SRV_FLAG_AUDIO) and played through an SDL3
 * audio stream — latency first, like the picture.
 *
 * A thread of its own per connection: it opens the UDP flow (AUDIO_HELLO
 * every second, which is also the keepalive and the clock probe), and puts
 * each packet into the SDL stream the moment it arrives. The SDL stream
 * *is* the jitter buffer, and it is kept short:
 *
 *   - start (or restart after a pause in the sound): TARGET ms of silence
 *     first, then the packets — the buffer starts at the target;
 *   - above TARGET + DROP_SLACK ms queued, whole packets are thrown away
 *     until the level is back to the target: latency never creeps up
 *     after a network hiccup or a scheduling stall;
 *   - a hole in the sequence numbers (lost packet) is filled with the
 *     same amount of silence, a late or duplicate packet is dropped;
 *   - the two sound cards (the server's graph clock, ours) never run at
 *     exactly the same rate: a slow control loop nudges the stream's
 *     frequency ratio (at most ±0.3 %, inaudible) so the average level
 *     stays on the target instead of drifting into drops or underruns.
 *
 * Latency estimate for --stats: the packet timestamps are the server's
 * clock; each ECHO gives the offset between the two clocks (NTP style,
 * the sample with the smallest round trip of the last few wins), so
 * arrival - timestamp is the one-way delay; the queue ahead of a packet
 * and the device period add up to when it is heard (the local sound
 * server and the DAC are not counted).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "view.h"

#define BYTES_PER_FRAME   (SREMFB_AUDIO_CHANNELS * 2)
#define DROP_SLACK_MS     10.0        /* above target + this: drop */
#define IDLE_GAP_NS       60000000ull /* no packet for 60 ms: sound paused,
                                         the next one restarts the buffer */
#define HELLO_FAST_MS     250         /* until the first packet */
#define HELLO_EVERY_MS    1000
#define ECHO_WINDOW       8
#define RATIO_MAX         0.003
#define RATIO_GAIN        0.0003      /* per ms of level error */
#define CONCEAL_MAX       8           /* packets of silence for a hole */

struct view_audio {
    struct view_stats *st;
    struct view_audio_probe *probe;
    double target_ms;
    FILE *dump;

    pthread_t thread;
    int stop_fd;
    atomic_int stop;
    int sock;
    uint16_t token;

    SDL_AudioStream *stream;
    double dev_ms;                  /* device period, for the estimate */

    /* receiver state (thread only) */
    int started;
    int dropping;
    uint32_t next_seq;
    uint64_t last_rx_ns;
    double level_ewma_ms;
    double ratio;
    unsigned since_ratio;

    /* clock offset server - client, µs */
    struct { int64_t rtt, off; } echo[ECHO_WINDOW];
    unsigned echo_n, echo_i;
    int64_t offset_us;
    int have_offset;
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double queued_ms(struct view_audio *a)
{
    int q = SDL_GetAudioStreamQueued(a->stream);
    return q > 0 ? (double)q / BYTES_PER_FRAME * 1000.0 / SREMFB_AUDIO_RATE
                 : 0.0;
}

static void put(struct view_audio *a, const void *pcm, size_t len)
{
    SDL_PutAudioStreamData(a->stream, pcm, (int)len);
    if (a->dump)
        fwrite(pcm, 1, len, a->dump);
}

static void put_silence(struct view_audio *a, double ms)
{
    static const uint8_t zero[SREMFB_AUDIO_MAX_FRAMES * BYTES_PER_FRAME];
    size_t len = (size_t)(ms * SREMFB_AUDIO_RATE / 1000.0) * BYTES_PER_FRAME;

    while (len > 0) {
        size_t n = len < sizeof(zero) ? len : sizeof(zero);
        put(a, zero, n);
        len -= n;
    }
}

static void send_hello(struct view_audio *a)
{
    struct sremfb_udp_hello h = {
        .magic = SREMFB_MAGIC,
        .type = SREMFB_UDP_AUDIO_HELLO,
        .token = a->token,
        .t_client_us = now_ns() / 1000,
    };
    send(a->sock, &h, sizeof(h), MSG_DONTWAIT | MSG_NOSIGNAL);
}

static void on_echo(struct view_audio *a, const struct sremfb_udp_echo *e)
{
    int64_t t3 = (int64_t)(now_ns() / 1000);
    int64_t t1 = (int64_t)e->t_client_us;
    int64_t rtt = t3 - t1;

    if (rtt < 0 || rtt > 1000000)
        return;
    a->echo[a->echo_i] = (typeof(a->echo[0])){
        rtt, (int64_t)e->t_server_us - (t1 + t3) / 2 };
    a->echo_i = (a->echo_i + 1) % ECHO_WINDOW;
    if (a->echo_n < ECHO_WINDOW)
        a->echo_n++;
    unsigned best = 0;
    for (unsigned i = 1; i < a->echo_n; i++)
        if (a->echo[i].rtt < a->echo[best].rtt)
            best = i;
    a->offset_us = a->echo[best].off;
    a->have_offset = 1;
    atomic_store_explicit(&a->st->a_rtt_us, (unsigned)a->echo[best].rtt,
                          memory_order_relaxed);
}

/* --latency-test: the probe's click is a burst of full anti-phase
 * stereo (left +, right -) — nothing a game mixes by accident. Returns
 * the frame index of its onset in this packet, or -1. */
static int find_click(const int16_t *s, unsigned frames, unsigned *run)
{
    for (unsigned i = 0; i < frames; i++) {
        int l = s[2 * i], r = s[2 * i + 1];
        if (l > 4000 && r < -4000 && abs(l + r) < l / 8) {
            if (++*run == 16)
                return (int)i - 15 < 0 ? 0 : (int)i - 15;
        } else {
            *run = 0;
        }
    }
    return -1;
}

static void on_packet(struct view_audio *a, const uint8_t *buf, size_t len,
                      uint64_t rx_ns)
{
    const struct sremfb_audio_hdr *h = (const void *)buf;
    unsigned frames = h->frames;
    size_t pcm_len = (size_t)frames * BYTES_PER_FRAME;
    double pkt_ms = frames * 1000.0 / SREMFB_AUDIO_RATE;
    struct view_stats *st = a->st;

    if (h->format != SREMFB_AUDIO_S16LE_48K_STEREO || frames == 0 ||
        frames > SREMFB_AUDIO_MAX_FRAMES || len != sizeof(*h) + pcm_len)
        return;
    atomic_fetch_add_explicit(&st->a_packets, 1, memory_order_relaxed);

    /* a pause in the sound (or the very first packet): start over */
    if (!a->started || rx_ns - a->last_rx_ns > IDLE_GAP_NS) {
        if (a->started && queued_ms(a) <= 0.0)
            atomic_fetch_add_explicit(&st->a_restarts, 1,
                                      memory_order_relaxed);
        a->started = 0;
    }
    a->last_rx_ns = rx_ns;

    if (a->started) {
        int32_t d = (int32_t)(h->seq - a->next_seq);
        if (d < 0) {                    /* late or duplicate */
            atomic_fetch_add_explicit(&st->a_late, 1, memory_order_relaxed);
            return;
        }
        if (d > 0) {
            atomic_fetch_add_explicit(&st->a_lost, (unsigned)d,
                                      memory_order_relaxed);
            if (d <= CONCEAL_MAX)
                put_silence(a, d * pkt_ms);
            else
                a->started = 0;         /* long outage: fresh start */
        }
    }

    double q = queued_ms(a);
    if (!a->started) {
        SDL_ClearAudioStream(a->stream);
        put_silence(a, a->target_ms);
        q = a->target_ms;
        a->started = 1;
        a->dropping = 0;
        a->level_ewma_ms = a->target_ms;
    } else if (q <= 0.0) {
        atomic_fetch_add_explicit(&st->a_underruns, 1, memory_order_relaxed);
        put_silence(a, a->target_ms);  /* back to the target at once */
        q = a->target_ms;
    }
    a->next_seq = h->seq + 1;

    /* too deep: throw packets away until back on the target */
    if (q > a->target_ms + DROP_SLACK_MS)
        a->dropping = 1;
    if (a->dropping) {
        if (q > a->target_ms) {
            atomic_fetch_add_explicit(&st->a_dropped, 1, memory_order_relaxed);
            return;
        }
        a->dropping = 0;
    }

    const uint8_t *pcm = buf + sizeof(*h);
    struct view_audio_probe *p = a->probe;
    if (p && atomic_load_explicit(&p->armed, memory_order_acquire)) {
        int at = find_click((const int16_t *)pcm, frames, &p->run);
        if (at >= 0) {
            /* heard when everything queued ahead has played, plus the
             * device period */
            double ahead = q + at * 1000.0 / SREMFB_AUDIO_RATE + a->dev_ms;
            atomic_store(&p->out_ns, rx_ns + (uint64_t)(ahead * 1e6));
            atomic_store(&p->hit_ns, rx_ns);
            atomic_store_explicit(&p->armed, 0, memory_order_release);
        }
    }
    put(a, pcm, pcm_len);

    /* stats: one-way delay, level, estimated latency */
    if (a->have_offset) {
        int64_t owd = (int64_t)(rx_ns / 1000) -
                      ((int64_t)h->t_us - a->offset_us);
        atomic_fetch_add_explicit(&st->a_owd_us, owd > 0 ? owd : 0,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&st->a_owd_n, 1, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&st->a_level_us, (unsigned long long)(q * 1000),
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&st->a_level_n, 1, memory_order_relaxed);

    /* clock drift: nudge the playback rate so the average level sits on
     * the target (the level right after a put swings by one packet and
     * one device period; its mean is what drifts) */
    a->level_ewma_ms += 0.002 * (q + pkt_ms - a->level_ewma_ms);
    if (++a->since_ratio >= 40) {
        double err = a->level_ewma_ms - (a->target_ms + pkt_ms);
        double r = 1.0 + err * RATIO_GAIN;
        if (r > 1.0 + RATIO_MAX) r = 1.0 + RATIO_MAX;
        if (r < 1.0 - RATIO_MAX) r = 1.0 - RATIO_MAX;
        if (r != a->ratio) {
            SDL_SetAudioStreamFrequencyRatio(a->stream, (float)r);
            a->ratio = r;
        }
        atomic_store_explicit(&st->a_ratio_ppm, (int)((r - 1.0) * 1e6),
                              memory_order_relaxed);
        a->since_ratio = 0;
    }
}

static void *audio_thread(void *arg)
{
    struct view_audio *a = arg;
    uint8_t buf[2048];
    uint64_t next_hello = 0;
    int got_audio = 0;

    while (!atomic_load(&a->stop)) {
        uint64_t now = now_ns();
        if (now >= next_hello) {
            send_hello(a);
            next_hello = now + (uint64_t)(got_audio ? HELLO_EVERY_MS
                                                    : HELLO_FAST_MS) * 1000000;
        }
        struct pollfd pfd[2] = {
            { .fd = a->stop_fd, .events = POLLIN },
            { .fd = a->sock, .events = POLLIN },
        };
        int timeout = (int)((next_hello - now) / 1000000) + 1;
        int r = poll(pfd, 2, timeout);
        if (r < 0 && errno != EINTR)
            break;
        if (r <= 0)
            continue;
        if (pfd[0].revents)
            break;
        for (;;) {
            ssize_t n = recv(a->sock, buf, sizeof(buf), MSG_DONTWAIT);
            uint64_t rx = now_ns();
            if (n < 0)
                break;
            if ((size_t)n < 8 || ((const uint32_t *)buf)[0] != SREMFB_MAGIC)
                continue;
            if (buf[4] == SREMFB_UDP_AUDIO &&
                (size_t)n >= sizeof(struct sremfb_audio_hdr)) {
                if (!got_audio) {
                    got_audio = 1;
                    view_log("audio: receiving (buffer target %.0f ms, "
                             "device period %.1f ms)", a->target_ms,
                             a->dev_ms);
                }
                on_packet(a, buf, (size_t)n, rx);
            } else if (buf[4] == SREMFB_UDP_ECHO &&
                       (size_t)n == sizeof(struct sremfb_udp_echo)) {
                struct sremfb_udp_echo e;
                memcpy(&e, buf, sizeof(e));
                on_echo(a, &e);
            }
        }
    }
    return NULL;
}

struct view_audio *view_audio_start(const struct sockaddr *srv,
                                    socklen_t srvlen, uint16_t token,
                                    const struct view_opts *o,
                                    struct view_stats *st,
                                    struct view_audio_probe *probe)
{
    struct view_audio *a = calloc(1, sizeof(*a));
    SDL_AudioSpec spec = { SDL_AUDIO_S16LE, SREMFB_AUDIO_CHANNELS,
                           SREMFB_AUDIO_RATE };

    if (!a)
        return NULL;
    a->st = st;
    a->probe = probe;
    a->token = token;
    a->target_ms = o->audio_target_ms;
    a->ratio = 1.0;
    a->sock = socket(srv->sa_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (a->sock < 0 || connect(a->sock, srv, srvlen) < 0) {
        view_log("audio: UDP socket: %s", strerror(errno));
        goto fail;
    }
    int tos = 0xb8;                     /* DSCP EF, like the server */
    setsockopt(a->sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    int rcvbuf = 64 * 1024;
    setsockopt(a->sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &spec, NULL, NULL);
    if (!a->stream) {
        view_log("audio: cannot open the sound device: %s", SDL_GetError());
        goto fail;
    }
    SDL_AudioSpec dspec;
    int dframes = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(a->stream), &dspec,
                                 &dframes) && dframes > 0 && dspec.freq > 0)
        a->dev_ms = dframes * 1000.0 / dspec.freq;
    if (o->audio_dump) {
        a->dump = fopen(o->audio_dump, "ab");
        if (!a->dump)
            view_log("audio: cannot write %s: %s", o->audio_dump,
                     strerror(errno));
    }
    SDL_ResumeAudioStreamDevice(a->stream);

    a->stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (a->stop_fd < 0 || pthread_create(&a->thread, NULL, audio_thread, a)) {
        view_log("audio: cannot start the thread");
        if (a->stop_fd >= 0)
            close(a->stop_fd);
        goto fail;
    }
    atomic_store(&st->a_active, 1);
    return a;

fail:
    if (a->stream)
        SDL_DestroyAudioStream(a->stream);
    if (a->dump)
        fclose(a->dump);
    if (a->sock >= 0)
        close(a->sock);
    free(a);
    return NULL;
}

void view_audio_stop(struct view_audio *a)
{
    uint64_t one = 1;

    if (!a)
        return;
    atomic_store(&a->stop, 1);
    if (write(a->stop_fd, &one, sizeof(one)) < 0)
        view_log("audio: cannot signal the thread");
    pthread_join(a->thread, NULL);
    atomic_store(&a->st->a_active, 0);
    close(a->stop_fd);
    close(a->sock);
    SDL_DestroyAudioStream(a->stream);
    if (a->dump)
        fclose(a->dump);
    free(a);
}
