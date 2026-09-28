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
 * Keyboard, mouse and gamepad are forwarded to the server's uinput
 * devices once it accepted INPUT (input_event() below); the wire pixel
 * format stays abstract in struct view_fb, and new per-rect encodings
 * plug into net.c's decode switch without touching the display side.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <linux/input-event-codes.h>
#include <math.h>
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
    const char *test;
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

static const char *shot_path;       /* --test "shot FILE": explicit name */

static void dump_ppm(const uint8_t *px, unsigned w, unsigned h,
                     uint8_t pixfmt, unsigned idx)
{
    char path[1024];
    if (shot_path)
        snprintf(path, sizeof(path), "%s", shot_path);
    else
        snprintf(path, sizeof(path), "%s/sremfb-view-%04u.ppm", O.dump_dir,
                 idx);
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
    int input;                 /* FB.input as of the last pull */
    uint64_t probe_shown_ns;   /* --latency-test: first present carrying
                                  the probe hit */
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

static void input_state_changed(void);

/* Pull what the network thread published, upload the damage, present.
 * `pull` = 0 only repaints (expose/resize). */
static void refresh(int pull)
{
    uint64_t t0 = now_ns();
    int dump_now = 0;
    unsigned dw = 0, dh = 0;
    uint8_t dfmt = 0;
    uint64_t probe_hit = 0;
    int input_was = D.input;

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
        D.input = FB.input;
        probe_hit = FB.probe_hit_ns;   /* its pixels are in this upload */
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
    if (probe_hit && !D.probe_shown_ns)
        D.probe_shown_ns = now_ns();

    atomic_fetch_add_explicit(&ST.present_ns, now_ns() - t0,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&ST.presents, 1, memory_order_relaxed);

    if (dump_now)                       /* off the timed path */
        dump_ppm(D.dumpbuf, dw, dh, dfmt, D.dumped++);
    if (pull && D.input != input_was)
        input_state_changed();
}

/* ---------------------------------------------------------- input */

/*
 * Every keyboard/mouse/gamepad event goes through here. Once the server
 * confirmed INPUT (FB.input, mirrored in D.input), events are forwarded
 * as evdev triplets to its uinput devices; before that (or with an old
 * server / --no-input) the window is view only and keeps the historical
 * local bindings (F11 fullscreen, hold Escape to quit).
 *
 * Two mouse modes:
 *   - absolute (default, desktop/KVM use): the window is a tablet over
 *     the remote screen, the remote cursor follows the local one;
 *     positions are mapped to stream pixels by
 *     SDL_ConvertEventToRenderCoordinates() (the logical presentation
 *     already accounts for scaling and black bars);
 *   - captured (games): SDL relative mode (pointer locked, raw deltas)
 *     sent to the relative mouse, plus a keyboard grab so the desktop's
 *     own shortcuts (Super, Alt+Tab...) go to the remote side too — on
 *     GNOME this is the keyboard-shortcuts-inhibit protocol, which asks
 *     for permission once.
 * The host key is Right Ctrl (as in VirtualBox): a tap toggles the
 * capture, Right Ctrl+F toggles fullscreen, Right Ctrl+Q quits. Right
 * Ctrl itself is never forwarded. Losing the focus releases everything
 * held on the remote side and leaves the capture.
 */
static Uint64 esc_down_ms;
static struct view_net *NET;

static struct {
    int captured;
    int rctrl_down, rctrl_combo;
    float rel_x, rel_y;             /* sub-pixel remainders */
    int wheel_v, wheel_h;           /* hi-res remainders (1/120 notch) */
    int abs_x, abs_y;               /* last position sent, -1 = none */
    SDL_Gamepad *pad;
    SDL_JoystickID pad_id;
    unsigned dpad;                  /* bits: 1 up, 2 down, 4 left, 8 right */
    struct sremfb_input_msg q[64];  /* pending, sent in one write */
    int qn;
} I = { .abs_x = -1, .abs_y = -1 };

static void update_title(double fps);
static double last_fps;

static void in_flush(void)
{
    if (I.qn && D.input && NET)
        view_net_send(NET, I.q, sizeof(I.q[0]) * (size_t)I.qn);
    I.qn = 0;
}

static void in_push(uint8_t dev, uint16_t type, uint16_t code, int32_t value)
{
    if (I.qn == (int)(sizeof(I.q) / sizeof(I.q[0])))
        in_flush();
    struct sremfb_input_msg *m = &I.q[I.qn++];
    memset(m, 0, sizeof(*m));
    m->magic = SREMFB_MAGIC;
    m->type = SREMFB_CMSG_INPUT;
    m->dev = dev;
    m->ev_type = type;
    m->ev_code = code;
    m->ev_value = value;
}

static void in_syn(uint8_t dev)
{
    in_push(dev, EV_SYN, SYN_REPORT, 0);
}

/* Everything held on the remote side goes up (keys, buttons, gamepad). */
static void in_release_all(void)
{
    in_push(SREMFB_INDEV_ALL, 0, 0, 0);
    in_flush();
    I.dpad = 0;
}

static void capture_set(int on)
{
    if (on && !D.input)
        return;
    if (on == I.captured)
        return;
    if (!SDL_SetWindowRelativeMouseMode(D.win, on))
        view_log("relative mouse mode: %s", SDL_GetError());
    if (!SDL_SetWindowKeyboardGrab(D.win, on))
        view_log("keyboard grab: %s", SDL_GetError());
    I.captured = on;
    I.rel_x = I.rel_y = 0;
    I.abs_x = I.abs_y = -1;
    if (!on)
        in_release_all();
    view_log("mouse %s", on ? "captured (relative) — Right Ctrl releases" :
             "released (absolute)");
    update_title(last_fps);
}

/* Absolute pointer position (stream pixels) of a mouse event. */
static void in_abs_from(SDL_Event *ev)
{
    if (!D.tw || !D.th)
        return;
    SDL_ConvertEventToRenderCoordinates(D.ren, ev);
    float fx = ev->type == SDL_EVENT_MOUSE_MOTION ? ev->motion.x :
               ev->type == SDL_EVENT_MOUSE_WHEEL ? ev->wheel.mouse_x :
               ev->button.x;
    float fy = ev->type == SDL_EVENT_MOUSE_MOTION ? ev->motion.y :
               ev->type == SDL_EVENT_MOUSE_WHEEL ? ev->wheel.mouse_y :
               ev->button.y;
    int x = fx < 0 ? 0 : fx >= (float)D.tw ? (int)D.tw - 1 : (int)fx;
    int y = fy < 0 ? 0 : fy >= (float)D.th ? (int)D.th - 1 : (int)fy;
    if (x == I.abs_x && y == I.abs_y)
        return;
    I.abs_x = x;
    I.abs_y = y;
    in_push(SREMFB_INDEV_POINTER, EV_ABS, ABS_X, x);
    in_push(SREMFB_INDEV_POINTER, EV_ABS, ABS_Y, y);
    in_syn(SREMFB_INDEV_POINTER);
}

static uint16_t in_button_code(Uint8 b)
{
    switch (b) {
    case SDL_BUTTON_LEFT:   return BTN_LEFT;
    case SDL_BUTTON_MIDDLE: return BTN_MIDDLE;
    case SDL_BUTTON_RIGHT:  return BTN_RIGHT;
    case SDL_BUTTON_X1:     return BTN_SIDE;
    case SDL_BUTTON_X2:     return BTN_EXTRA;
    default:                return 0;
    }
}

/* Wheel in 1/120 notches (hi-res), with the classic notch events every
 * 120 for applications that ignore hi-res. */
static void in_wheel(uint8_t dev, float x, float y)
{
    int hv = (int)lroundf(y * 120.0f), hh = (int)lroundf(x * 120.0f);

    if (hv) {
        in_push(dev, EV_REL, REL_WHEEL_HI_RES, hv);
        I.wheel_v += hv;
        for (; I.wheel_v >= 120; I.wheel_v -= 120)
            in_push(dev, EV_REL, REL_WHEEL, 1);
        for (; I.wheel_v <= -120; I.wheel_v += 120)
            in_push(dev, EV_REL, REL_WHEEL, -1);
    }
    if (hh) {
        in_push(dev, EV_REL, REL_HWHEEL_HI_RES, hh);
        I.wheel_h += hh;
        for (; I.wheel_h >= 120; I.wheel_h -= 120)
            in_push(dev, EV_REL, REL_HWHEEL, 1);
        for (; I.wheel_h <= -120; I.wheel_h += 120)
            in_push(dev, EV_REL, REL_HWHEEL, -1);
    }
    if (hv || hh)
        in_syn(dev);
}

/* SDL gamepad -> Xbox 360 pad as exposed by the kernel's xpad driver. */
static uint16_t pad_button_code(int b)
{
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH:          return BTN_A;
    case SDL_GAMEPAD_BUTTON_EAST:           return BTN_B;
    case SDL_GAMEPAD_BUTTON_WEST:           return BTN_X;
    case SDL_GAMEPAD_BUTTON_NORTH:          return BTN_Y;
    case SDL_GAMEPAD_BUTTON_BACK:           return BTN_SELECT;
    case SDL_GAMEPAD_BUTTON_GUIDE:          return BTN_MODE;
    case SDL_GAMEPAD_BUTTON_START:          return BTN_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return BTN_THUMBL;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return BTN_THUMBR;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return BTN_TL;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return BTN_TR;
    default:                                return 0;
    }
}

static void pad_button(int b, int down)
{
    unsigned bit = b == SDL_GAMEPAD_BUTTON_DPAD_UP ? 1 :
                   b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 2 :
                   b == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? 4 :
                   b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 8 : 0;
    if (bit) {
        I.dpad = down ? I.dpad | bit : I.dpad & ~bit;
        in_push(SREMFB_INDEV_GAMEPAD, EV_ABS, ABS_HAT0X,
                !!(I.dpad & 8) - !!(I.dpad & 4));
        in_push(SREMFB_INDEV_GAMEPAD, EV_ABS, ABS_HAT0Y,
                !!(I.dpad & 2) - !!(I.dpad & 1));
    } else {
        uint16_t code = pad_button_code(b);
        if (!code)
            return;                 /* paddles, touchpad, misc: no 360 twin */
        in_push(SREMFB_INDEV_GAMEPAD, EV_KEY, code, down);
    }
    in_syn(SREMFB_INDEV_GAMEPAD);
    in_flush();
}

static void pad_axis(int a, int v)
{
    static const uint16_t codes[] = {
        [SDL_GAMEPAD_AXIS_LEFTX] = ABS_X,
        [SDL_GAMEPAD_AXIS_LEFTY] = ABS_Y,
        [SDL_GAMEPAD_AXIS_RIGHTX] = ABS_RX,
        [SDL_GAMEPAD_AXIS_RIGHTY] = ABS_RY,
        [SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = ABS_Z,
        [SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = ABS_RZ,
    };
    if (a < 0 || a >= (int)(sizeof(codes) / sizeof(codes[0])))
        return;
    /* SDL: sticks -32768..32767 with +Y down, like xpad; triggers
     * 0..32767 -> xpad's 0..255 */
    if (a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
        a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
        v = v <= 0 ? 0 : (v * 255 + 16383) / 32767;
    in_push(SREMFB_INDEV_GAMEPAD, EV_ABS, codes[a], v);
    in_syn(SREMFB_INDEV_GAMEPAD);
    in_flush();
}

/* Gamepad gone: neutral state on the remote pad. */
static void pad_neutral(void)
{
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
        uint16_t code = pad_button_code(b);
        if (code)
            in_push(SREMFB_INDEV_GAMEPAD, EV_KEY, code, 0);
    }
    I.dpad = 0;
    in_push(SREMFB_INDEV_GAMEPAD, EV_ABS, ABS_HAT0X, 0);
    in_push(SREMFB_INDEV_GAMEPAD, EV_ABS, ABS_HAT0Y, 0);
    for (uint16_t a = ABS_X; a <= ABS_RZ; a++)
        in_push(SREMFB_INDEV_GAMEPAD, EV_ABS, a, 0);
    in_syn(SREMFB_INDEV_GAMEPAD);
    in_flush();
}

static int local_only;              /* --latency-test: ignore local input */

static int input_event(SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        int down = ev->type == SDL_EVENT_KEY_DOWN;
        if (!D.input || local_only) {
            /* view only: historical local bindings */
            if (down && !ev->key.repeat && ev->key.key == SDLK_F11) {
                O.fullscreen = !O.fullscreen;
                SDL_SetWindowFullscreen(D.win, O.fullscreen);
            } else if (ev->key.key == SDLK_ESCAPE && !ev->key.repeat) {
                esc_down_ms = down ? SDL_GetTicks() : 0;
            }
            return 0;
        }
        if (ev->key.scancode == SDL_SCANCODE_RCTRL) {      /* host key */
            if (down && !ev->key.repeat) {
                I.rctrl_down = 1;
                I.rctrl_combo = 0;
            } else if (!down) {
                if (I.rctrl_down && !I.rctrl_combo)
                    capture_set(!I.captured);
                I.rctrl_down = 0;
            }
            return 0;
        }
        if (I.rctrl_down) {                 /* host combos stay local */
            if (down && !ev->key.repeat) {
                I.rctrl_combo = 1;
                if (ev->key.scancode == SDL_SCANCODE_F) {
                    O.fullscreen = !O.fullscreen;
                    SDL_SetWindowFullscreen(D.win, O.fullscreen);
                } else if (ev->key.scancode == SDL_SCANCODE_Q) {
                    view_log("Right Ctrl+Q, quitting");
                    return 1;
                }
            }
            return 0;
        }
        if (ev->key.repeat)
            return 0;                       /* the remote side repeats */
        int code = view_scancode_to_evdev(ev->key.scancode);
        if (!code)
            return 0;
        in_push(SREMFB_INDEV_KEYBOARD, EV_KEY, (uint16_t)code, down);
        in_syn(SREMFB_INDEV_KEYBOARD);
        in_flush();
        return 0;
    }

    case SDL_EVENT_MOUSE_MOTION:
        if (!D.input || local_only)
            return 0;
        if (I.captured) {
            I.rel_x += ev->motion.xrel;
            I.rel_y += ev->motion.yrel;
            int dx = (int)I.rel_x, dy = (int)I.rel_y;
            I.rel_x -= (float)dx;
            I.rel_y -= (float)dy;
            if (!dx && !dy)
                return 0;
            if (dx)
                in_push(SREMFB_INDEV_MOUSE, EV_REL, REL_X, dx);
            if (dy)
                in_push(SREMFB_INDEV_MOUSE, EV_REL, REL_Y, dy);
            in_syn(SREMFB_INDEV_MOUSE);
        } else {
            in_abs_from(ev);
        }
        in_flush();
        return 0;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!D.input || local_only)
            return 0;
        uint16_t code = in_button_code(ev->button.button);
        if (!code)
            return 0;
        uint8_t dev = I.captured ? SREMFB_INDEV_MOUSE : SREMFB_INDEV_POINTER;
        if (!I.captured)
            in_abs_from(ev);                /* click where the pointer is */
        in_push(dev, EV_KEY, code, ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        in_syn(dev);
        in_flush();
        return 0;
    }

    case SDL_EVENT_MOUSE_WHEEL: {
        if (!D.input || local_only)
            return 0;
        float x = ev->wheel.x, y = ev->wheel.y;
        if (ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            x = -x;                         /* physical direction: the */
            y = -y;                         /* remote side flips itself */
        }
        if (!I.captured)
            in_abs_from(ev);
        in_wheel(I.captured ? SREMFB_INDEV_MOUSE : SREMFB_INDEV_POINTER, x, y);
        in_flush();
        return 0;
    }

    case SDL_EVENT_WINDOW_FOCUS_LOST:
        I.rctrl_down = 0;
        if (D.input) {
            capture_set(0);
            in_release_all();               /* nothing stuck remotely */
        }
        return 0;

    case SDL_EVENT_GAMEPAD_ADDED:
        if (!I.pad) {
            I.pad = SDL_OpenGamepad(ev->gdevice.which);
            if (I.pad) {
                I.pad_id = ev->gdevice.which;
                view_log("gamepad \"%s\" forwarded as an Xbox 360 pad",
                         SDL_GetGamepadName(I.pad));
            }
        }
        return 0;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (I.pad && ev->gdevice.which == I.pad_id) {
            view_log("gamepad removed");
            SDL_CloseGamepad(I.pad);
            I.pad = NULL;
            if (D.input)
                pad_neutral();
        }
        return 0;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (D.input && !local_only && I.pad &&
            ev->gbutton.which == I.pad_id)
            pad_button(ev->gbutton.button, ev->gbutton.down);
        return 0;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (D.input && !local_only && I.pad && ev->gaxis.which == I.pad_id)
            pad_axis(ev->gaxis.axis, ev->gaxis.value);
        return 0;

    default:
        return 0;
    }
}

/* The server confirmed or withdrew INPUT (new connection). */
static void input_state_changed(void)
{
    if (!D.input) {
        if (I.captured)
            capture_set(0);
        I.qn = 0;
    }
    I.abs_x = I.abs_y = -1;
    I.wheel_v = I.wheel_h = 0;
    I.dpad = 0;
    update_title(last_fps);
}

/* ---------------------------------------------------------- test script */

/*
 * --test "cmd; cmd; ...": scripted input once the server accepted INPUT,
 * for validation without touching the local keyboard/mouse:
 *   wait MS | key NAME | hold NAME | up NAME | type TEXT
 *   abs X Y | rel DX DY | click [left|right|middle] | rclick-rel BTN
 *   wheel N | pad BTN | padhold BTN | padup BTN | padaxis AXIS VALUE
 *   shot FILE.ppm | quit (no release: the server must release)
 * NAME = SDL scancode name ("A", "Return", "Left Shift"...); BTN = a b x
 * y back guide start ls rs lb rb up down left right; AXIS = lx ly rx ry
 * lt rt (SDL ranges).
 */
static struct {
    char **cmds;
    int n, i;
    Uint64 next_ms;
    Uint64 wait_since;
} T;

static void test_parse(const char *spec)
{
    char *dup = strdup(spec), *save = NULL;
    for (char *tok = strtok_r(dup, ";", &save); tok;
         tok = strtok_r(NULL, ";", &save)) {
        while (*tok == ' ')
            tok++;
        size_t l = strlen(tok);
        while (l && tok[l - 1] == ' ')
            tok[--l] = '\0';
        if (!*tok)
            continue;
        T.cmds = realloc(T.cmds, sizeof(char *) * (size_t)(T.n + 1));
        T.cmds[T.n++] = strdup(tok);
    }
    free(dup);
}

static int pad_button_by_name(const char *s)
{
    static const struct { const char *n; int b; } t[] = {
        { "a", SDL_GAMEPAD_BUTTON_SOUTH }, { "b", SDL_GAMEPAD_BUTTON_EAST },
        { "x", SDL_GAMEPAD_BUTTON_WEST }, { "y", SDL_GAMEPAD_BUTTON_NORTH },
        { "back", SDL_GAMEPAD_BUTTON_BACK },
        { "guide", SDL_GAMEPAD_BUTTON_GUIDE },
        { "start", SDL_GAMEPAD_BUTTON_START },
        { "ls", SDL_GAMEPAD_BUTTON_LEFT_STICK },
        { "rs", SDL_GAMEPAD_BUTTON_RIGHT_STICK },
        { "lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
        { "rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
        { "up", SDL_GAMEPAD_BUTTON_DPAD_UP },
        { "down", SDL_GAMEPAD_BUTTON_DPAD_DOWN },
        { "left", SDL_GAMEPAD_BUTTON_DPAD_LEFT },
        { "right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (strcmp(s, t[i].n) == 0)
            return t[i].b;
    return -1;
}

static void test_key(SDL_Scancode sc, int down)
{
    int code = view_scancode_to_evdev(sc);
    if (!code) {
        view_log("test: no evdev code for scancode %d", sc);
        return;
    }
    in_push(SREMFB_INDEV_KEYBOARD, EV_KEY, (uint16_t)code, down);
    in_syn(SREMFB_INDEV_KEYBOARD);
}

/* Runs the script steps that are due. Returns 1 to quit. */
static int test_step(void)
{
    while (T.i < T.n && SDL_GetTicks() >= T.next_ms) {
        char *c = T.cmds[T.i++];
        char a[128] = "", b[64] = "";
        int x = 0, y = 0;
        view_log("test: %s", c);

        if (sscanf(c, "wait %d", &x) == 1) {
            T.next_ms = SDL_GetTicks() + (Uint64)x;
        } else if (strncmp(c, "type ", 5) == 0) {
            for (const char *p = c + 5; *p; p++) {
                SDL_Keymod mod = 0;
                SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)*p, &mod);
                if (sc == SDL_SCANCODE_UNKNOWN)
                    continue;
                if (mod & SDL_KMOD_SHIFT)
                    test_key(SDL_SCANCODE_LSHIFT, 1);
                test_key(sc, 1);
                test_key(sc, 0);
                if (mod & SDL_KMOD_SHIFT)
                    test_key(SDL_SCANCODE_LSHIFT, 0);
            }
        } else if (sscanf(c, "key %127[^\n]", a) == 1) {
            SDL_Scancode sc = SDL_GetScancodeFromName(a);
            test_key(sc, 1);
            test_key(sc, 0);
        } else if (sscanf(c, "hold %127[^\n]", a) == 1) {
            test_key(SDL_GetScancodeFromName(a), 1);
        } else if (sscanf(c, "up %127[^\n]", a) == 1) {
            test_key(SDL_GetScancodeFromName(a), 0);
        } else if (sscanf(c, "abs %d %d", &x, &y) == 2) {
            in_push(SREMFB_INDEV_POINTER, EV_ABS, ABS_X, x);
            in_push(SREMFB_INDEV_POINTER, EV_ABS, ABS_Y, y);
            in_syn(SREMFB_INDEV_POINTER);
        } else if (sscanf(c, "rel %d %d", &x, &y) == 2) {
            in_push(SREMFB_INDEV_MOUSE, EV_REL, REL_X, x);
            in_push(SREMFB_INDEV_MOUSE, EV_REL, REL_Y, y);
            in_syn(SREMFB_INDEV_MOUSE);
        } else if (strncmp(c, "click", 5) == 0 ||
                   strncmp(c, "rclick-rel", 10) == 0) {
            uint8_t dev = c[0] == 'r' ? SREMFB_INDEV_MOUSE :
                                        SREMFB_INDEV_POINTER;
            sscanf(c, "%*s %63s", b);
            uint16_t code = strcmp(b, "right") == 0 ? BTN_RIGHT :
                            strcmp(b, "middle") == 0 ? BTN_MIDDLE : BTN_LEFT;
            in_push(dev, EV_KEY, code, 1);
            in_syn(dev);
            in_push(dev, EV_KEY, code, 0);
            in_syn(dev);
        } else if (sscanf(c, "wheel %d", &x) == 1) {
            in_wheel(SREMFB_INDEV_POINTER, 0, (float)x);
        } else if (sscanf(c, "padaxis %63s %d", b, &x) == 2) {
            static const char *ax[] = { "lx", "ly", "rx", "ry", "lt", "rt" };
            for (int k = 0; k < 6; k++)
                if (strcmp(b, ax[k]) == 0)
                    pad_axis(k, x);
        } else if (sscanf(c, "padhold %63s", b) == 1) {
            pad_button(pad_button_by_name(b), 1);
        } else if (sscanf(c, "padup %63s", b) == 1) {
            pad_button(pad_button_by_name(b), 0);
        } else if (sscanf(c, "pad %63s", b) == 1) {
            pad_button(pad_button_by_name(b), 1);
            pad_button(pad_button_by_name(b), 0);
        } else if (sscanf(c, "shot %127s", a) == 1) {
            pthread_mutex_lock(&FB.lock);
            size_t sz = FB.pixels ? FB.stride * FB.h : 0;
            uint8_t *copy = sz ? malloc(sz) : NULL;
            unsigned w = FB.w, h = FB.h;
            uint8_t fmt = FB.pixfmt;
            if (copy)
                memcpy(copy, FB.pixels, sz);
            pthread_mutex_unlock(&FB.lock);
            if (copy) {
                shot_path = a;
                dump_ppm(copy, w, h, fmt, 0);
                shot_path = NULL;
                free(copy);
            }
        } else if (strcmp(c, "quit") == 0) {
            in_flush();
            return 1;
        } else {
            view_log("test: unknown command \"%s\"", c);
        }
        in_flush();
    }
    return 0;
}

/* ---------------------------------------------------------- latency */

/*
 * --latency-test N: input -> remote rendering -> capture -> network ->
 * decoded here, on this machine's clock only. Needs the probe on the
 * server (tools/sremfb-latency-probe: a fullscreen window on our virtual
 * screen that flips its color on every input event). Each trial reads
 * the probe pixel, sends one event, and the network thread timestamps
 * the first decoded rect that changes that pixel ("rx"); "shown" is the
 * return of the first SDL_RenderPresent() after it. Not counted: the
 * local compositor and scanout, and the monitor's own lag.
 */
#define LAT_TIMEOUT_MS   1000
#define LAT_SETTLE_MS    60000     /* max wait for the probe */

enum { LAT_KEY, LAT_ABS, LAT_REL };

static struct {
    unsigned n;                 /* trials wanted, 0 = off */
    int kind;
    int px, py;                 /* probe pixel, -1 = default */
    unsigned done, lost;
    double *rx_ms, *shown_ms;
    int phase;                  /* 0 wait stream, 1 settle, 2 gap, 3 armed */
    Uint64 phase_ms;
    uint64_t t0_ns;
    int toggle;
    Uint64 click_ms;
} LAT = { .px = -1, .py = -1 };

static void lat_send(void)
{
    switch (LAT.kind) {
    case LAT_KEY:
        in_push(SREMFB_INDEV_KEYBOARD, EV_KEY, KEY_LEFTSHIFT, 1);
        in_syn(SREMFB_INDEV_KEYBOARD);
        break;
    case LAT_ABS:
        LAT.toggle ^= 1;
        in_push(SREMFB_INDEV_POINTER, EV_ABS, ABS_X,
                (int)FB.w / 4 + (LAT.toggle ? 60 : 0));
        in_push(SREMFB_INDEV_POINTER, EV_ABS, ABS_Y, (int)FB.h / 4);
        in_syn(SREMFB_INDEV_POINTER);
        break;
    case LAT_REL:
        LAT.toggle ^= 1;
        in_push(SREMFB_INDEV_MOUSE, EV_REL, REL_X, LAT.toggle ? 40 : -40);
        in_syn(SREMFB_INDEV_MOUSE);
        break;
    }
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void lat_print(const char *what, double *v, unsigned n)
{
    if (!n) {
        printf("  %-6s no sample\n", what);
        return;
    }
    qsort(v, n, sizeof(*v), cmp_double);
    double sum = 0;
    for (unsigned i = 0; i < n; i++)
        sum += v[i];
    unsigned p95 = (unsigned)((n - 1) * 0.95 + 0.5);
    printf("  %-6s min %6.2f  median %6.2f  p95 %6.2f  max %6.2f  "
           "mean %6.2f ms\n", what, v[0],
           n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2, v[p95],
           v[n - 1], sum / n);
}

/* Latency state machine, run from the main loop. Returns 1 when done. */
static int lat_step(void)
{
    Uint64 now = SDL_GetTicks();

    pthread_mutex_lock(&FB.lock);
    int ready = FB.connected && FB.input && FB.pixels && D.input;
    unsigned w = FB.w, h = FB.h;
    pthread_mutex_unlock(&FB.lock);

    if (!ready) {
        if (LAT.phase != 0)
            view_log("latency: stream lost, waiting");
        LAT.phase = 0;
        return 0;
    }
    int px = LAT.px >= 0 ? LAT.px : (int)w * 3 / 4;
    int py = LAT.py >= 0 ? LAT.py : (int)h * 3 / 4;
    if (px >= (int)w || py >= (int)h) {
        view_log("latency: probe point outside the %ux%u stream", w, h);
        return 1;
    }

    switch (LAT.phase) {
    case 0:                             /* stream up: let the devices and
                                           the probe settle, park the
                                           pointer in the probe window */
        LAT.phase = 1;
        LAT.phase_ms = now;
        LAT.click_ms = now + 1000;      /* devices: let libinput add them */
        in_push(SREMFB_INDEV_POINTER, EV_ABS, ABS_X, (int)w / 4);
        in_push(SREMFB_INDEV_POINTER, EV_ABS, ABS_Y, (int)h / 4);
        in_syn(SREMFB_INDEV_POINTER);
        in_flush();
        view_log("latency: %u trials (%s), probe pixel %d,%d — waiting "
                 "for the probe",
                 LAT.n, LAT.kind == LAT_KEY ? "key" :
                 LAT.kind == LAT_ABS ? "absolute pointer" : "relative mouse",
                 px, py);
        return 0;
    case 1:                             /* wait for the probe: click it
                                           once a second until it flips
                                           (proves it runs, gives it the
                                           focus for the key test) */
        pthread_mutex_lock(&FB.lock);
        if (FB.probe_hit_ns) {
            FB.probe_hit_ns = 0;
            pthread_mutex_unlock(&FB.lock);
            view_log("latency: probe answered, measuring");
            LAT.phase = 2;
            LAT.phase_ms = now + 500;
            return 0;
        }
        pthread_mutex_unlock(&FB.lock);
        if (now - LAT.phase_ms > LAT_SETTLE_MS) {
            view_log("latency: no probe on the virtual screen after %d s "
                     "(start sremfb-latency-probe there)",
                     LAT_SETTLE_MS / 1000);
            return 1;
        }
        if (now >= LAT.click_ms) {
            LAT.click_ms = now + 1000;
            pthread_mutex_lock(&FB.lock);
            FB.probe_x = (unsigned)px;
            FB.probe_y = (unsigned)py;
            FB.probe_base = view_fb_pixel(&FB, (unsigned)px, (unsigned)py);
            FB.probe_hit_ns = 0;
            FB.probe_armed = 1;
            pthread_mutex_unlock(&FB.lock);
            /* position again: the first one may predate the server's
             * view of our screen (it drops those, and the clicks) */
            in_push(SREMFB_INDEV_POINTER, EV_ABS, ABS_X, (int)w / 4);
            in_push(SREMFB_INDEV_POINTER, EV_ABS, ABS_Y, (int)h / 4 + 1);
            in_syn(SREMFB_INDEV_POINTER);
            in_push(SREMFB_INDEV_POINTER, EV_KEY, BTN_LEFT, 1);
            in_syn(SREMFB_INDEV_POINTER);
            in_push(SREMFB_INDEV_POINTER, EV_KEY, BTN_LEFT, 0);
            in_syn(SREMFB_INDEV_POINTER);
            in_flush();
        }
        return 0;
    case 2:                             /* random gap: decorrelate from the
                                           remote refresh phase */
        if (now < LAT.phase_ms)
            return 0;
        pthread_mutex_lock(&FB.lock);
        FB.probe_x = (unsigned)px;
        FB.probe_y = (unsigned)py;
        FB.probe_base = view_fb_pixel(&FB, (unsigned)px, (unsigned)py);
        FB.probe_hit_ns = 0;
        FB.probe_armed = 1;
        pthread_mutex_unlock(&FB.lock);
        D.probe_shown_ns = 0;
        LAT.t0_ns = now_ns();
        lat_send();
        in_flush();
        LAT.phase = 3;
        LAT.phase_ms = now;
        return 0;
    case 3: {
        pthread_mutex_lock(&FB.lock);
        uint64_t hit = FB.probe_hit_ns;
        int armed = FB.probe_armed;
        pthread_mutex_unlock(&FB.lock);
        if (now - LAT.phase_ms < LAT_TIMEOUT_MS &&
            ((!hit && armed) || (hit && !D.probe_shown_ns)))
            return 0;                   /* not decoded / not shown yet */
        if (hit && D.probe_shown_ns) {
            LAT.rx_ms[LAT.done] = (double)(hit - LAT.t0_ns) / 1e6;
            LAT.shown_ms[LAT.done] =
                (double)(D.probe_shown_ns - LAT.t0_ns) / 1e6;
            LAT.done++;
        } else {
            pthread_mutex_lock(&FB.lock);
            FB.probe_armed = 0;
            uint32_t cur = view_fb_pixel(&FB, (unsigned)px, (unsigned)py);
            uint32_t base = FB.probe_base;
            uint64_t late = FB.probe_hit_ns;
            pthread_mutex_unlock(&FB.lock);
            LAT.lost++;
            view_log("latency: trial %u lost (no change within %d ms; "
                     "pixel %06x, before %06x%s)", LAT.done + LAT.lost,
                     LAT_TIMEOUT_MS, cur, base,
                     late ? ", changed but not shown yet" : "");
        }
        if (LAT.kind == LAT_KEY) {
            in_push(SREMFB_INDEV_KEYBOARD, EV_KEY, KEY_LEFTSHIFT, 0);
            in_syn(SREMFB_INDEV_KEYBOARD);
            in_flush();
        }
        if (LAT.done + LAT.lost >= LAT.n || LAT.lost > LAT.n / 4 + 3) {
            printf("latency (%s, %ux%u, %u ok, %u lost):\n",
                   LAT.kind == LAT_KEY ? "key" :
                   LAT.kind == LAT_ABS ? "absolute pointer" :
                   "relative mouse", w, h, LAT.done, LAT.lost);
            lat_print("rx", LAT.rx_ms, LAT.done);
            lat_print("shown", LAT.shown_ms, LAT.done);
            fflush(stdout);
            return 1;
        }
        LAT.phase = 2;
        LAT.phase_ms = now + 60 + (Uint64)(rand() % 100);
        return 0;
    }
    }
    return 0;
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
    last_fps = fps;
    snprintf(t, sizeof(t), "sremfb-view — %s — %s — %s", O.o.server, state,
             !D.connected ? "input off" :
             !D.input ? "view only" :
             I.captured ? "CAPTURED (Right Ctrl: release)" :
                          "mouse absolute (Right Ctrl: capture)");
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
"in a window, forwarding keyboard, mouse and gamepad when the server\n"
"allows it (SREMFB_INPUT=1 on its side).\n"
"\n"
"With input: the mouse is absolute (the remote cursor follows yours);\n"
"tap Right Ctrl to capture it (relative mouse + all shortcuts, for\n"
"games), tap again to release. Right Ctrl+F fullscreen, Right Ctrl+Q\n"
"quit. View only (no input): F11 fullscreen, hold Escape %d s to quit.\n"
"Closing the window always quits.\n"
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
"      --no-input       never forward input (view only)\n"
"      --stats          print receive/present statistics every %d s\n"
"      --dump N         save the first N updated frames as\n"
"                       sremfb-view-NNNN.ppm (validation without a screen)\n"
"      --dump-every K   save one updated frame out of K (default 1)\n"
"      --dump-dir DIR   where to save them (default: current directory)\n"
"      --latency-test N measure input-to-picture latency over N trials\n"
"                       (needs sremfb-latency-probe on the server's\n"
"                       virtual screen), print min/median/p95/max, quit\n"
"      --latency-input key|abs|rel\n"
"                       event used by the test (default key: Left Shift)\n"
"      --latency-at X,Y probe pixel (default: 3/4 of the stream)\n"
"      --test SCRIPT    scripted input, \"cmd; cmd...\" (validation, see\n"
"                       the source: wait, key, type, abs, rel, click,\n"
"                       wheel, pad, padaxis, shot FILE, quit...)\n"
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
    OPT_NO_INPUT, OPT_LAT, OPT_LAT_INPUT, OPT_LAT_AT, OPT_TEST,
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
        { "no-input",   no_argument,       NULL, OPT_NO_INPUT },
        { "latency-test", required_argument, NULL, OPT_LAT },
        { "latency-input", required_argument, NULL, OPT_LAT_INPUT },
        { "latency-at", required_argument, NULL, OPT_LAT_AT },
        { "test",       required_argument, NULL, OPT_TEST },
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
        case OPT_NO_INPUT:   O.o.no_input = 1; break;
        case OPT_LAT:        LAT.n = parse_count(optarg, "trial count"); break;
        case OPT_LAT_INPUT:
            if (strcmp(optarg, "key") == 0) {
                LAT.kind = LAT_KEY;
            } else if (strcmp(optarg, "abs") == 0) {
                LAT.kind = LAT_ABS;
            } else if (strcmp(optarg, "rel") == 0) {
                LAT.kind = LAT_REL;
            } else {
                fprintf(stderr, "sremfb-view: --latency-input key|abs|rel\n");
                exit(2);
            }
            break;
        case OPT_LAT_AT: {
            char tail;
            if (sscanf(optarg, "%d,%d%c", &LAT.px, &LAT.py, &tail) != 2 ||
                LAT.px < 0 || LAT.py < 0) {
                fprintf(stderr, "sremfb-view: invalid --latency-at \"%s\"\n",
                        optarg);
                exit(2);
            }
            break;
        }
        case OPT_TEST:       O.test = optarg; break;
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
    if ((LAT.n || O.test) && O.o.no_input) {
        fprintf(stderr, "sremfb-view: --latency-test/--test need input\n");
        exit(2);
    }
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
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS |
                  (O.o.no_input ? 0 : SDL_INIT_GAMEPAD))) {
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

    if (LAT.n) {
        LAT.rx_ms = calloc(LAT.n, sizeof(double));
        LAT.shown_ms = calloc(LAT.n, sizeof(double));
        local_only = 1;
        srand((unsigned)now_ns());
    }
    if (O.test)
        test_parse(O.test);

    struct view_net *net = view_net_start(&O.o, &FB, &ST, wake_main);
    NET = net;
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
        if ((LAT.n || T.n) && wait > 2)
            wait = 2;                   /* scripted input: fine timers */
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

        if (T.n && D.input && T.i < T.n) {
            if (!T.wait_since) {
                T.wait_since = SDL_GetTicks();
                T.next_ms = T.wait_since;
            }
            if (test_step())
                break;
            if (T.i >= T.n)
                view_log("test: script done");
        } else if (T.n && !D.input && SDL_GetTicks() > 15000 &&
                   T.i == 0) {
            view_log("test: the server did not accept input, giving up");
            break;
        }
        if (LAT.n && lat_step())
            break;

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
    if (I.captured)
        capture_set(0);
    if (I.pad)
        SDL_CloseGamepad(I.pad);
    NET = NULL;
    view_net_stop(net);                 /* unplugs the remote monitor; the
                                           server releases what we held */
    if (D.tex)
        SDL_DestroyTexture(D.tex);
    SDL_DestroyRenderer(D.ren);
    SDL_DestroyWindow(D.win);
    SDL_Quit();
    free(FB.pixels);
    free(D.dumpbuf);
    return 0;
}
