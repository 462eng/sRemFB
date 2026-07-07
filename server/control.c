/*
 * Pressure controller: measures congestion as *delay* and decides, per
 * client, when the raw damage-rect path can't hold ~15 delivered fps and
 * the stream should switch to H.264 — and when to switch back.
 *
 * The delay signal: a PING control message carrying the server's
 * monotonic clock is enqueued into the TCP stream every 250 ms (only
 * while traffic flows); the client echoes it back once it has *applied*
 * everything queued ahead. So (now - echo) covers the kernel send buffer,
 * the network queues and the client's own processing — the end-to-end
 * pressure, with zero clock-sync needed since only our clock is involved.
 *
 * Nothing here is user-configured: link capacity is learned by watching
 * the achieved wire rate while the delay says the link is saturated, the
 * H.264 bitrate then tracks the delay with a classic AIMD loop, and the
 * switch back to RAW happens only after a long calm streak AND a
 * prediction (from the observed damage rate and LZ4 ratio) that RAW
 * would fit in the measured capacity with headroom. The SBC is assumed
 * dedicated to this display, so the delay it sees is representative.
 *
 * Env overrides (A/B testing): SREMFB_FORCE_H264 pins H.264 on capable
 * clients, SREMFB_NO_H264 pins RAW (the delay is still measured/logged).
 */
#include <stdlib.h>
#include <string.h>

#include "sremfb-server.h"

#define PING_INTERVAL_MS   250
#define PING_IDLE_US       2000000   /* heartbeat period on a silent link */
#define PONG_DEAD_US       6000000   /* no echo at all for this long = dead */
#define PINGS_MAX_FLIGHT   4

#define DELAY_HI_US        150000.0  /* pressure */
#define DELAY_AIMD_LO_US   50000.0   /* room to grow the bitrate */
#define DELAY_LO_US        30000.0   /* calm */
#define FPS_MIN            15.0
#define DAMAGE_GAP_US      100000    /* grabs closer than this = continuous */
#define DAMAGE_CONT_US     1000000   /* ... for at least this long */
#define ENTER_CONFIRM_US   500000
#define EXIT_CALM_US       10000000
#define EXIT_HEADROOM      0.5       /* predicted RAW must fit in half the
                                        measured capacity */
#define CAP_WINDOW_US      1000000   /* capacity sampling window */

#define KBPS_MIN           1500
#define KBPS_MAX           40000     /* H.264 L4.0, the Pi 3 ceiling */
#define KBPS_START         8000
#define AIMD_UP_KBPS       500
#define AIMD_DOWN          0.7

#define EWMA(cur, x, a)    ((cur) == 0.0 ? (double)(x) : \
                            (cur) + (a) * ((double)(x) - (cur)))

static int env_force_h264(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("SREMFB_FORCE_H264") != NULL;
    return v;
}

static int env_no_h264(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("SREMFB_NO_H264") != NULL;
    return v;
}

static gboolean h264_allowed(const SremfbClient *c)
{
    return c->h264_cap && c->feedback && !c->h264_failed && !env_no_h264();
}

const char *sremfb_ctl_state_name(const SremfbClient *c)
{
    switch (c->ctl_state) {
    case SREMFB_CTL_RAW_STEADY:    return "raw";
    case SREMFB_CTL_RAW_SUSPECT:   return "raw?";
    case SREMFB_CTL_H264_ACTIVE:   return "h264";
    case SREMFB_CTL_H264_COOLDOWN: return "h264~";
    }
    return "?";
}

static void ctl_set_state(SremfbClient *c, SremfbCtlState s)
{
    if (c->ctl_state == s)
        return;
    c->ctl_state = s;
    c->ctl_since_us = g_get_monotonic_time();
    c->ctl_calm_us = 0;
}

static void send_ping(SremfbClient *c)
{
    struct {
        struct sremfb_frame_hdr hdr;
        uint64_t t_us;
    } __attribute__((packed)) msg = {0};

    msg.hdr.magic = SREMFB_MAGIC;
    msg.hdr.encoding = SREMFB_ENC_PING;
    msg.hdr.payload_len = sizeof(msg.t_us);
    msg.t_us = (uint64_t)g_get_monotonic_time();

    if (!net_send_all(c->fd, &msg, sizeof(msg))) {
        sremfb_schedule_client_lost(c);
        return;
    }
    c->pings_outstanding++;
    c->last_ping_us = (gint64)msg.t_us;
    c->wire_total += sizeof(msg);
    c->ping_wire_mark = c->wire_total;   /* after: pings aren't traffic */
}

/* True while damage keeps arriving back-to-back: separates "low fps
 * because the link is drowning" from "low fps because nothing moves on
 * screen". The streak length itself is checked against damage_cont_us. */
static gboolean damage_continuous(const SremfbClient *c, gint64 now)
{
    return c->last_grab_us && now - c->last_grab_us < DAMAGE_GAP_US;
}

/* What RAW mode would put on the wire right now, from the observed damage
 * byte rate and the LZ4 ratio learned during RAW periods. */
static double raw_predicted_Bps(const SremfbClient *c)
{
    double ratio = c->lz4_ratio_ewma > 1.0 ? c->lz4_ratio_ewma : 1.0;
    return c->raw_rate_Bps / ratio;
}

static void ctl_evaluate(SremfbClient *c)
{
    gint64 now = g_get_monotonic_time();

    switch (c->ctl_state) {
    case SREMFB_CTL_RAW_STEADY:
    case SREMFB_CTL_RAW_SUSPECT: {
        gboolean pressure =
            c->delay_ewma_us > DELAY_HI_US ||
            (c->grab_fps > 0.1 && c->grab_fps < FPS_MIN &&
             damage_continuous(c, now) &&
             now - c->damage_cont_us > DAMAGE_CONT_US);

        if (env_force_h264() && h264_allowed(c))
            pressure = TRUE;

        if (c->ctl_state == SREMFB_CTL_RAW_STEADY) {
            if (pressure) {
                ctl_set_state(c, SREMFB_CTL_RAW_SUSPECT);
                g_message("[%s] pressure: delay %.0f ms, %.1f fps — "
                          "confirming", c->macstr,
                          c->delay_ewma_us / 1000.0, c->grab_fps);
            }
        } else if (!pressure) {
            ctl_set_state(c, SREMFB_CTL_RAW_STEADY);
        } else if (now - c->ctl_since_us > ENTER_CONFIRM_US ||
                   env_force_h264()) {
            if (!h264_allowed(c)) {
                /* nothing we can do for this client; don't spam */
                if (now - c->ctl_since_us > 30 * G_USEC_PER_SEC)
                    ctl_set_state(c, SREMFB_CTL_RAW_STEADY);
                break;
            }
            sremfb_xmit_set_mode(c, SREMFB_XMIT_H264);
            if (c->xmit_mode == SREMFB_XMIT_H264)
                ctl_set_state(c, SREMFB_CTL_H264_ACTIVE);
            else
                ctl_set_state(c, SREMFB_CTL_RAW_STEADY);  /* enc failed */
        }
        break;
    }

    case SREMFB_CTL_H264_ACTIVE:
        if (env_force_h264())
            break;
        if (c->delay_ewma_us < DELAY_LO_US) {
            ctl_set_state(c, SREMFB_CTL_H264_COOLDOWN);
            c->ctl_calm_us = now;
        }
        break;

    case SREMFB_CTL_H264_COOLDOWN: {
        if (c->delay_ewma_us > DELAY_HI_US) {
            ctl_set_state(c, SREMFB_CTL_H264_ACTIVE);
            break;
        }
        if (c->delay_ewma_us > DELAY_LO_US) {
            c->ctl_calm_us = now;              /* calm streak broken */
            break;
        }
        if (!c->ctl_calm_us)
            c->ctl_calm_us = now;
        gint64 calm = now - c->ctl_calm_us;
        if (calm < EXIT_CALM_US)
            break;
        /* The predictor keeps us in H.264 while RAW would clearly drown
         * the measured capacity again — but that measurement can go
         * stale (link healed, damage pattern changed), so after a much
         * longer calm streak we probe RAW anyway; if the pressure comes
         * back, re-entry only takes ~a second. */
        if (c->capacity_Bps > 0.0 &&
            raw_predicted_Bps(c) > EXIT_HEADROOM * c->capacity_Bps &&
            calm < 3 * (gint64)EXIT_CALM_US)
            break;
        sremfb_xmit_set_mode(c, SREMFB_XMIT_RAW);
        ctl_set_state(c, SREMFB_CTL_RAW_STEADY);
        break;
    }
    }
}

/* AIMD on the encoder bitrate, one step per echo. */
static void ctl_aimd(SremfbClient *c)
{
    if (c->xmit_mode != SREMFB_XMIT_H264 || !c->enc)
        return;
    int kbps = sremfb_enc_bitrate(c->enc);
    if (c->delay_ewma_us > DELAY_HI_US)
        kbps = (int)(kbps * AIMD_DOWN);
    else if (c->delay_ewma_us < DELAY_AIMD_LO_US)
        kbps += AIMD_UP_KBPS;
    else
        return;
    kbps = CLAMP(kbps, KBPS_MIN, KBPS_MAX);
    sremfb_enc_set_bitrate(c->enc, kbps);
}

/* In H.264 mode, when the end-to-end delay says the pipe is behind —
 * network queues or the client's decoder (a Pi 3 tops out around
 * 1080p30) — coalesce damage instead of encoding every event: capped
 * near FPS_MIN under pressure. Skipping is free of artifacts because
 * grabbuf is always the full current frame, so the next P-frame carries
 * everything that changed meanwhile. */
gboolean sremfb_ctl_skip_encode(SremfbClient *c)
{
    if (c->delay_ewma_us <= DELAY_HI_US)
        return FALSE;
    return g_get_monotonic_time() - c->last_enc_us <
           (gint64)(G_USEC_PER_SEC / FPS_MIN);
}

/* Opening bitrate for a new H.264 episode: a bit under the measured link
 * capacity when we have one, a safe middle ground otherwise. */
int sremfb_ctl_initial_kbps(const SremfbClient *c)
{
    if (c->capacity_Bps > 0.0)
        return (int)CLAMP(0.8 * c->capacity_Bps * 8.0 / 1000.0,
                          KBPS_MIN, KBPS_MAX);
    return KBPS_START;
}

void sremfb_ctl_on_pong(SremfbClient *c, uint64_t t_echo_us)
{
    gint64 now = g_get_monotonic_time();
    double delay = (double)(now - (gint64)t_echo_us);

    if (delay < 0 || delay > 60.0 * G_USEC_PER_SEC)
        return;                                /* garbage echo */
    c->last_pong_us = now;
    if (c->pings_outstanding)
        c->pings_outstanding--;
    c->delay_ewma_us = EWMA(c->delay_ewma_us, delay, 0.3);
    ctl_aimd(c);
    ctl_evaluate(c);
}

/* Called after every successful grab+send, in any mode. raw_bytes is the
 * pre-compression size of the damage, wire_bytes what actually left. */
void sremfb_ctl_on_grab(SremfbClient *c, size_t raw_bytes, size_t wire_bytes)
{
    gint64 now = g_get_monotonic_time();

    if (c->last_grab_us) {
        double gap = (double)(now - c->last_grab_us);
        if (gap > 0)
            c->grab_fps = EWMA(c->grab_fps, G_USEC_PER_SEC / gap, 0.2);
        if (gap >= DAMAGE_GAP_US)
            c->damage_cont_us = now;           /* streak restarts */
    } else {
        c->damage_cont_us = now;
    }
    c->last_grab_us = now;

    /* damage byte rate (what RAW would have to move), EWMA over ~2 s */
    if (c->cap_win_start_us == 0)
        c->cap_win_start_us = now;
    c->cap_win_bytes += wire_bytes;
    c->raw_win_bytes += raw_bytes;
    gint64 win = now - c->cap_win_start_us;
    if (win >= CAP_WINDOW_US) {
        double wire_rate = (double)c->cap_win_bytes * G_USEC_PER_SEC / win;
        double raw_rate = (double)c->raw_win_bytes * G_USEC_PER_SEC / win;
        c->raw_rate_Bps = EWMA(c->raw_rate_Bps, raw_rate, 0.3);
        /* saturated link => achieved rate ~= capacity */
        if (c->delay_ewma_us > DELAY_HI_US && wire_rate > 0)
            c->capacity_Bps = EWMA(c->capacity_Bps, wire_rate, 0.3);
        if (c->xmit_mode == SREMFB_XMIT_RAW && c->cap_win_bytes > 0)
            c->lz4_ratio_ewma = EWMA(c->lz4_ratio_ewma,
                                     raw_rate / wire_rate, 0.2);
        c->cap_win_start_us = now;
        c->cap_win_bytes = 0;
        c->raw_win_bytes = 0;
    }

    ctl_evaluate(c);
}

static gboolean ping_tick(gpointer data)
{
    SremfbClient *c = data;
    gint64 now = g_get_monotonic_time();

    if (c->fd < 0 || c->state != SREMFB_CLIENT_STREAMING || c->lost_id)
        return G_SOURCE_CONTINUE;

    /* liveness watchdog: pings in flight but no echo of any kind for
     * PONG_DEAD_US — the peer (or the path) is gone. Kills the virtual
     * monitor in ~6 s instead of waiting for the TCP keepalive. */
    if (c->pings_outstanding > 0 && now - c->last_pong_us > PONG_DEAD_US) {
        g_message("[%s] no echo for %d s, dropping", c->macstr,
                  (int)(PONG_DEAD_US / G_USEC_PER_SEC));
        sremfb_schedule_client_lost(c);
        return G_SOURCE_CONTINUE;
    }

    /* silent link (static screen): nothing to measure — let the delay
     * and damage-rate estimates decay (an idle link is by definition
     * uncongested), and drop to a slow heartbeat so both ends can still
     * tell a quiet peer from a dead one. */
    if (c->wire_total == c->ping_wire_mark) {
        c->delay_ewma_us = EWMA(c->delay_ewma_us, 0.0, 0.1);
        c->raw_rate_Bps = EWMA(c->raw_rate_Bps, 0.0, 0.1);
        ctl_evaluate(c);
        if (now - c->last_ping_us < PING_IDLE_US ||
            c->pings_outstanding >= PINGS_MAX_FLIGHT)
            return G_SOURCE_CONTINUE;
        send_ping(c);                    /* heartbeat, ~28 B every 2 s */
        return G_SOURCE_CONTINUE;
    }
    if (c->pings_outstanding >= PINGS_MAX_FLIGHT)
        return G_SOURCE_CONTINUE;

    send_ping(c);
    return G_SOURCE_CONTINUE;
}

void sremfb_ctl_start(SremfbClient *c)
{
    c->xmit_mode = SREMFB_XMIT_RAW;
    ctl_set_state(c, SREMFB_CTL_RAW_STEADY);
    c->delay_ewma_us = 0.0;
    c->grab_fps = 0.0;
    c->last_grab_us = 0;
    c->last_ping_us = 0;
    c->last_pong_us = g_get_monotonic_time();
    c->pings_outstanding = 0;
    c->wire_total = 0;
    c->ping_wire_mark = 0;
    c->cap_win_start_us = 0;
    c->cap_win_bytes = 0;
    c->raw_win_bytes = 0;

    if (c->feedback && !c->ping_timer_id)
        c->ping_timer_id = g_timeout_add(PING_INTERVAL_MS, ping_tick, c);
}

void sremfb_ctl_stop(SremfbClient *c)
{
    g_clear_handle_id(&c->ping_timer_id, g_source_remove);
    sremfb_enc_close(c->enc);
    c->enc = NULL;
    g_clear_pointer(&c->yuvbuf, g_free);
    c->xmit_mode = SREMFB_XMIT_RAW;
}
