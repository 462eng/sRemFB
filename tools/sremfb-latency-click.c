/*
 * sremfb-latency-click — the sound half of `sremfb-view --latency-test`.
 *
 * A minimal low-latency PipeWire player (128-frame quantum) on the
 * server's default output: silence, and a 2 ms click for every byte read
 * on stdin. sremfb-latency-probe --click starts it and writes one byte
 * each time its window flips, so the viewer can time the sound of an
 * input event against its picture, on the viewer's clock only.
 *
 * The click is a burst of anti-phase stereo (left positive, right
 * negative), a shape the viewer looks for and that ordinary game or
 * desktop sound does not produce.
 *
 *   sremfb-latency-click [--amplitude N]      (default 5000 of 32767)
 */
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#define RATE          48000
#define CLICK_FRAMES  96               /* 2 ms */

static struct {
    struct pw_main_loop *loop;
    struct pw_stream *stream;
    atomic_int pending;
    int left;                          /* data thread only */
    int16_t amp;
} P = { .amp = 5000 };

static void on_process(void *data)
{
    struct pw_buffer *b = pw_stream_dequeue_buffer(P.stream);

    if (!b)
        return;
    struct spa_data *d = &b->buffer->datas[0];
    int16_t *s = d->data;
    uint32_t frames = d->maxsize / 4;

    if (!s) {
        pw_stream_queue_buffer(P.stream, b);
        return;
    }
    if (b->requested && b->requested < frames)
        frames = (uint32_t)b->requested;
    for (uint32_t i = 0; i < frames; i++) {
        if (P.left == 0 && atomic_load_explicit(&P.pending,
                                                memory_order_relaxed) > 0) {
            atomic_fetch_sub(&P.pending, 1);
            P.left = CLICK_FRAMES;
        }
        if (P.left > 0) {
            s[2 * i] = P.amp;
            s[2 * i + 1] = (int16_t)-P.amp;
            P.left--;
        } else {
            s[2 * i] = s[2 * i + 1] = 0;
        }
    }
    d->chunk->offset = 0;
    d->chunk->stride = 4;
    d->chunk->size = frames * 4;
    pw_stream_queue_buffer(P.stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

static void on_stdin(void *data, int fd, uint32_t mask)
{
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf));

    if (n > 0) {
        atomic_fetch_add(&P.pending, (int)n);
        return;
    }
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return;
    pw_main_loop_quit(P.loop);         /* EOF: the probe is gone */
}

static void on_signal(void *data, int sig)
{
    pw_main_loop_quit(P.loop);
}

int main(int argc, char **argv)
{
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--amplitude") == 0 && i + 1 < argc) {
            int a = atoi(argv[++i]);
            if (a < 1 || a > 32767) {
                fprintf(stderr, "--amplitude 1..32767\n");
                return 2;
            }
            P.amp = (int16_t)a;
        } else {
            fprintf(stderr, "usage: %s [--amplitude N] (one click per "
                    "byte on stdin)\n", argv[0]);
            return 2;
        }
    }

    pw_init(&argc, &argv);
    P.loop = pw_main_loop_new(NULL);
    if (!P.loop) {
        fprintf(stderr, "sremfb-latency-click: no PipeWire loop\n");
        return 1;
    }
    pw_loop_add_signal(pw_main_loop_get_loop(P.loop), SIGINT, on_signal, NULL);
    pw_loop_add_signal(pw_main_loop_get_loop(P.loop), SIGTERM, on_signal,
                       NULL);
    pw_loop_add_io(pw_main_loop_get_loop(P.loop), STDIN_FILENO,
                   SPA_IO_IN | SPA_IO_HUP, false, on_stdin, NULL);

    P.stream = pw_stream_new_simple(pw_main_loop_get_loop(P.loop),
        "sremfb-latency-click", pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Game",
            PW_KEY_NODE_LATENCY, "128/48000",
            PW_KEY_APP_NAME, "sremfb-latency-click",
            NULL),
        &stream_events, NULL);
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_S16_LE,
                                 .rate = RATE, .channels = 2,
                                 .position = { SPA_AUDIO_CHANNEL_FL,
                                               SPA_AUDIO_CHANNEL_FR }));
    if (!P.stream ||
        pw_stream_connect(P.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS, params, 1) < 0) {
        fprintf(stderr, "sremfb-latency-click: cannot play\n");
        return 1;
    }
    pw_main_loop_run(P.loop);
    pw_stream_destroy(P.stream);
    pw_main_loop_destroy(P.loop);
    pw_deinit();
    return 0;
}
