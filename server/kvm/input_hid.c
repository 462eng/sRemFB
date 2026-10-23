/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * Entrées vers la cible : les messages INPUT du client (un événement evdev
 * par message, effectifs au SYN_REPORT) deviennent des rapports HID écrits
 * sur les trois fonctions du gadget USB du Pi (SPEC-kvm-backend.md §4.7,
 * gadget : sremfb-kvm-gadget du dépôt IPKVM) :
 *
 *   KEYBOARD  /dev/hidg0  clavier boot protocol, 8 octets, 6KRO :
 *                         [modificateurs][0][k1..k6]
 *   MOUSE     /dev/hidg1  souris relative, 5 octets :
 *                         [boutons][dx][dy][wheel][hwheel] (int8)
 *   POINTER   /dev/hidg2  pointeur absolu, 7 octets :
 *                         [boutons][X u16 LE][Y u16 LE][wheel][hwheel],
 *                         X/Y 0..32767 sur l'écran de la cible, letterbox
 *                         du flux inversé (sremfb_kvm_abs_map)
 *   GAMEPAD   —           ignoré en v1
 *
 * Même API que core/input.c. Le gadget est unique : un seul client tient
 * les entrées (le premier qui démarre son flux avec input_cap) ; les autres
 * n'obtiennent pas SREMFB_SRV_FLAG_INPUT, donc n'en envoient pas. Les nœuds
 * sont ouverts quand un client prend les entrées et fermés quand il les
 * rend : un nœud disparu (ENODEV, gadget recréé) est rouvert à la connexion
 * suivante.
 *
 * Écritures non bloquantes : une cible éteinte, en veille ou pas encore
 * énumérée ne lit pas ses rapports (EAGAIN, ESHUTDOWN) ; ils sont perdus,
 * jamais mis en attente, avec un message par épisode (rappel au plus une
 * fois par minute). Rien ne reste enfoncé : à la perte du client et sur
 * SREMFB_INDEV_ALL/0, des rapports vides sont envoyés.
 *
 * Les codes clavier sont des positions physiques (evdev) : la disposition
 * appliquée est celle de la cible, comme pour un vrai clavier USB. Les
 * touches multimédia (page Consumer) ne sont pas exposées en v1.
 *
 * SREMFB_HID_KBD / SREMFB_HID_MOUSE / SREMFB_HID_POINTER changent les
 * nœuds ; une variable vide supprime la fonction (par ex. pointeur absolu
 * refusé par une cible). Des FIFO ou fichiers ordinaires conviennent pour
 * les tests (tools/hid-test.py du dépôt IPKVM les décode).
 */
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kvm.h"

#define HID_ABS_MAX        32767
#define HID_MAX_SPLIT      32          /* rapports par SYN pour un grand
                                          déplacement relatif (±4064) */
#define HID_STALL_LOG_US   (60 * G_USEC_PER_SEC)

/* ----------------------------------------------- encodage (fonctions pures) */

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

gboolean sremfb_hid_kbd_held(const struct sremfb_hid_kbd *k, unsigned code)
{
    return code < KEY_CNT && bit_get(k->down, code);
}

int sremfb_hid_kbd_key(struct sremfb_hid_kbd *k, unsigned code, int value)
{
    unsigned u;
    int i;

    if (code >= KEY_CNT)
        return SREMFB_HID_KEY_NOUSAGE;
    u = sremfb_hid_usage[code];
    if (u == 0 || (u > SREMFB_HID_KBD_USAGE_MAX && (u < 0xE0 || u > 0xE7)))
        return SREMFB_HID_KEY_NOUSAGE;
    if (value != 0 && value != 1)
        return SREMFB_HID_KEY_SAME;    /* repeat : la cible répète seule */
    if ((value == 1) == bit_get(k->down, code))
        return SREMFB_HID_KEY_SAME;    /* double appui, relâché non tenu */

    if (u >= 0xE0) {                   /* modificateur : un bit, pas de case */
        if (value)
            k->mods |= (uint8_t)(1u << (u - 0xE0));
        else
            k->mods &= (uint8_t)~(1u << (u - 0xE0));
    } else if (value) {
        for (i = 0; i < 6 && k->keys[i]; i++)
            ;
        if (i == 6)
            return SREMFB_HID_KEY_ROLLOVER;
        k->keys[i] = (uint8_t)u;
    } else {
        for (i = 0; i < 6 && k->keys[i] != u; i++)
            ;
        if (i < 6) {                   /* retrait, les suivantes remontent */
            memmove(&k->keys[i], &k->keys[i + 1], (size_t)(5 - i));
            k->keys[5] = 0;
        }
    }
    bit_put(k->down, code, value == 1);
    return SREMFB_HID_KEY_CHANGED;
}

void sremfb_hid_kbd_report(const struct sremfb_hid_kbd *k, uint8_t out[8])
{
    out[0] = k->mods;
    out[1] = 0;
    memcpy(&out[2], k->keys, 6);
}

int sremfb_hid_button_bit(unsigned code)
{
    switch (code) {
    case BTN_LEFT:   return 0;
    case BTN_RIGHT:  return 1;
    case BTN_MIDDLE: return 2;
    case BTN_SIDE:   return 3;
    case BTN_EXTRA:  return 4;
    default:         return -1;
    }
}

/* Prend au plus ±127 de *acc. */
static int8_t take127(int *acc)
{
    int v = CLAMP(*acc, -127, 127);
    *acc -= v;
    return (int8_t)v;
}

gboolean sremfb_hid_mouse_report(struct sremfb_hid_mouse *m, uint8_t out[5])
{
    out[0] = m->buttons;
    out[1] = (uint8_t)take127(&m->dx);
    out[2] = (uint8_t)take127(&m->dy);
    out[3] = (uint8_t)take127(&m->wheel);
    out[4] = (uint8_t)take127(&m->hwheel);
    return m->dx || m->dy || m->wheel || m->hwheel;
}

gboolean sremfb_hid_pointer_report(struct sremfb_hid_mouse *m, int hx, int hy,
                                   uint8_t out[7])
{
    hx = CLAMP(hx, 0, HID_ABS_MAX);
    hy = CLAMP(hy, 0, HID_ABS_MAX);
    out[0] = m->buttons;
    out[1] = (uint8_t)(hx & 0xFF);
    out[2] = (uint8_t)(hx >> 8);
    out[3] = (uint8_t)(hy & 0xFF);
    out[4] = (uint8_t)(hy >> 8);
    out[5] = (uint8_t)take127(&m->wheel);
    out[6] = (uint8_t)take127(&m->hwheel);
    m->dx = m->dy = 0;                 /* sans objet en absolu */
    return m->wheel || m->hwheel;
}

/* Pixels du flux -> 0..32767 sur l'écran de la cible : inversion du
 * letterbox, clic dans les bandes = bord, centre du pixel source. Sans
 * signal (src_w == 0), le flux lui-même tient lieu de source. */
void sremfb_kvm_abs_map(const struct sremfb_kvm_fit *f, int x, int y,
                        int *hx, int *hy)
{
    int sw = f->src_w, sh = f->src_h;
    double s = f->s > 0 ? f->s : 1.0;
    double sx, sy;

    if (sw <= 0 || sh <= 0) {
        sw = f->dst_w > 0 ? f->dst_w : 1;
        sh = f->dst_h > 0 ? f->dst_h : 1;
        sx = x;
        sy = y;
    } else {
        sx = floor((x - f->ox) / s);
        sy = floor((y - f->oy) / s);
    }
    sx = CLAMP(sx, 0.0, (double)(sw - 1));
    sy = CLAMP(sy, 0.0, (double)(sh - 1));
    *hx = CLAMP((int)((sx + 0.5) * 32768.0 / sw), 0, HID_ABS_MAX);
    *hy = CLAMP((int)((sy + 0.5) * 32768.0 / sh), 0, HID_ABS_MAX);
}

/* ------------------------------------------------------- nœuds du gadget */

typedef struct {
    const char *what;                  /* pour les journaux */
    const char *path;                  /* "" : fonction absente */
    size_t len;                        /* taille du rapport */
    int fd;                            /* -1 : fermé */
    guint led_watch;                   /* clavier : rapports de sortie */
    gboolean stalled;                  /* épisode EAGAIN/ESHUTDOWN en cours */
    gint64 stall_log_us;
    guint64 written, dropped, ep_dropped;
} HidNode;

/* Un seul gadget, donc un seul détenteur. */
static struct {
    HidNode node[SREMFB_INDEV_POINTER + 1];   /* [0] inutilisé */
    SremfbClient *owner;
    gboolean gamepad_warned;
    uint8_t warned[KEY_CNT / 8];       /* touches sans usage déjà signalées */
} H;

typedef struct SremfbInput {
    struct sremfb_hid_kbd kbd;
    struct sremfb_hid_mouse mouse, ptr;
    uint8_t last_kbd[8];               /* derniers rapports émis */
    uint8_t last_btn[SREMFB_INDEV_POINTER + 1];
    gboolean dirty[SREMFB_INDEV_POINTER + 1];
    gboolean abs_placed;               /* un ABS reçu sur cette connexion */
    int abs_x, abs_y;                  /* pixels du flux */
    unsigned long n_events;
} SremfbInput;

/* Rapports de sortie du clavier (LED) : lus et jetés, sinon la file du
 * gadget grossit. */
static gboolean on_led(gint fd, GIOCondition cond, gpointer data)
{
    HidNode *n = data;
    uint8_t buf[64];
    ssize_t r;

    while ((r = read(fd, buf, sizeof(buf))) > 0)
        g_debug("entrées HID : LED clavier 0x%02x", buf[0]);
    if (r == 0 || (cond & (G_IO_HUP | G_IO_ERR)) ||
        (r < 0 && errno != EAGAIN && errno != EINTR)) {
        n->led_watch = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean node_open(HidNode *n)
{
    struct stat sb;
    int flags = O_NONBLOCK | O_CLOEXEC;
    gboolean have = n->path && *n->path && stat(n->path, &sb) == 0;

    n->stalled = FALSE;
    n->ep_dropped = 0;
    if (!n->path || !*n->path)
        return FALSE;
    /* le vrai nœud (caractère) du clavier est aussi lu, pour ses LED ;
     * une FIFO ou un fichier de test n'est qu'écrit (le lire volerait
     * les rapports au décodeur) */
    if (have && S_ISCHR(sb.st_mode) && n == &H.node[SREMFB_INDEV_KEYBOARD])
        flags |= O_RDWR;
    else
        flags |= O_WRONLY | (have && S_ISREG(sb.st_mode) ? O_APPEND : 0);
    n->fd = open(n->path, flags);
    if (n->fd < 0) {
        g_warning("entrées HID : %s %s inutilisable : %s", n->what, n->path,
                  g_strerror(errno));
        return FALSE;
    }
    if ((flags & O_ACCMODE) == O_RDWR)
        n->led_watch = g_unix_fd_add(n->fd, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                     on_led, n);
    return TRUE;
}

static void node_close(HidNode *n)
{
    g_clear_handle_id(&n->led_watch, g_source_remove);
    if (n->fd >= 0)
        close(n->fd);
    n->fd = -1;
}

/* Écrit un rapport sans jamais bloquer ; perdu si la cible ne lit pas. */
static void node_write(HidNode *n, const uint8_t *rep)
{
    ssize_t r;
    gint64 now;

    if (n->fd < 0)
        return;
    do
        r = write(n->fd, rep, n->len);
    while (r < 0 && errno == EINTR);

    if (r == (ssize_t)n->len) {
        n->written++;
        if (n->stalled) {
            g_message("entrées HID : %s : la cible lit de nouveau "
                      "(%" G_GUINT64_FORMAT " rapports perdus)", n->what,
                      n->ep_dropped);
            n->stalled = FALSE;
            n->ep_dropped = 0;
        }
        return;
    }
    if (r >= 0 || errno == EAGAIN || errno == EWOULDBLOCK ||
        errno == ESHUTDOWN) {
        n->dropped++;
        n->ep_dropped++;
        now = g_get_monotonic_time();
        if (!n->stalled) {
            n->stalled = TRUE;
            n->stall_log_us = now;
            g_message("entrées HID : %s : la cible ne lit pas (%s), "
                      "rapports abandonnés (éteinte, en veille, pas "
                      "énumérée ?)", n->what,
                      r >= 0 ? "écriture partielle" : g_strerror(errno));
        } else if (now - n->stall_log_us >= HID_STALL_LOG_US) {
            n->stall_log_us = now;
            g_message("entrées HID : %s : toujours pas lue, "
                      "%" G_GUINT64_FORMAT " rapports perdus", n->what,
                      n->ep_dropped);
        }
        return;
    }
    /* erreur dure (ENODEV : gadget retiré, EPIPE : lecteur de test
     * parti...) : fermé jusqu'à la prochaine prise des entrées */
    g_warning("entrées HID : %s %s : %s, fonction coupée jusqu'à la "
              "prochaine connexion", n->what, n->path, g_strerror(errno));
    node_close(n);
}

/* ----------------------------------------------------------- rapports */

static void kbd_flush(SremfbInput *in)
{
    sremfb_hid_kbd_report(&in->kbd, in->last_kbd);
    node_write(&H.node[SREMFB_INDEV_KEYBOARD], in->last_kbd);
    in->dirty[SREMFB_INDEV_KEYBOARD] = FALSE;
}

static void mouse_flush(SremfbInput *in)
{
    uint8_t rep[5];
    int i = 0;
    gboolean more;

    do {
        more = sremfb_hid_mouse_report(&in->mouse, rep);
        node_write(&H.node[SREMFB_INDEV_MOUSE], rep);
    } while (more && ++i < HID_MAX_SPLIT);
    in->mouse.dx = in->mouse.dy = in->mouse.wheel = in->mouse.hwheel = 0;
    in->last_btn[SREMFB_INDEV_MOUSE] = in->mouse.buttons;
    in->dirty[SREMFB_INDEV_MOUSE] = FALSE;
}

static void ptr_flush(SremfbClient *c, SremfbInput *in)
{
    const SremfbKvmClient *kc = c->src_ctx;
    struct sremfb_kvm_fit id = {
        .dst_w = c->geom.width, .dst_h = c->geom.height, .s = 1.0,
    };
    uint8_t rep[7];
    int hx, hy, i = 0;
    gboolean more;

    /* fit courant : suit les changements de mode de la source */
    sremfb_kvm_abs_map(kc ? &kc->fit : &id, in->abs_x, in->abs_y, &hx, &hy);
    do {
        more = sremfb_hid_pointer_report(&in->ptr, hx, hy, rep);
        node_write(&H.node[SREMFB_INDEV_POINTER], rep);
    } while (more && ++i < HID_MAX_SPLIT);
    in->ptr.wheel = in->ptr.hwheel = 0;
    in->last_btn[SREMFB_INDEV_POINTER] = in->ptr.buttons;
    in->dirty[SREMFB_INDEV_POINTER] = FALSE;
}

static void dev_flush(SremfbClient *c, SremfbInput *in, int dev)
{
    if (dev == SREMFB_INDEV_KEYBOARD)
        kbd_flush(in);
    else if (dev == SREMFB_INDEV_MOUSE)
        mouse_flush(in);
    else if (in->abs_placed)
        ptr_flush(c, in);
}

/* Rapports vides : rien ne reste enfoncé sur la cible. Le pointeur absolu
 * garde sa position (un rapport à 0,0 le ferait sauter dans le coin). */
static void release_all(SremfbClient *c, SremfbInput *in)
{
    memset(&in->kbd, 0, sizeof(in->kbd));
    memset(&in->mouse, 0, sizeof(in->mouse));
    memset(&in->ptr, 0, sizeof(in->ptr));
    for (int i = SREMFB_INDEV_KEYBOARD; i <= SREMFB_INDEV_POINTER; i++)
        dev_flush(c, in, i);
    in->dirty[SREMFB_INDEV_POINTER] = FALSE;
}

static gboolean kbd_usage_sent(const SremfbInput *in, unsigned code)
{
    unsigned u = sremfb_hid_usage[code];

    if (u >= 0xE0 && u <= 0xE7)
        return (in->last_kbd[0] >> (u - 0xE0)) & 1;
    return memchr(&in->last_kbd[2], (int)u, 6) != NULL;
}

static void on_key(SremfbClient *c, SremfbInput *in, unsigned code, int v)
{
    int r;

    /* appui et relâchement dans la même trame : émettre l'appui d'abord,
     * sinon la cible ne verrait rien */
    if (v == 0 && sremfb_hid_kbd_held(&in->kbd, code) &&
        !kbd_usage_sent(in, code))
        kbd_flush(in);
    r = sremfb_hid_kbd_key(&in->kbd, code, v);
    if (r == SREMFB_HID_KEY_CHANGED) {
        in->dirty[SREMFB_INDEV_KEYBOARD] = TRUE;
    } else if (r == SREMFB_HID_KEY_NOUSAGE && code < KEY_CNT && v == 1 &&
               !bit_get(H.warned, code)) {
        bit_put(H.warned, code, TRUE);
        g_message("[%s] entrées HID : touche evdev %u sans usage du "
                  "clavier boot (multimédia ?), ignorée", c->macstr, code);
    } else if (r == SREMFB_HID_KEY_ROLLOVER) {
        g_debug("[%s] entrées HID : 7e touche simultanée %u ignorée (6KRO)",
                c->macstr, code);
    }
}

static void on_button(SremfbClient *c, SremfbInput *in, int dev,
                      unsigned code, int v)
{
    struct sremfb_hid_mouse *m = dev == SREMFB_INDEV_MOUSE ? &in->mouse
                                                          : &in->ptr;
    int b = sremfb_hid_button_bit(code);
    uint8_t bit;

    if (b < 0 || (v != 0 && v != 1))
        return;
    bit = (uint8_t)(1u << b);
    if (!!(m->buttons & bit) == v)
        return;
    /* un clic avant la première position tomberait là où le pointeur de
     * la cible se trouve : jeté (comme core/input.c) */
    if (dev == SREMFB_INDEV_POINTER && v == 1 && !in->abs_placed)
        return;
    /* clic complet dans une seule trame : l'appui part d'abord */
    if (v == 0 && !(in->last_btn[dev] & bit))
        dev_flush(c, in, dev);
    if (v)
        m->buttons |= bit;
    else
        m->buttons &= (uint8_t)~bit;
    in->dirty[dev] = TRUE;
}

/* ------------------------------------------------------------ API */

void sremfb_input_init(SremfbServer *srv)
{
    static const char *const what[] = { NULL, "clavier", "souris",
                                        "pointeur" };
    static const char *const env[] = { NULL, "SREMFB_HID_KBD",
                                       "SREMFB_HID_MOUSE",
                                       "SREMFB_HID_POINTER" };
    static const char *const def[] = { NULL, "/dev/hidg0", "/dev/hidg1",
                                       "/dev/hidg2" };
    static const size_t len[] = { 0, 8, 5, 7 };

    for (int i = SREMFB_INDEV_KEYBOARD; i <= SREMFB_INDEV_POINTER; i++) {
        const char *v = getenv(env[i]);
        H.node[i].what = what[i];
        H.node[i].path = v ? v : def[i];
        H.node[i].len = len[i];
        H.node[i].fd = -1;
    }
    /* une FIFO de test dont le lecteur part ne doit pas tuer le serveur
     * (EPIPE suffit) ; le cœur envoie déjà en MSG_NOSIGNAL */
    signal(SIGPIPE, SIG_IGN);
    srv->input_enabled = TRUE;
    g_message("entrées HID ACTIVES : clavier %s, souris %s, pointeur %s "
              "(ouverts quand un client prend les entrées, un seul à la "
              "fois)",
              *H.node[1].path ? H.node[1].path : "désactivé",
              *H.node[2].path ? H.node[2].path : "désactivée",
              *H.node[3].path ? H.node[3].path : "désactivé");
}

uint8_t sremfb_input_start(SremfbClient *c)
{
    SremfbInput *in;
    int n = 0;

    if (!c->input_cap)
        return 0;
    if (!c->srv->input_enabled) {
        g_message("[%s] le client propose ses entrées, ignorées "
                  "(SREMFB_INPUT=0)", c->macstr);
        return 0;
    }
    if (c->input)
        return SREMFB_SRV_FLAG_INPUT;
    if (H.owner) {
        g_message("[%s] entrées refusées : déjà tenues par [%s] %s (un seul "
                  "client à la fois ; se reconnecter après son départ)",
                  c->macstr, H.owner->macstr, H.owner->peer);
        return 0;
    }

    for (int i = SREMFB_INDEV_KEYBOARD; i <= SREMFB_INDEV_POINTER; i++)
        n += node_open(&H.node[i]);
    if (n == 0) {
        g_warning("[%s] entrées HID : aucun nœud utilisable (gadget USB "
                  "absent ? sremfb-kvm-gadget start), entrées non offertes",
                  c->macstr);
        return 0;
    }
    in = g_new0(SremfbInput, 1);
    c->input = in;
    H.owner = c;
    g_message("[%s] entrées prises par %s : clavier %s, souris %s, "
              "pointeur %s", c->macstr, c->peer,
              H.node[1].fd >= 0 ? "oui" : "non",
              H.node[2].fd >= 0 ? "oui" : "non",
              H.node[3].fd >= 0 ? "oui" : "non");
    return SREMFB_SRV_FLAG_INPUT;
}

void sremfb_input_stop(SremfbClient *c)
{
    SremfbInput *in = c->input;
    guint64 w = 0, d = 0;

    if (!in)
        return;
    release_all(c, in);                /* rien ne reste enfoncé */
    for (int i = SREMFB_INDEV_KEYBOARD; i <= SREMFB_INDEV_POINTER; i++) {
        w += H.node[i].written;
        d += H.node[i].dropped;
        H.node[i].written = H.node[i].dropped = 0;
        node_close(&H.node[i]);
    }
    if (H.owner == c)
        H.owner = NULL;
    g_message("[%s] entrées relâchées (%lu événements, %" G_GUINT64_FORMAT
              " rapports écrits, %" G_GUINT64_FORMAT " perdus)", c->macstr,
              in->n_events, w, d);
    g_free(in);
    c->input = NULL;
}

void sremfb_input_msg(SremfbClient *c, const struct sremfb_input_msg *m)
{
    SremfbInput *in = c->input;
    int32_t v = m->ev_value;
    int dev = m->dev;
    struct sremfb_hid_mouse *mm;

    if (!in || H.owner != c)
        return;                        /* pas négocié : ignoré */
    if (dev == SREMFB_INDEV_ALL) {
        if (m->ev_type == 0)
            release_all(c, in);
        return;
    }
    if (dev == SREMFB_INDEV_GAMEPAD) {
        if (!H.gamepad_warned) {
            H.gamepad_warned = TRUE;
            g_message("[%s] entrées HID : manette non gérée (v1), "
                      "événements ignorés", c->macstr);
        }
        return;
    }
    if (dev < SREMFB_INDEV_KEYBOARD || dev > SREMFB_INDEV_POINTER ||
        H.node[dev].fd < 0)
        return;
    in->n_events++;
    mm = dev == SREMFB_INDEV_POINTER ? &in->ptr : &in->mouse;

    switch (m->ev_type) {
    case EV_SYN:
        if (m->ev_code == SYN_REPORT && in->dirty[dev])
            dev_flush(c, in, dev);
        break;
    case EV_KEY:
        if (dev == SREMFB_INDEV_KEYBOARD)
            on_key(c, in, m->ev_code, v);
        else
            on_button(c, in, dev, m->ev_code, v);
        break;
    case EV_REL:
        if (dev == SREMFB_INDEV_KEYBOARD)
            break;
        if (m->ev_code == REL_WHEEL)
            mm->wheel += v;
        else if (m->ev_code == REL_HWHEEL)
            mm->hwheel += v;
        else if (m->ev_code == REL_X && dev == SREMFB_INDEV_MOUSE)
            mm->dx += v;
        else if (m->ev_code == REL_Y && dev == SREMFB_INDEV_MOUSE)
            mm->dy += v;
        else
            break;                     /* *_HI_RES : la forme grossière
                                          suffit, le client envoie les deux */
        in->dirty[dev] = TRUE;
        break;
    case EV_ABS:
        if (dev != SREMFB_INDEV_POINTER ||
            (m->ev_code != ABS_X && m->ev_code != ABS_Y))
            break;
        if (m->ev_code == ABS_X)
            in->abs_x = v;
        else
            in->abs_y = v;
        in->abs_placed = TRUE;
        in->dirty[dev] = TRUE;
        break;
    default:
        break;
    }
}
