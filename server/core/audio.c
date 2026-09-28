/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * Audio: what the server's desktop plays, streamed to the client that asked
 * for it (SREMFB_HELLO_FLAG_AUDIO) as raw PCM over UDP — no codec, no
 * buffering on this side beyond one PipeWire quantum.
 *
 * Per audio client, a PipeWire stream with media.class Audio/Sink: to the
 * desktop it is an ordinary output device ("sRemFB <model>"), and it
 * becomes the default output while the client is connected — every
 * application that follows the default (games, Steam, the desktop) moves
 * onto it, and the previous default comes back when the client leaves
 * (unless the user picked another output in the meantime). The graph
 * converts to S16LE 48 kHz stereo for us; each process cycle (the stream
 * asks for 128 frames = 2.7 ms) is sent right away from PipeWire's data
 * thread, split into packets of at most 240 frames.
 *
 * Nothing here runs on the glib main loop except the UDP hello handling:
 * the PipeWire side lives in its own pw_thread_loop, and the data thread
 * only does a non-blocking sendto() per packet, so neither the video path
 * nor input injection can be delayed by audio.
 *
 * UDP flow (see protocol.h): the client sends AUDIO_HELLO datagrams from
 * the socket it receives on, quoting the token of its server hello; the
 * server checks the source IP against the client's TCP peer, answers each
 * one with an ECHO (clock offset for the client's latency estimate) and
 * sends the audio there. No hello for 5 s = the client stopped listening:
 * packets stop too (the TCP session is what decides the client is gone).
 *
 * Config: SREMFB_AUDIO=0 disables it (on by default: only clients that
 * ask for audio get an output, and they already pass SREMFB_ALLOW).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <glib-unix.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/json.h>

#include "core.h"

#define AUDIO_QUANTUM      "128/48000"
#define HELLO_TIMEOUT_US   (5 * G_USEC_PER_SEC)
#define NODE_PREFIX        "sremfb."
#define DEFAULT_KEY        "default.configured.audio.sink"
#define SAVED_FILE         "sremfb-audio-default"

struct SremfbAudio {
    SremfbClient *c;                   /* main thread only */
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    char node_name[48];
    char desc[48];
    uint16_t token;
    gboolean made_default;             /* we pointed the default at it */
    gboolean announced;

    /* read by the data thread */
    struct sockaddr_storage dest;
    socklen_t destlen;
    atomic_int have_dest;
    atomic_llong last_hello_us;
    uint32_t seq;                      /* data thread only */
    atomic_ullong packets, bytes;
};

static struct {
    gboolean enabled;
    int udp_fd;
    guint udp_watch;

    struct pw_thread_loop *tl;
    struct pw_context *ctx;
    struct pw_core *core;
    struct spa_hook core_listener;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    struct pw_metadata *md;            /* the "default" metadata */
    struct spa_hook md_listener;
    gboolean broken;                   /* core error: reconnect next time */
    int sync_seq, done_seq;            /* shutdown flush */

    /* everything below: under the thread-loop lock */
    char *cur_default;                 /* JSON value, NULL = unset */
    gboolean saved;                    /* orig_default holds the previous */
    char *orig_default;
    GPtrArray *live;                   /* SremfbAudio* with a stream */
} A = { .udp_fd = -1 };

static gint64 mono_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (gint64)ts.tv_sec * G_USEC_PER_SEC + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------------ data path */

static void send_frames(struct SremfbAudio *a, const uint8_t *pcm,
                        uint32_t frames, gint64 t0_us)
{
    uint8_t pkt[sizeof(struct sremfb_audio_hdr) +
                SREMFB_AUDIO_MAX_FRAMES * 4];
    struct sremfb_audio_hdr *h = (struct sremfb_audio_hdr *)pkt;
    uint32_t off = 0;

    while (off < frames) {
        uint32_t n = MIN(frames - off, (uint32_t)SREMFB_AUDIO_MAX_FRAMES);
        size_t len = sizeof(*h) + (size_t)n * 4;

        memset(h, 0, sizeof(*h));
        h->magic = SREMFB_MAGIC;
        h->type = SREMFB_UDP_AUDIO;
        h->format = SREMFB_AUDIO_S16LE_48K_STEREO;
        h->frames = (uint16_t)n;
        h->seq = a->seq++;
        h->t_us = (uint64_t)(t0_us + (gint64)off * G_USEC_PER_SEC /
                             SREMFB_AUDIO_RATE);
        memcpy(pkt + sizeof(*h), pcm + (size_t)off * 4, (size_t)n * 4);
        if (sendto(A.udp_fd, pkt, len, MSG_DONTWAIT | MSG_NOSIGNAL,
                   (struct sockaddr *)&a->dest, a->destlen) > 0) {
            atomic_fetch_add_explicit(&a->packets, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&a->bytes, len, memory_order_relaxed);
        }
        off += n;
    }
}

/* PipeWire data thread (RT): one quantum in, packets out. */
static void on_process(void *data)
{
    struct SremfbAudio *a = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(a->stream);
    gint64 now = mono_us();

    if (!b)
        return;
    struct spa_data *d = &b->buffer->datas[0];
    if (d->data && d->chunk &&
        atomic_load_explicit(&a->have_dest, memory_order_acquire) &&
        now - atomic_load_explicit(&a->last_hello_us, memory_order_relaxed) <
            HELLO_TIMEOUT_US) {
        uint32_t off = d->chunk->offset % d->maxsize;
        uint32_t size = MIN(d->chunk->size, d->maxsize - off);
        send_frames(a, (const uint8_t *)d->data + off, size / 4, now);
    }
    pw_stream_queue_buffer(a->stream, b);
}

/* ---------------------------------------------------- default output */

/* Thread-loop lock held. value NULL = remove the key. */
static void default_set(const char *value)
{
    if (!A.md)
        return;
    pw_metadata_set_property(A.md, 0, DEFAULT_KEY,
                             value ? "Spa:String:JSON" : NULL, value);
}

static gchar *saved_path(void)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");

    return g_build_filename(rundir && *rundir ? rundir : "/tmp",
                            SAVED_FILE, NULL);
}

/* The default we replaced survives a crash of ours: the next start puts
 * it back if the configured default still names one of our (gone)
 * outputs. "-" = there was none. */
static void saved_write(const char *value)
{
    gchar *p = saved_path();

    if (value == (const char *)1)
        unlink(p);
    else
        g_file_set_contents(p, value ? value : "-", -1, NULL);
    g_free(p);
}

static gboolean names_node(const char *json, const char *node)
{
    char want[80];

    if (!json || !node)
        return FALSE;
    g_snprintf(want, sizeof(want), "\"%s\"", node);
    return strstr(json, want) != NULL;
}

/* Thread-loop lock held: our output exists, make it the default. */
static void audio_take_default(struct SremfbAudio *a)
{
    char json[96];

    if (!A.md)
        return;
    if (!A.saved) {
        /* first of ours: remember what the desktop had (never one of
         * our own outputs, e.g. after a crash) */
        g_free(A.orig_default);
        A.orig_default = (A.cur_default &&
                          !strstr(A.cur_default, "\"" NODE_PREFIX)) ?
                         g_strdup(A.cur_default) : NULL;
        A.saved = TRUE;
        saved_write(A.orig_default);
    }
    g_snprintf(json, sizeof(json), "{ \"name\": \"%s\" }", a->node_name);
    default_set(json);
    a->made_default = TRUE;
}

/* Thread-loop lock held: a's output is going away. */
static void audio_give_back_default(struct SremfbAudio *a)
{
    if (!a->made_default || !A.md)
        return;
    a->made_default = FALSE;
    if (A.cur_default && !names_node(A.cur_default, a->node_name))
        return;                        /* the user chose another output */

    /* another client of ours still listening: hand it the default */
    for (guint i = 0; i < A.live->len; i++) {
        struct SremfbAudio *o = g_ptr_array_index(A.live, i);
        if (o != a && o->stream) {
            audio_take_default(o);
            return;
        }
    }
    default_set(A.orig_default);
    g_message("audio: default output restored (%s)",
              A.orig_default ? A.orig_default : "none configured");
    g_clear_pointer(&A.orig_default, g_free);
    A.saved = FALSE;
    saved_write((const char *)1);
}

/* Metadata events arrive in the thread loop (lock held). */
static int on_md_property(void *data, uint32_t subject, const char *key,
                          const char *type, const char *value)
{
    if (subject != 0 || (key && strcmp(key, DEFAULT_KEY) != 0))
        return 0;
    g_free(A.cur_default);
    A.cur_default = key ? g_strdup(value) : NULL;

    /* crash recovery: we are not streaming, yet the default is still
     * one of our outputs — put back what was saved */
    if (A.live->len == 0 && A.cur_default &&
        strstr(A.cur_default, "\"" NODE_PREFIX)) {
        gchar *p = saved_path(), *v = NULL;
        if (g_file_get_contents(p, &v, NULL, NULL)) {
            g_message("audio: stale default output %s, restoring %s",
                      A.cur_default, v);
            default_set(strcmp(v, "-") == 0 ? NULL : v);
            unlink(p);
        }
        g_free(v);
        g_free(p);
    }
    return 0;
}

static const struct pw_metadata_events md_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_md_property,
};

static void on_global(void *data, uint32_t id, uint32_t permissions,
                      const char *type, uint32_t version,
                      const struct spa_dict *props)
{
    const char *name;

    if (A.md || strcmp(type, PW_TYPE_INTERFACE_Metadata) != 0 || !props)
        return;
    name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
    if (!name || strcmp(name, "default") != 0)
        return;
    A.md = pw_registry_bind(A.registry, id, type, PW_VERSION_METADATA, 0);
    if (A.md)
        pw_metadata_add_listener(A.md, &A.md_listener, &md_events, NULL);
}

static void on_global_remove(void *data, uint32_t id)
{
    if (A.md && pw_proxy_get_bound_id((struct pw_proxy *)A.md) == id) {
        spa_hook_remove(&A.md_listener);
        pw_proxy_destroy((struct pw_proxy *)A.md);
        A.md = NULL;
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_global,
    .global_remove = on_global_remove,
};

static void on_core_error(void *data, uint32_t id, int seq, int res,
                          const char *message)
{
    if (id == PW_ID_CORE && res == -EPIPE) {
        g_warning("audio: PipeWire connection lost (%s)", message);
        A.broken = TRUE;
    }
}

static void on_core_done(void *data, uint32_t id, int seq)
{
    if (id == PW_ID_CORE) {
        A.done_seq = seq;
        pw_thread_loop_signal(A.tl, false);
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
    .error = on_core_error,
};

/* --------------------------------------------------------- per stream */

static void on_state(void *data, enum pw_stream_state old,
                     enum pw_stream_state state, const char *error)
{
    struct SremfbAudio *a = data;

    if (state == PW_STREAM_STATE_ERROR) {
        g_warning("audio: output %s failed: %s", a->node_name,
                  error ? error : "?");
        return;
    }
    /* registered with the graph: a default can now resolve to it */
    if (!a->announced && (state == PW_STREAM_STATE_PAUSED ||
                          state == PW_STREAM_STATE_STREAMING)) {
        a->announced = TRUE;
        audio_take_default(a);
        g_message("audio: output \"%s\" (%s) is the default", a->desc,
                  a->node_name);
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state,
    .process = on_process,
};

/* ----------------------------------------------------------- UDP side */

static struct SremfbAudio *find_by_token(uint16_t token)
{
    for (guint i = 0; i < A.live->len; i++) {
        struct SremfbAudio *a = g_ptr_array_index(A.live, i);
        if (a->token == token)
            return a;
    }
    return NULL;
}

/* "a.b.c.d" of a (possibly v4-mapped) UDP source, as in c->peer. */
static void addr_ip(const struct sockaddr_storage *ss, char *out, size_t len)
{
    g_strlcpy(out, "?", len);
    if (ss->ss_family == AF_INET) {
        inet_ntop(AF_INET, &((const struct sockaddr_in *)ss)->sin_addr,
                  out, (socklen_t)len);
    } else if (ss->ss_family == AF_INET6) {
        const struct in6_addr *a6 =
            &((const struct sockaddr_in6 *)ss)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(a6)) {
            struct in_addr v4;
            memcpy(&v4, a6->s6_addr + 12, 4);
            inet_ntop(AF_INET, &v4, out, (socklen_t)len);
        } else {
            inet_ntop(AF_INET6, a6, out, (socklen_t)len);
        }
    }
}

static gboolean on_udp(gint fd, GIOCondition cond, gpointer data)
{
    for (;;) {
        struct sremfb_udp_hello h;
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        ssize_t n = recvfrom(fd, &h, sizeof(h), MSG_DONTWAIT,
                             (struct sockaddr *)&ss, &sl);
        gint64 now = mono_us();

        if (n < 0)
            break;
        if ((size_t)n != sizeof(h) || h.magic != SREMFB_MAGIC ||
            h.type != SREMFB_UDP_AUDIO_HELLO)
            continue;

        pw_thread_loop_lock(A.tl);
        struct SremfbAudio *a = find_by_token(h.token);
        pw_thread_loop_unlock(A.tl);
        if (!a)
            continue;

        char ip[INET6_ADDRSTRLEN], peer_ip[64];
        char *colon;
        addr_ip(&ss, ip, sizeof(ip));
        g_strlcpy(peer_ip, a->c->peer, sizeof(peer_ip));
        colon = strrchr(peer_ip, ':');
        if (colon)
            *colon = '\0';
        if (strcmp(ip, peer_ip) != 0)
            continue;                  /* token from somewhere else */

        if (!atomic_load(&a->have_dest)) {
            /* written once, before have_dest publishes it */
            memcpy(&a->dest, &ss, sl);
            a->destlen = sl;
            atomic_store_explicit(&a->have_dest, 1, memory_order_release);
            g_message("[%s] audio: sending to %s port %u", a->c->macstr, ip,
                      ntohs(ss.ss_family == AF_INET ?
                            ((struct sockaddr_in *)&ss)->sin_port :
                            ((struct sockaddr_in6 *)&ss)->sin6_port));
        }
        atomic_store_explicit(&a->last_hello_us, now, memory_order_relaxed);

        struct sremfb_udp_echo e = {
            .magic = SREMFB_MAGIC,
            .type = SREMFB_UDP_ECHO,
            .t_client_us = h.t_client_us,
            .t_server_us = (uint64_t)now,
        };
        sendto(fd, &e, sizeof(e), MSG_DONTWAIT | MSG_NOSIGNAL,
               (struct sockaddr *)&ss, sl);
    }
    return G_SOURCE_CONTINUE;
}

static int udp_listen(uint16_t port)
{
    int fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int zero = 0;
    struct sockaddr_in6 a6 = { .sin6_family = AF_INET6,
                               .sin6_port = htons(port),
                               .sin6_addr = IN6ADDR_ANY_INIT };

    if (fd >= 0) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        if (bind(fd, (struct sockaddr *)&a6, sizeof(a6)) == 0)
            goto ok;
        close(fd);
    }
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in a4 = { .sin_family = AF_INET,
                              .sin_port = htons(port),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&a4, sizeof(a4)) < 0) {
        close(fd);
        return -1;
    }
ok:;
    /* audio is tiny, but interactive: keep it ahead of bulk traffic */
    int tos = 0xb8;                    /* DSCP EF */
    setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof(tos));
    return fd;
}

/* ------------------------------------------------------- connection */

static void pw_disconnect(void)
{
    if (A.md) {
        spa_hook_remove(&A.md_listener);
        pw_proxy_destroy((struct pw_proxy *)A.md);
        A.md = NULL;
    }
    if (A.registry) {
        spa_hook_remove(&A.registry_listener);
        pw_proxy_destroy((struct pw_proxy *)A.registry);
        A.registry = NULL;
    }
    if (A.core) {
        spa_hook_remove(&A.core_listener);
        pw_core_disconnect(A.core);
        A.core = NULL;
    }
    g_clear_pointer(&A.cur_default, g_free);
    A.broken = FALSE;
}

/* Thread-loop lock held. */
static gboolean pw_connect(void)
{
    if (A.core && !A.broken)
        return TRUE;
    if (A.core)
        pw_disconnect();
    A.core = pw_context_connect(A.ctx, NULL, 0);
    if (!A.core) {
        g_warning("audio: cannot connect to PipeWire: %s", g_strerror(errno));
        return FALSE;
    }
    pw_core_add_listener(A.core, &A.core_listener, &core_events, NULL);
    A.registry = pw_core_get_registry(A.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(A.registry, &A.registry_listener,
                             &registry_events, NULL);
    return TRUE;
}

void sremfb_audio_init(SremfbServer *srv)
{
    const char *env = getenv("SREMFB_AUDIO");

    if (env && strcmp(env, "0") == 0) {
        g_message("audio disabled (SREMFB_AUDIO=0)");
        return;
    }
    A.udp_fd = udp_listen(srv->port);
    if (A.udp_fd < 0) {
        g_warning("audio disabled: cannot bind UDP port %u: %s", srv->port,
                  g_strerror(errno));
        return;
    }
    pw_init(NULL, NULL);
    A.tl = pw_thread_loop_new("sremfb-audio", NULL);
    A.ctx = A.tl ? pw_context_new(pw_thread_loop_get_loop(A.tl), NULL, 0)
                 : NULL;
    if (!A.ctx || pw_thread_loop_start(A.tl) < 0) {
        g_warning("audio disabled: cannot start the PipeWire loop");
        close(A.udp_fd);
        A.udp_fd = -1;
        return;
    }
    A.live = g_ptr_array_new();
    pw_thread_loop_lock(A.tl);
    pw_connect();                      /* early: stale-default recovery */
    pw_thread_loop_unlock(A.tl);
    A.udp_watch = g_unix_fd_add(A.udp_fd, G_IO_IN, on_udp, NULL);
    A.enabled = TRUE;
    g_message("audio enabled: clients that ask get a PipeWire output "
              "(UDP port %u; SREMFB_AUDIO=0 to disable)", srv->port);
}

uint8_t sremfb_audio_start(SremfbClient *c)
{
    struct SremfbAudio *a;
    uint8_t buf[1024];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    char model[14];

    if (!c->audio_cap || !A.enabled || c->audio)
        return 0;

    a = g_new0(struct SremfbAudio, 1);
    a->c = c;
    g_snprintf(a->node_name, sizeof(a->node_name), NODE_PREFIX "%02x%02x%02x"
               "%02x%02x%02x", c->hello.mac[0], c->hello.mac[1],
               c->hello.mac[2], c->hello.mac[3], c->hello.mac[4],
               c->hello.mac[5]);
    memcpy(model, c->hello.model, 13);
    model[13] = '\0';
    g_strstrip(model);
    g_snprintf(a->desc, sizeof(a->desc), "sRemFB %s",
               model[0] ? model : c->macstr);

    pw_thread_loop_lock(A.tl);
    do {
        a->token = (uint16_t)g_random_int_range(1, 65536);
    } while (find_by_token(a->token));
    if (!pw_connect()) {
        pw_thread_loop_unlock(A.tl);
        g_free(a);
        return 0;
    }
    a->stream = pw_stream_new(A.core, a->desc, pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CLASS, "Audio/Sink",
        PW_KEY_NODE_NAME, a->node_name,
        PW_KEY_NODE_DESCRIPTION, a->desc,
        PW_KEY_NODE_LATENCY, AUDIO_QUANTUM,
        PW_KEY_NODE_VIRTUAL, "true",
        "node.pause-on-idle", "false",
        NULL));
    if (!a->stream) {
        pw_thread_loop_unlock(A.tl);
        g_warning("[%s] audio: cannot create the output", c->macstr);
        g_free(a);
        return 0;
    }
    pw_stream_add_listener(a->stream, &a->stream_listener, &stream_events, a);
    params[0] = spa_format_audio_raw_build(&bld, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_S16_LE,
                                 .rate = SREMFB_AUDIO_RATE,
                                 .channels = SREMFB_AUDIO_CHANNELS,
                                 .position = { SPA_AUDIO_CHANNEL_FL,
                                               SPA_AUDIO_CHANNEL_FR }));
    if (pw_stream_connect(a->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS, params, 1) < 0) {
        spa_hook_remove(&a->stream_listener);
        pw_stream_destroy(a->stream);
        pw_thread_loop_unlock(A.tl);
        g_warning("[%s] audio: cannot connect the output", c->macstr);
        g_free(a);
        return 0;
    }
    g_ptr_array_add(A.live, a);
    pw_thread_loop_unlock(A.tl);

    c->audio = a;
    c->audio_token = a->token;
    g_message("[%s] audio ON: output \"%s\"", c->macstr, a->desc);
    return SREMFB_SRV_FLAG_AUDIO;
}

void sremfb_audio_stop(SremfbClient *c)
{
    struct SremfbAudio *a = c->audio;

    if (!a)
        return;
    c->audio = NULL;
    pw_thread_loop_lock(A.tl);
    g_ptr_array_remove(A.live, a);
    audio_give_back_default(a);
    spa_hook_remove(&a->stream_listener);
    pw_stream_destroy(a->stream);      /* the data thread is done with it */
    a->stream = NULL;
    pw_thread_loop_unlock(A.tl);
    g_message("[%s] audio off (%llu packets, %.1f MB sent)", c->macstr,
              (unsigned long long)atomic_load(&a->packets),
              (double)atomic_load(&a->bytes) / 1e6);
    g_free(a);
}

void sremfb_audio_shutdown(void)
{
    if (!A.enabled)
        return;
    g_clear_handle_id(&A.udp_watch, g_source_remove);
    pw_thread_loop_lock(A.tl);
    if (A.core && !A.broken) {
        /* let the default-output restore reach the server first */
        A.sync_seq = pw_core_sync(A.core, PW_ID_CORE, 0);
        for (int i = 0; i < 20 && A.done_seq != A.sync_seq; i++)
            pw_thread_loop_timed_wait(A.tl, 1);
    }
    pw_disconnect();
    pw_thread_loop_unlock(A.tl);
    pw_thread_loop_stop(A.tl);
    pw_context_destroy(A.ctx);
    pw_thread_loop_destroy(A.tl);
    g_ptr_array_free(A.live, TRUE);
    close(A.udp_fd);
    A.udp_fd = -1;
    A.enabled = FALSE;
}
