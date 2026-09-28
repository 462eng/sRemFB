/*
 * sremfb-view — windowed sRemFB viewer for Linux desktops (SDL3, native
 * Wayland). Plugs a virtual monitor into a remote sremfb-server exactly
 * like an SBC client would, and shows it in a resizable window.
 *
 * Built for latency first:
 *   - a network thread (net.c) receives and decodes (LZ4) as soon as
 *     bytes arrive and echoes the server's PINGs;
 *   - the main thread uploads only the damaged rects to a streaming
 *     texture and presents the newest picture — there is no frame queue
 *     anywhere, a late frame is merged into the next upload;
 *   - vsync is off unless --vsync;
 *   - no H.264 capability is advertised: the stream stays on independent
 *     damage rects (RAW/LZ4).
 *
 * The event loop is the one that will capture keyboard/mouse/gamepad
 * input (input_event() below), the wire pixel format stays abstract in
 * struct view_fb, and new per-rect encodings plug into net.c's decode
 * switch without touching the display side.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "view.h"

#define ESC_HOLD_MS     1000        /* hold Escape this long to quit */
#define TITLE_EVERY_MS  1000
#define STATS_EVERY_MS  5000

#ifndef SREMFB_VIEW_VERSION
#define SREMFB_VIEW_VERSION "dev"
#endif

static struct {
    struct view_opts o;
    int vsync;
    int fullscreen;
    int nearest;
    int stats;
    const char *renderer;
    unsigned dump_n, dump_every;
    const char *dump_dir;
} O;

static struct view_fb FB = { .lock = PTHREAD_MUTEX_INITIALIZER };
static struct view_stats ST;

static Uint32 wake_type;
static atomic_int wake_pending;

void view_log(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "sremfb-view: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Network thread -> main thread: one pending SDL event at most. */
static void wake_main(void)
{
    if (!atomic_exchange(&wake_pending, 1)) {
        SDL_Event e;
        SDL_zero(e);
        e.type = wake_type;
        if (!SDL_PushEvent(&e))
            atomic_store(&wake_pending, 0);
    }
}

/* ------------------------------------------------------ identity */

static int parse_mac(const char *s, uint8_t mac[6])
{
    unsigned v[6];
    char tail;

    if (sscanf(s, "%x:%x:%x:%x:%x:%x%c", &v[0], &v[1], &v[2], &v[3],
               &v[4], &v[5], &tail) != 6)
        return -1;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF)
            return -1;
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}

/* Stable pseudo-MAC: FNV-1a 64 of an application salt + /etc/machine-id,
 * forced to a locally administered unicast address — never a real NIC
 * MAC, so it can't collide with an SBC client, and 48 bits of a salted
 * hash don't give the machine-id away. Same machine => same virtual
 * monitor (EDID serial) => GNOME restores its position. */
static void derive_mac(uint8_t mac[6])
{
    char id[128] = "";
    FILE *f = fopen("/etc/machine-id", "r");
    if (f) {
        if (!fgets(id, sizeof(id), f))
            id[0] = '\0';
        fclose(f);
    }
    id[strcspn(id, "\r\n")] = '\0';
    if (!id[0]) {
        view_log("no /etc/machine-id, deriving the identity from the "
                 "hostname");
        if (gethostname(id, sizeof(id) - 1) < 0)
            snprintf(id, sizeof(id), "sremfb-view");
    }

    uint64_t h = 0xcbf29ce484222325ull;
    const char *salt = "sremfb-view/";
    for (const char *p = salt; *p; p++)
        h = (h ^ (uint8_t)*p) * 0x100000001b3ull;
    for (const char *p = id; *p; p++)
        h = (h ^ (uint8_t)*p) * 0x100000001b3ull;
    for (int i = 0; i < 6; i++)
        mac[i] = (uint8_t)(h >> (8 * i));
    mac[0] = (uint8_t)((mac[0] & 0xFC) | 0x02);   /* local, unicast */
}

/* Copies up to 13 printable chars (wire format: NUL only when shorter). */
static void model_copy(char model[13], const char *s)
{
    int n = 0;

    memset(model, 0, 13);
    while (n < 13 && s[n] >= 0x20 && s[n] <= 0x7E)
        n++;
    while (n > 0 && s[n - 1] == ' ')
        n--;
    memcpy(model, s, (size_t)n);
}

/* ---------------------------------------------------------- dump */

static void dump_ppm(const uint8_t *px, unsigned w, unsigned h,
                     uint8_t pixfmt, unsigned idx)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/sremfb-view-%04u.ppm", O.dump_dir, idx);
    FILE *f = fopen(path, "wb");
    if (!f) {
        view_log("cannot write %s: %s", path, strerror(errno));
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    uint8_t *row = malloc((size_t)w * 3);
    for (unsigned y = 0; row && y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            uint8_t *o = row + (size_t)x * 3;
            if (pixfmt == SREMFB_PIX_RGB565) {
                uint16_t v = ((const uint16_t *)px)[(size_t)y * w + x];
                o[0] = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
                o[1] = (uint8_t)(((v >> 5) & 0x3F) * 255 / 63);
                o[2] = (uint8_t)((v & 0x1F) * 255 / 31);
            } else {
                const uint8_t *p = px + ((size_t)y * w + x) * 4; /* B,G,R,X */
                o[0] = p[2];
                o[1] = p[1];
                o[2] = p[0];
            }
        }
        fwrite(row, 1, (size_t)w * 3, f);
    }
    free(row);
    fclose(f);
    view_log("wrote %s", path);
}

/* ------------------------------------------------------- display */

static struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    unsigned gen;              /* FB.gen the texture was built for */
    unsigned tw, th;
    int connected, blanked;
    unsigned last_batches;     /* for the dump: new pixels since */
    unsigned presented_new;    /* presents that carried new pixels */
    unsigned dumped;
    uint8_t *dumpbuf;
} D = { .gen = ~0u };

static SDL_PixelFormat view_sdl_format(uint8_t pixfmt)
{
    /* both match the wire's little-endian memory layout */
    return pixfmt == SREMFB_PIX_RGB565 ? SDL_PIXELFORMAT_RGB565
                                       : SDL_PIXELFORMAT_XRGB8888;
}

/* Called with FB.lock held: (re)build the texture for a new stream. */
static int texture_rebuild(void)
{
    if (D.tex) {
        SDL_DestroyTexture(D.tex);
        D.tex = NULL;
    }
    D.tw = D.th = 0;
    if (!FB.connected)
        return 0;
    D.tex = SDL_CreateTexture(D.ren, view_sdl_format(FB.pixfmt),
                              SDL_TEXTUREACCESS_STREAMING,
                              (int)FB.w, (int)FB.h);
    if (!D.tex) {
        view_log("cannot create a %ux%u texture: %s", FB.w, FB.h,
                 SDL_GetError());
        return -1;
    }
    SDL_SetTextureScaleMode(D.tex, O.nearest ? SDL_SCALEMODE_NEAREST
                                             : SDL_SCALEMODE_LINEAR);
    D.tw = FB.w;
    D.th = FB.h;
    /* keep the ratio, black bars around, whatever the window size */
    SDL_SetRenderLogicalPresentation(D.ren, (int)FB.w, (int)FB.h,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    return 0;
}

/* Pull what the network thread published, upload the damage, present.
 * `pull` = 0 only repaints (expose/resize). */
static void refresh(int pull)
{
    uint64_t t0 = now_ns();
    int dump_now = 0;
    unsigned dw = 0, dh = 0;
    uint8_t dfmt = 0;

    if (pull) {
        atomic_store(&wake_pending, 0);
        pthread_mutex_lock(&FB.lock);
        if (FB.gen != D.gen) {
            D.gen = FB.gen;
            texture_rebuild();
            FB.damage_all = 1;
        }
        D.connected = FB.connected;
        D.blanked = FB.blanked;
        if (D.tex && FB.pixels) {
            if (FB.damage_all) {
                SDL_UpdateTexture(D.tex, NULL, FB.pixels, (int)FB.stride);
            } else {
                for (int i = 0; i < FB.damage_n; i++) {
                    const struct view_rect *r = &FB.damage[i];
                    SDL_Rect sr = { r->x, r->y, r->w, r->h };
                    SDL_UpdateTexture(D.tex, &sr,
                                      FB.pixels + (size_t)r->y * FB.stride +
                                      (size_t)r->x * FB.bytespp,
                                      (int)FB.stride);
                }
            }
        }
        FB.damage_all = 0;
        FB.damage_n = 0;

        if (FB.batches != D.last_batches && FB.pixels) {
            D.last_batches = FB.batches;
            if (O.dump_n && D.dumped < O.dump_n &&
                D.presented_new++ % O.dump_every == 0) {
                size_t sz = FB.stride * FB.h;
                free(D.dumpbuf);
                D.dumpbuf = malloc(sz);
                if (D.dumpbuf) {
                    memcpy(D.dumpbuf, FB.pixels, sz);
                    dw = FB.w;
                    dh = FB.h;
                    dfmt = FB.pixfmt;
                    dump_now = 1;
                }
            }
        }
        pthread_mutex_unlock(&FB.lock);
    }

    SDL_SetRenderDrawColor(D.ren, 0, 0, 0, 255);
    SDL_RenderClear(D.ren);
    if (D.tex && D.connected && !D.blanked)
        SDL_RenderTexture(D.ren, D.tex, NULL, NULL);
    SDL_RenderPresent(D.ren);

    atomic_fetch_add_explicit(&ST.present_ns, now_ns() - t0,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&ST.presents, 1, memory_order_relaxed);

    if (dump_now)                       /* off the timed path */
        dump_ppm(D.dumpbuf, dw, dh, dfmt, D.dumped++);
}

/* ---------------------------------------------------------- input */

/*
 * Every keyboard/mouse/gamepad event goes through here first. Local
 * bindings (F11, long Escape) stay local; the rest will be forwarded to
 * the server's uinput devices (next step) through view_net_send(), with
 * pointer positions mapped to stream coordinates by
 * SDL_ConvertEventToRenderCoordinates() — the logical presentation
 * already accounts for scaling and black bars. Returns 1 to quit.
 */
static Uint64 esc_down_ms;

static int input_event(SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
        if (ev->key.repeat)
            return 0;
        if (ev->key.key == SDLK_F11) {
            O.fullscreen = !O.fullscreen;
            SDL_SetWindowFullscreen(D.win, O.fullscreen);
        } else if (ev->key.key == SDLK_ESCAPE) {
            esc_down_ms = SDL_GetTicks();
        }
        return 0;
    case SDL_EVENT_KEY_UP:
        if (ev->key.key == SDLK_ESCAPE)
            esc_down_ms = 0;
        return 0;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------- title/stats */

static void update_title(double fps)
{
    char t[256];
    const char *state;
    char fpsbuf[32];

    if (D.connected && D.blanked) {
        state = "screen off";
    } else if (D.connected) {
        snprintf(fpsbuf, sizeof(fpsbuf), "%.0f fps", fps);
        state = fpsbuf;
    } else {
        state = "no signal";
    }
    snprintf(t, sizeof(t), "sremfb-view — %s — %s", O.o.server, state);
    SDL_SetWindowTitle(D.win, t);
}

struct snap {
    unsigned long long batches, rects, bytes, dec_ns, dec_n, pings, pq_us;
    unsigned long long presents, pres_ns;
};

static void snap_take(struct snap *s)
{
    s->batches = atomic_load(&ST.rx_batches);
    s->rects = atomic_load(&ST.rx_rects);
    s->bytes = atomic_load(&ST.rx_bytes);
    s->dec_ns = atomic_load(&ST.decode_ns);
    s->dec_n = atomic_load(&ST.decode_n);
    s->pings = atomic_load(&ST.pings);
    s->pq_us = atomic_load(&ST.ping_queue_us);
    s->presents = atomic_load(&ST.presents);
    s->pres_ns = atomic_load(&ST.present_ns);
}

static void print_stats(const struct snap *a, const struct snap *b,
                        double dt)
{
    unsigned long long dec_n = b->dec_n - a->dec_n;
    unsigned long long pres = b->presents - a->presents;
    unsigned long long pings = b->pings - a->pings;

    fprintf(stderr,
            "sremfb-view: stats: rx %.1f fps (%.0f rects/s, %.2f MB/s) | "
            "shown %.1f fps | lz4 %.3f ms/rect (%llu) | "
            "upload+present %.3f ms | tcp srtt %.2f ms, ping queue +%.1f ms%s\n",
            (double)(b->batches - a->batches) / dt,
            (double)(b->rects - a->rects) / dt,
            (double)(b->bytes - a->bytes) / dt / 1e6,
            (double)pres / dt,
            dec_n ? (double)(b->dec_ns - a->dec_ns) / (double)dec_n / 1e6 : 0.0,
            dec_n,
            pres ? (double)(b->pres_ns - a->pres_ns) / (double)pres / 1e6 : 0.0,
            atomic_load(&ST.tcp_rtt_us) / 1000.0,
            pings ? (double)(b->pq_us - a->pq_us) / (double)pings / 1000.0 : 0.0,
            D.connected ? "" : " [not connected]");
}

/* ---------------------------------------------------------- main */

static void usage(FILE *out)
{
    fprintf(out,
"usage: sremfb-view [options] <server>\n"
"\n"
"Plugs a virtual monitor into the sremfb-server on <server> and shows it\n"
"in a window. F11 toggles fullscreen; hold Escape %d s (or close the\n"
"window) to quit.\n"
"\n"
"options:\n"
"  -s, --size WxH       virtual monitor resolution (default 1920x1080)\n"
"  -p, --port PORT      server TCP port (default %d)\n"
"      --rgb565         ask for 16-bit RGB565 pixels (half the bandwidth,\n"
"                       dithered by the server) instead of XRGB8888\n"
"      --no-lz4         don't accept LZ4 (raw pixels only)\n"
"      --vsync          sync presentation to the display (default: off,\n"
"                       lowest latency, may tear)\n"
"  -f, --fullscreen     start fullscreen\n"
"      --nearest        nearest-neighbour scaling (default: linear)\n"
"      --renderer NAME  SDL render driver (e.g. opengl, vulkan, gpu)\n"
"      --mac MAC        identity announced to the server (default: a\n"
"                       locally administered MAC derived from\n"
"                       /etc/machine-id, stable across runs)\n"
"      --model NAME     monitor name shown by the server's desktop\n"
"                       (13 chars max, default \"sremfb-view\")\n"
"      --stats          print receive/present statistics every %d s\n"
"      --dump N         save the first N updated frames as\n"
"                       sremfb-view-NNNN.ppm (validation without a screen)\n"
"      --dump-every K   save one updated frame out of K (default 1)\n"
"      --dump-dir DIR   where to save them (default: current directory)\n"
"  -h, --help           this help\n"
"  -V, --version        print the version\n",
            ESC_HOLD_MS / 1000, SREMFB_DEFAULT_PORT, STATS_EVERY_MS / 1000);
}

static unsigned parse_count(const char *s, const char *what)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || end == s || *end || v == 0 || v > 1000000) {
        fprintf(stderr, "sremfb-view: invalid %s \"%s\"\n", what, s);
        exit(2);
    }
    return (unsigned)v;
}

enum {
    OPT_RGB565 = 256, OPT_NO_LZ4, OPT_VSYNC, OPT_NEAREST, OPT_RENDERER,
    OPT_MAC, OPT_MODEL, OPT_STATS, OPT_DUMP, OPT_DUMP_EVERY, OPT_DUMP_DIR,
};

static void parse_args(int argc, char **argv)
{
    static const struct option lo[] = {
        { "size",       required_argument, NULL, 's' },
        { "port",       required_argument, NULL, 'p' },
        { "rgb565",     no_argument,       NULL, OPT_RGB565 },
        { "no-lz4",     no_argument,       NULL, OPT_NO_LZ4 },
        { "vsync",      no_argument,       NULL, OPT_VSYNC },
        { "fullscreen", no_argument,       NULL, 'f' },
        { "nearest",    no_argument,       NULL, OPT_NEAREST },
        { "renderer",   required_argument, NULL, OPT_RENDERER },
        { "mac",        required_argument, NULL, OPT_MAC },
        { "model",      required_argument, NULL, OPT_MODEL },
        { "stats",      no_argument,       NULL, OPT_STATS },
        { "dump",       required_argument, NULL, OPT_DUMP },
        { "dump-every", required_argument, NULL, OPT_DUMP_EVERY },
        { "dump-dir",   required_argument, NULL, OPT_DUMP_DIR },
        { "help",       no_argument,       NULL, 'h' },
        { "version",    no_argument,       NULL, 'V' },
        { NULL, 0, NULL, 0 },
    };
    int have_mac = 0;
    static char port[8];

    O.o.req_w = 1920;
    O.o.req_h = 1080;
    O.o.pixfmt = SREMFB_PIX_XRGB8888;
    snprintf(port, sizeof(port), "%d", SREMFB_DEFAULT_PORT);
    O.o.port = port;
    O.dump_every = 1;
    O.dump_dir = ".";
    model_copy(O.o.model, "sremfb-view");

    int c;
    while ((c = getopt_long(argc, argv, "s:p:fhV", lo, NULL)) != -1) {
        switch (c) {
        case 's': {
            char tail;
            if (sscanf(optarg, "%ux%u%c", &O.o.req_w, &O.o.req_h,
                       &tail) != 2 ||
                O.o.req_w < 64 || O.o.req_h < 64 ||
                O.o.req_w > 8192 || O.o.req_h > 8192) {
                fprintf(stderr, "sremfb-view: invalid size \"%s\" "
                        "(WxH, 64..8192)\n", optarg);
                exit(2);
            }
            break;
        }
        case 'p':
            if (parse_count(optarg, "port") > 65535) {
                fprintf(stderr, "sremfb-view: invalid port \"%s\"\n", optarg);
                exit(2);
            }
            O.o.port = optarg;
            break;
        case OPT_RGB565:     O.o.pixfmt = SREMFB_PIX_RGB565; break;
        case OPT_NO_LZ4:     O.o.no_lz4 = 1; break;
        case OPT_VSYNC:      O.vsync = 1; break;
        case 'f':            O.fullscreen = 1; break;
        case OPT_NEAREST:    O.nearest = 1; break;
        case OPT_RENDERER:   O.renderer = optarg; break;
        case OPT_MAC:
            if (parse_mac(optarg, O.o.mac) < 0) {
                fprintf(stderr, "sremfb-view: invalid MAC \"%s\"\n", optarg);
                exit(2);
            }
            have_mac = 1;
            break;
        case OPT_MODEL:      model_copy(O.o.model, optarg); break;
        case OPT_STATS:      O.stats = 1; break;
        case OPT_DUMP:       O.dump_n = parse_count(optarg, "--dump count"); break;
        case OPT_DUMP_EVERY:
            O.dump_every = parse_count(optarg, "--dump-every count");
            break;
        case OPT_DUMP_DIR:   O.dump_dir = optarg; break;
        case 'h':
            usage(stdout);
            exit(0);
        case 'V':
            printf("sremfb-view %s\n", SREMFB_VIEW_VERSION);
            exit(0);
        default:                        /* getopt already said what */
            fprintf(stderr, "try 'sremfb-view --help'\n");
            exit(2);
        }
    }
    if (optind != argc - 1) {
        fprintf(stderr, optind >= argc ?
                "sremfb-view: missing <server>\n" :
                "sremfb-view: too many arguments\n");
        fprintf(stderr, "try 'sremfb-view --help'\n");
        exit(2);
    }
    O.o.server = argv[optind];
    if (!have_mac)
        derive_mac(O.o.mac);
}

/* Initial window: the stream size, shrunk to fit the usable desktop. */
static void initial_window_size(int *w, int *h)
{
    SDL_Rect ub;
    *w = (int)O.o.req_w;
    *h = (int)O.o.req_h;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &ub) &&
        ub.w > 0 && ub.h > 0) {
        double sx = 0.9 * ub.w / *w, sy = 0.9 * ub.h / *h;
        double s = sx < sy ? sx : sy;
        if (s < 1.0) {
            *w = (int)(*w * s);
            *h = (int)(*h * s);
        }
    }
}

int main(int argc, char **argv)
{
    parse_args(argc, argv);

    SDL_SetAppMetadata("sremfb-view", SREMFB_VIEW_VERSION,
                       "fr.462eng.sremfb-view");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        view_log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    wake_type = SDL_RegisterEvents(1);

    int ww, wh;
    initial_window_size(&ww, &wh);
    D.win = SDL_CreateWindow("sremfb-view", ww, wh,
                             SDL_WINDOW_RESIZABLE |
                             SDL_WINDOW_HIGH_PIXEL_DENSITY |
                             (O.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!D.win) {
        view_log("cannot open a window: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    D.ren = SDL_CreateRenderer(D.win, O.renderer);
    if (!D.ren) {
        view_log("cannot create a renderer%s%s: %s",
                 O.renderer ? " " : "", O.renderer ? O.renderer : "",
                 SDL_GetError());
        SDL_DestroyWindow(D.win);
        SDL_Quit();
        return 1;
    }
    if (!SDL_SetRenderVSync(D.ren, O.vsync ? 1 : SDL_RENDERER_VSYNC_DISABLED))
        view_log("cannot %s vsync: %s", O.vsync ? "enable" : "disable",
                 SDL_GetError());
    view_log("video %s, renderer %s, vsync %s, identity "
             "%02x:%02x:%02x:%02x:%02x:%02x \"%.13s\"",
             SDL_GetCurrentVideoDriver(), SDL_GetRendererName(D.ren),
             O.vsync ? "on" : "off", O.o.mac[0], O.o.mac[1], O.o.mac[2],
             O.o.mac[3], O.o.mac[4], O.o.mac[5], O.o.model);
    update_title(0);
    refresh(0);

    struct view_net *net = view_net_start(&O.o, &FB, &ST, wake_main);
    if (!net) {
        SDL_DestroyRenderer(D.ren);
        SDL_DestroyWindow(D.win);
        SDL_Quit();
        return 1;
    }

    struct snap s_title, s_stats, s_now;
    snap_take(&s_title);
    s_stats = s_title;
    Uint64 t_title = SDL_GetTicks(), t_stats = t_title;
    int quit = 0;

    while (!quit) {
        Uint64 now = SDL_GetTicks();
        Sint64 wait = (Sint64)(t_title + TITLE_EVERY_MS) - (Sint64)now;
        if (O.stats && (Sint64)(t_stats + STATS_EVERY_MS) - (Sint64)now < wait)
            wait = (Sint64)(t_stats + STATS_EVERY_MS) - (Sint64)now;
        if (esc_down_ms && wait > 50)
            wait = 50;
        if (wait < 0)
            wait = 0;

        int pull = 0, repaint = 0;
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, (Sint32)wait)) {
            do {
                if (ev.type == wake_type) {
                    pull = 1;
                    continue;
                }
                switch (ev.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    quit = 1;
                    break;
                case SDL_EVENT_WINDOW_EXPOSED:
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                case SDL_EVENT_WINDOW_RESIZED:
                    repaint = 1;
                    break;
                default:
                    if (input_event(&ev))
                        quit = 1;
                    break;
                }
            } while (SDL_PollEvent(&ev));
        }
        if (esc_down_ms && SDL_GetTicks() - esc_down_ms >= ESC_HOLD_MS) {
            view_log("Escape held, quitting");
            quit = 1;
        }
        if (quit)
            break;
        if (pull || repaint)
            refresh(pull);

        now = SDL_GetTicks();
        if (now - t_title >= TITLE_EVERY_MS) {
            snap_take(&s_now);
            update_title((double)(s_now.presents - s_title.presents) * 1000.0 /
                         (double)(now - t_title));
            s_title = s_now;
            t_title = now;
        }
        if (O.stats && now - t_stats >= STATS_EVERY_MS) {
            snap_take(&s_now);
            print_stats(&s_stats, &s_now, (double)(now - t_stats) / 1000.0);
            s_stats = s_now;
            t_stats = now;
        }
    }

    view_log("exiting");
    view_net_stop(net);                 /* unplugs the remote monitor */
    if (D.tex)
        SDL_DestroyTexture(D.tex);
    SDL_DestroyRenderer(D.ren);
    SDL_DestroyWindow(D.win);
    SDL_Quit();
    free(FB.pixels);
    free(D.dumpbuf);
    return 0;
}
