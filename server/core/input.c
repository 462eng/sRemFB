/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * Input injection: the client's keyboard, mouse and gamepad, replayed on
 * uinput devices of the server's seat — the desktop (libinput / mutter,
 * Steam, SDL) sees ordinary hardware.
 *
 * Off by default. SREMFB_INPUT=1 in /etc/sremfb-server.conf turns it on
 * for the clients that pass the CIDR allowlist; each client then gets its
 * own four devices, created when its stream starts (the server hello
 * confirms SREMFB_SRV_FLAG_INPUT only once they exist) and destroyed when
 * it leaves — after every key and button it still held has been released,
 * so a dropped link never leaves a key stuck down.
 *
 *   keyboard  every KEY_* below BTN_MISC, no autorepeat (the compositor
 *             repeats, as with a USB keyboard)
 *   mouse     relative: REL_X/Y + wheels (hi-res too), 5 buttons
 *   pointer   absolute, like QEMU's USB tablet: ABS_X/Y 0..32767 over the
 *             compositor's whole layout; the client sends stream pixels
 *             and this file places them on its virtual monitor, using the
 *             layout read from org.gnome.Mutter.DisplayConfig
 *   gamepad   "Microsoft X-Box 360 pad" 045e:028e, so Steam and SDL apply
 *             their stock mapping (no force feedback yet)
 *
 * /dev/uinput access: the package's udev rule (60-sremfb-uinput.rules)
 * tags it uaccess — the active seat's user, like Steam's own rule.
 */
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <linux/uinput.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core.h"

#define ABS_POINTER_MAX 32767

typedef struct {
    int fd;                            /* -1 = not created */
    const char *what;                  /* for logs */
    uint8_t keys[KEY_CNT / 8];         /* declared EV_KEY codes */
    uint8_t down[KEY_CNT / 8];         /* currently pressed */
    uint32_t rels;                     /* declared EV_REL codes (bits) */
    uint64_t abss;                     /* declared EV_ABS codes (bits) */
    int abs_min[ABS_CNT], abs_max[ABS_CNT];
    gboolean dirty;                    /* events since the last SYN */
} SremfbIndev;

typedef struct SremfbInput SremfbInput;

struct SremfbInput {
    SremfbIndev dev[SREMFB_INDEV_GAMEPAD + 1];   /* [0] unused */
    gboolean geo_warned;
    gboolean abs_placed;               /* the pointer reached the client's
                                          screen at least once */
    unsigned long n_events;
};

/* ------------------------------------------------ desktop layout (D-Bus) */

/* One monitor of the compositor's layout, in stage (global) coordinates —
 * the space libinput's absolute coordinates are scaled onto. */
typedef struct {
    char connector[32];
    int x, y, w, h;
} SremfbLayoutMon;

static struct {
    GDBusConnection *bus;
    guint sub_id;
    gboolean pending, again;
    GArray *mons;                      /* SremfbLayoutMon */
    int ext_w, ext_h;                  /* stage extents */
} L;

static void layout_refresh(void);

static void layout_parse(GVariant *res)
{
    GVariant *mons = NULL, *logs = NULL, *props = NULL;
    GHashTable *modes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, g_free);
    guint serial, layout_mode = 1;     /* 1 = logical, 2 = physical */
    GVariantIter it;
    GVariant *v;

    g_variant_get(res, "(u@a((ssss)a(siiddada{sv})a{sv})"
                  "@a(iiduba(ssss)a{sv})@a{sv})",
                  &serial, &mons, &logs, &props);
    g_variant_lookup(props, "layout-mode", "u", &layout_mode);

    /* current mode size of every monitor, by connector */
    g_variant_iter_init(&it, mons);
    while ((v = g_variant_iter_next_value(&it))) {
        GVariant *spec = g_variant_get_child_value(v, 0);
        GVariant *mlist = g_variant_get_child_value(v, 1);
        const char *conn;
        GVariantIter mi;
        GVariant *m;

        g_variant_get_child(spec, 0, "&s", &conn);
        g_variant_iter_init(&mi, mlist);
        while ((m = g_variant_iter_next_value(&mi))) {
            GVariant *mp = g_variant_get_child_value(m, 6);
            gboolean cur = FALSE;
            if (g_variant_lookup(mp, "is-current", "b", &cur) && cur) {
                int *wh = g_new(int, 2);
                g_variant_get_child(m, 1, "i", &wh[0]);
                g_variant_get_child(m, 2, "i", &wh[1]);
                g_hash_table_replace(modes, g_strdup(conn), wh);
            }
            g_variant_unref(mp);
            g_variant_unref(m);
        }
        g_variant_unref(mlist);
        g_variant_unref(spec);
        g_variant_unref(v);
    }

    g_array_set_size(L.mons, 0);
    L.ext_w = L.ext_h = 0;
    g_variant_iter_init(&it, logs);
    while ((v = g_variant_iter_next_value(&it))) {
        int x, y, w = 0, h = 0;
        double scale;
        guint32 transform;
        GVariant *specs = g_variant_get_child_value(v, 5);
        GVariantIter si;
        const char *conn;

        g_variant_get_child(v, 0, "i", &x);
        g_variant_get_child(v, 1, "i", &y);
        g_variant_get_child(v, 2, "d", &scale);
        g_variant_get_child(v, 3, "u", &transform);

        /* mirrored monitors share one logical monitor: same rect */
        g_variant_iter_init(&si, specs);
        while (g_variant_iter_next(&si, "(&s&s&s&s)", &conn, NULL, NULL, NULL)) {
            const int *wh = g_hash_table_lookup(modes, conn);
            if (!w && wh) {
                w = wh[0];
                h = wh[1];
                if (transform & 1) {       /* 90/270, flipped or not */
                    int t = w;
                    w = h;
                    h = t;
                }
                if (layout_mode == 1 && scale > 0) {
                    w = (int)lround(w / scale);
                    h = (int)lround(h / scale);
                }
            }
            SremfbLayoutMon lm = { .x = x, .y = y, .w = w, .h = h };
            g_strlcpy(lm.connector, conn, sizeof(lm.connector));
            g_array_append_val(L.mons, lm);
        }
        if (w > 0) {
            L.ext_w = MAX(L.ext_w, x + w);
            L.ext_h = MAX(L.ext_h, y + h);
        }
        g_variant_unref(specs);
        g_variant_unref(v);
    }

    GString *desc = g_string_new(NULL);
    for (guint i = 0; i < L.mons->len; i++) {
        const SremfbLayoutMon *lm = &g_array_index(L.mons, SremfbLayoutMon, i);
        g_string_append_printf(desc, " %s=%dx%d+%d+%d", lm->connector,
                               lm->w, lm->h, lm->x, lm->y);
    }
    g_message("input: desktop layout %dx%d (%s):%s", L.ext_w, L.ext_h,
              layout_mode == 1 ? "logical" : "physical", desc->str);
    g_string_free(desc, TRUE);

    g_hash_table_destroy(modes);
    g_variant_unref(mons);
    g_variant_unref(logs);
    g_variant_unref(props);
}

static void on_layout_reply(GObject *src, GAsyncResult *ar, gpointer data)
{
    GError *err = NULL;
    GVariant *res = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src),
                                                  ar, &err);
    L.pending = FALSE;
    if (!res) {
        g_warning("input: GetCurrentState failed: %s — absolute pointer "
                  "events are dropped until the layout is known",
                  err->message);
        g_error_free(err);
    } else {
        layout_parse(res);
        g_variant_unref(res);
    }
    if (L.again) {
        L.again = FALSE;
        layout_refresh();
    }
}

static void layout_refresh(void)
{
    if (!L.bus)
        return;
    if (L.pending) {
        L.again = TRUE;                /* re-read once this one lands */
        return;
    }
    L.pending = TRUE;
    g_dbus_connection_call(L.bus, "org.gnome.Mutter.DisplayConfig",
                           "/org/gnome/Mutter/DisplayConfig",
                           "org.gnome.Mutter.DisplayConfig",
                           "GetCurrentState", NULL,
                           G_VARIANT_TYPE("(ua((ssss)a(siiddada{sv})a{sv})"
                                          "a(iiduba(ssss)a{sv})a{sv})"),
                           G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                           on_layout_reply, NULL);
}

static void on_monitors_changed(GDBusConnection *bus, const gchar *sender,
                                const gchar *path, const gchar *iface,
                                const gchar *signal, GVariant *params,
                                gpointer data)
{
    layout_refresh();
}

/* Stream pixel -> 0..ABS_POINTER_MAX over the stage. FALSE while the
 * layout does not (yet) know the client's connector. */
static gboolean layout_map(const SremfbClient *c, int code, int v, int *out)
{
    if (!c->connector[0] || L.ext_w <= 0 || L.ext_h <= 0)
        return FALSE;
    for (guint i = 0; i < L.mons->len; i++) {
        const SremfbLayoutMon *lm = &g_array_index(L.mons, SremfbLayoutMon, i);
        if (strcmp(lm->connector, c->connector) != 0 || lm->w <= 0)
            continue;
        /* libinput: stage = value * extent / (max - min + 1); aim at the
         * middle of the target pixel */
        double g;
        if (code == ABS_X) {
            v = CLAMP(v, 0, c->geom.width - 1);
            g = lm->x + (v + 0.5) * lm->w / c->geom.width;
            *out = (int)(g * (ABS_POINTER_MAX + 1) / L.ext_w);
        } else {
            v = CLAMP(v, 0, c->geom.height - 1);
            g = lm->y + (v + 0.5) * lm->h / c->geom.height;
            *out = (int)(g * (ABS_POINTER_MAX + 1) / L.ext_h);
        }
        *out = CLAMP(*out, 0, ABS_POINTER_MAX);
        return TRUE;
    }
    return FALSE;
}

/* --------------------------------------------------------- uinput */

static gboolean bit_get(const uint8_t *b, unsigned n)
{
    return (b[n / 8] >> (n % 8)) & 1;
}

static void bit_put(uint8_t *b, unsigned n, gboolean on)
{
    if (on)
        b[n / 8] |= (uint8_t)(1u << (n % 8));
    else
        b[n / 8] &= (uint8_t)~(1u << (n % 8));
}

static void ui_emit(SremfbIndev *d, uint16_t type, uint16_t code,
                    int32_t value)
{
    struct input_event ev = { .type = type, .code = code, .value = value };

    /* one tiny write per event: the kernel timestamps it, libinput acts
     * on the SYN_REPORT the client (or we) send after it */
    if (write(d->fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
        static gboolean warned;
        if (!warned) {
            warned = TRUE;
            g_warning("input: uinput write failed on the %s: %s", d->what,
                      g_strerror(errno));
        }
    }
    d->dirty = type != EV_SYN;
}

static void ui_key(SremfbIndev *d, unsigned code)
{
    ioctl(d->fd, UI_SET_KEYBIT, code);
    bit_put(d->keys, code, TRUE);
}

static void ui_rel(SremfbIndev *d, unsigned code)
{
    ioctl(d->fd, UI_SET_RELBIT, code);
    d->rels |= 1u << code;
}

static void ui_abs(SremfbIndev *d, unsigned code, int min, int max,
                   int fuzz, int flat)
{
    struct uinput_abs_setup a = {
        .code = (uint16_t)code,
        .absinfo = { .minimum = min, .maximum = max, .fuzz = fuzz,
                     .flat = flat },
    };
    ioctl(d->fd, UI_SET_ABSBIT, code);
    ioctl(d->fd, UI_ABS_SETUP, &a);
    d->abss |= 1ull << code;
    d->abs_min[code] = min;
    d->abs_max[code] = max;
}

static gboolean ui_open(SremfbIndev *d, const char *what)
{
    memset(d, 0, sizeof(*d));
    d->what = what;
    d->fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    return d->fd >= 0;
}

static gboolean ui_create(SremfbIndev *d, const char *name, uint16_t bus,
                          uint16_t vendor, uint16_t product,
                          uint16_t version)
{
    struct uinput_setup us = {
        .id = { .bustype = bus, .vendor = vendor, .product = product,
                .version = version },
    };
    g_strlcpy(us.name, name, sizeof(us.name));
    if (ioctl(d->fd, UI_DEV_SETUP, &us) < 0 ||
        ioctl(d->fd, UI_DEV_CREATE) < 0) {
        g_warning("input: cannot create the %s: %s", d->what,
                  g_strerror(errno));
        close(d->fd);
        d->fd = -1;
        return FALSE;
    }
    return TRUE;
}

static void ui_buttons(SremfbIndev *d)
{
    ioctl(d->fd, UI_SET_EVBIT, EV_KEY);
    ui_key(d, BTN_LEFT);
    ui_key(d, BTN_RIGHT);
    ui_key(d, BTN_MIDDLE);
    ui_key(d, BTN_SIDE);
    ui_key(d, BTN_EXTRA);
    ioctl(d->fd, UI_SET_EVBIT, EV_REL);
    ui_rel(d, REL_WHEEL);
    ui_rel(d, REL_HWHEEL);
    ui_rel(d, REL_WHEEL_HI_RES);
    ui_rel(d, REL_HWHEEL_HI_RES);
}

/* Releases everything held on one device (+ re-centers a gamepad). */
static void indev_release(SremfbIndev *d)
{
    gboolean any = FALSE;

    if (d->fd < 0)
        return;
    for (unsigned k = 0; k < KEY_CNT; k++)
        if (bit_get(d->down, k)) {
            ui_emit(d, EV_KEY, (uint16_t)k, 0);
            bit_put(d->down, k, FALSE);
            any = TRUE;
        }
    /* gamepad: sticks and hat back to the center, triggers released
     * (the pointer's axes are positions, left alone) */
    for (unsigned a = 0; a < ABS_CNT; a++)
        if ((d->abss >> a) & 1 &&
            (d->abs_min[a] < 0 || a == ABS_Z || a == ABS_RZ)) {
            ui_emit(d, EV_ABS, (uint16_t)a, 0);
            any = TRUE;
        }
    if (any || d->dirty)
        ui_emit(d, EV_SYN, SYN_REPORT, 0);
}

static void input_release_all(SremfbClient *c)
{
    for (int i = 1; i <= SREMFB_INDEV_GAMEPAD; i++)
        indev_release(&c->input->dev[i]);
}

static const char *ui_sysname(const SremfbIndev *d, char *buf, size_t len)
{
    if (d->fd < 0 || ioctl(d->fd, UI_GET_SYSNAME(len), buf) < 0)
        g_strlcpy(buf, "-", len);
    return buf;
}

/* ------------------------------------------------------------ API */

void sremfb_input_init(SremfbServer *srv)
{
    GError *err = NULL;

    srv->input_enabled = TRUE;
    L.mons = g_array_new(FALSE, TRUE, sizeof(SremfbLayoutMon));
    L.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!L.bus) {
        g_warning("input: no session bus (%s) — the absolute pointer "
                  "cannot be placed, keyboard/mouse/gamepad still work",
                  err->message);
        g_error_free(err);
    } else {
        L.sub_id = g_dbus_connection_signal_subscribe(
            L.bus, "org.gnome.Mutter.DisplayConfig",
            "org.gnome.Mutter.DisplayConfig", "MonitorsChanged",
            "/org/gnome/Mutter/DisplayConfig", NULL,
            G_DBUS_SIGNAL_FLAGS_NONE, on_monitors_changed, NULL, NULL);
    }
    g_message("input injection ENABLED (SREMFB_INPUT=1): allowed clients "
              "that ask for it get a keyboard, mouse, pointer and gamepad "
              "on this seat");
}

uint8_t sremfb_input_start(SremfbClient *c)
{
    SremfbInput *in;
    SremfbIndev *d;
    char name[UINPUT_MAX_NAME_SIZE];
    char s1[32], s2[32], s3[32], s4[32];

    if (!c->input_cap)
        return 0;
    if (!c->srv->input_enabled) {
        g_message("[%s] client offers input, ignored (SREMFB_INPUT not "
                  "set on this server)", c->macstr);
        return 0;
    }
    if (c->input)
        return SREMFB_SRV_FLAG_INPUT;

    in = g_new0(SremfbInput, 1);
    for (int i = 0; i <= SREMFB_INDEV_GAMEPAD; i++)
        in->dev[i].fd = -1;

    /* keyboard: every key code below the button ranges */
    d = &in->dev[SREMFB_INDEV_KEYBOARD];
    if (ui_open(d, "keyboard")) {
        ioctl(d->fd, UI_SET_EVBIT, EV_KEY);
        for (unsigned k = KEY_ESC; k < BTN_MISC; k++)
            ui_key(d, k);
        g_snprintf(name, sizeof(name), "sRemFB %s keyboard", c->macstr);
        ui_create(d, name, BUS_VIRTUAL, 0x1d6b, 0x0104, 1);
    }

    d = &in->dev[SREMFB_INDEV_MOUSE];
    if (ui_open(d, "mouse")) {
        ui_buttons(d);
        ui_rel(d, REL_X);
        ui_rel(d, REL_Y);
        g_snprintf(name, sizeof(name), "sRemFB %s mouse", c->macstr);
        ui_create(d, name, BUS_VIRTUAL, 0x1d6b, 0x0104, 1);
    }

    /* absolute pointer, shaped like QEMU's usb-tablet: abs axes + mouse
     * buttons and no pen/touch bits => udev/libinput take it as a
     * pointer with absolute motion */
    d = &in->dev[SREMFB_INDEV_POINTER];
    if (ui_open(d, "pointer")) {
        ui_buttons(d);
        ioctl(d->fd, UI_SET_EVBIT, EV_ABS);
        ui_abs(d, ABS_X, 0, ABS_POINTER_MAX, 0, 0);
        ui_abs(d, ABS_Y, 0, ABS_POINTER_MAX, 0, 0);
        g_snprintf(name, sizeof(name), "sRemFB %s pointer", c->macstr);
        ui_create(d, name, BUS_VIRTUAL, 0x1d6b, 0x0104, 1);
    }

    /* Xbox 360 wired pad as the kernel's xpad driver exposes it: Steam
     * and SDL recognize the VID:PID and apply their stock mapping */
    d = &in->dev[SREMFB_INDEV_GAMEPAD];
    if (ui_open(d, "gamepad")) {
        static const unsigned btns[] = {
            BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT,
            BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR,
        };
        ioctl(d->fd, UI_SET_EVBIT, EV_KEY);
        for (unsigned i = 0; i < G_N_ELEMENTS(btns); i++)
            ui_key(d, btns[i]);
        ioctl(d->fd, UI_SET_EVBIT, EV_ABS);
        ui_abs(d, ABS_X, -32768, 32767, 16, 128);
        ui_abs(d, ABS_Y, -32768, 32767, 16, 128);
        ui_abs(d, ABS_RX, -32768, 32767, 16, 128);
        ui_abs(d, ABS_RY, -32768, 32767, 16, 128);
        ui_abs(d, ABS_Z, 0, 255, 0, 0);
        ui_abs(d, ABS_RZ, 0, 255, 0, 0);
        ui_abs(d, ABS_HAT0X, -1, 1, 0, 0);
        ui_abs(d, ABS_HAT0Y, -1, 1, 0, 0);
        ui_create(d, "Microsoft X-Box 360 pad", BUS_USB, 0x045e, 0x028e,
                  0x0114);
    }

    if (in->dev[SREMFB_INDEV_KEYBOARD].fd < 0 &&
        in->dev[SREMFB_INDEV_MOUSE].fd < 0 &&
        in->dev[SREMFB_INDEV_POINTER].fd < 0 &&
        in->dev[SREMFB_INDEV_GAMEPAD].fd < 0) {
        g_warning("[%s] input: /dev/uinput unusable (%s) — needs the "
                  "uinput module and the package's uaccess rule; input "
                  "not offered", c->macstr, g_strerror(errno));
        g_free(in);
        return 0;
    }
    c->input = in;
    layout_refresh();                  /* our connector just lit up */
    g_message("[%s] input ON for %s: keyboard %s, mouse %s, pointer %s, "
              "gamepad %s (connector %s)", c->macstr, c->peer,
              ui_sysname(&in->dev[SREMFB_INDEV_KEYBOARD], s1, sizeof(s1)),
              ui_sysname(&in->dev[SREMFB_INDEV_MOUSE], s2, sizeof(s2)),
              ui_sysname(&in->dev[SREMFB_INDEV_POINTER], s3, sizeof(s3)),
              ui_sysname(&in->dev[SREMFB_INDEV_GAMEPAD], s4, sizeof(s4)),
              c->connector[0] ? c->connector : "unknown");
    return SREMFB_SRV_FLAG_INPUT;
}

void sremfb_input_stop(SremfbClient *c)
{
    SremfbInput *in = c->input;

    if (!in)
        return;
    input_release_all(c);              /* nothing stays held down */
    for (int i = 1; i <= SREMFB_INDEV_GAMEPAD; i++)
        if (in->dev[i].fd >= 0) {
            ioctl(in->dev[i].fd, UI_DEV_DESTROY);
            close(in->dev[i].fd);
        }
    g_message("[%s] input devices removed (%lu events injected)",
              c->macstr, in->n_events);
    g_free(in);
    c->input = NULL;
}

void sremfb_input_msg(SremfbClient *c, const struct sremfb_input_msg *m)
{
    SremfbInput *in = c->input;
    SremfbIndev *d;
    int32_t v = m->ev_value;

    if (!in)
        return;                        /* not negotiated: ignore */
    if (m->dev == SREMFB_INDEV_ALL) {
        if (m->ev_type == 0)
            input_release_all(c);
        return;
    }
    if (m->dev > SREMFB_INDEV_GAMEPAD)
        return;
    d = &in->dev[m->dev];
    if (d->fd < 0)
        return;

    switch (m->ev_type) {
    case EV_SYN:
        if (m->ev_code != SYN_REPORT)
            return;
        break;
    case EV_KEY:
        if (m->ev_code >= KEY_CNT || !bit_get(d->keys, m->ev_code) ||
            (v != 0 && v != 1))
            return;
        /* a click before the pointer could be placed on the client's
         * screen would land wherever the cursor was — on another
         * monitor of this desktop: drop it */
        if (m->dev == SREMFB_INDEV_POINTER && v == 1 && !in->abs_placed)
            return;
        if (v == bit_get(d->down, m->ev_code))
            return;                    /* no double press, no release of
                                          a key that is not down */
        bit_put(d->down, m->ev_code, v == 1);
        break;
    case EV_REL:
        if (m->ev_code >= 32 || !((d->rels >> m->ev_code) & 1))
            return;
        break;
    case EV_ABS:
        if (m->ev_code >= ABS_CNT || !((d->abss >> m->ev_code) & 1))
            return;
        if (m->dev == SREMFB_INDEV_POINTER) {
            if (!layout_map(c, m->ev_code, v, &v)) {
                if (!in->geo_warned) {
                    in->geo_warned = TRUE;
                    g_message("[%s] input: connector %s not in the "
                              "desktop layout yet, absolute pointer "
                              "events dropped", c->macstr,
                              c->connector[0] ? c->connector : "unknown");
                }
                layout_refresh();
                return;
            }
            in->abs_placed = TRUE;
        } else {
            v = CLAMP(v, d->abs_min[m->ev_code], d->abs_max[m->ev_code]);
        }
        break;
    default:
        return;
    }
    ui_emit(d, m->ev_type, m->ev_code, v);
    in->n_events++;
}
