/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * Source synthétique de sremfb-kvm (§4.9) : développer et tester sur PC,
 * ou valider un Pi sans cible branchée.
 *
 *   SREMFB_KVM_DEVICE=test:1920x1080@50
 *       mire fixe (barres de couleur, rampe de gris, texte), un carré qui
 *       se déplace, un compteur de frames en haut à gauche ;
 *   SREMFB_KVM_DEVICE=file:/chemin/capture.uyvy@1920x1080@50
 *       rejeu en boucle d'un fichier UYVY brut (v4l2-ctl --stream-to=…).
 *
 * Les frames UYVY sont écrites dans des buffers internes accrochés à
 * st->buf[] et passées à sremfb_kvm_frame() : même chemin diff -> convert
 * -> fan-out que les frames V4L2 (DQBUF remplacé par un appel direct).
 *
 * Signaux (simulation d'une cible) :
 *   SIGUSR1  changement de mode : ~700 ms sans signal (BLANK), puis
 *            1024x768 (et retour au mode de départ au SIGUSR1 suivant) ;
 *            en rejeu de fichier, simple coupure du signal
 *   SIGUSR2  cible éteinte / rallumée (signal perdu / retrouvé)
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <glib-unix.h>

#include "kvm.h"

#define TEST_ALT_W     1024
#define TEST_ALT_H     768
#define TEST_GAP_MS    700           /* « pas de signal » pendant un
                                        changement de mode simulé */
#define TEST_BOX       128           /* côté du carré mobile */
#define TEST_FONT_SC   4             /* échelle du texte (paire) */

struct sremfb_kvm_testsrc {
    SremfbKvmState *st;
    /* rejeu de fichier */
    gboolean file;
    char *path;
    GMappedFile *mf;
    size_t nframes, pos;
    /* mode */
    int base_w, base_h;
    unsigned fps;
    int w, h;
    gboolean signal, alt;
    /* rendu */
    uint8_t *bg;                     /* mire fixe du mode courant */
    uint8_t *mem[SREMFB_KVM_NBUF];
    guint64 count;
    int bx, by, bdx, bdy;
    /* sources glib */
    guint timer_id, usr1_id, usr2_id, phase_id;
};

/* ------------------------------------------------------------ dessin */

typedef struct { uint8_t y, u, v; } Yuv;

/* RGB -> YUV BT.601 limited (les coefficients de core/convert.c). */
static Yuv rgb(int r, int g, int b)
{
    Yuv p = {
        (uint8_t)((66 * r + 129 * g + 25 * b + 128 + 4096) >> 8),
        (uint8_t)((-38 * r - 74 * g + 112 * b + 128 + 32768) >> 8),
        (uint8_t)((112 * r - 94 * g - 18 * b + 128 + 32768) >> 8),
    };
    return p;
}

/* Rectangle plein ; x et rw arrondis aux macro-pixels (paires). */
static void fill(uint8_t *buf, int w, int h, int x, int y, int rw, int rh,
                 Yuv c)
{
    int x0 = MAX(x, 0) & ~1, x1 = MIN(x + rw, w) & ~1;
    int y0 = MAX(y, 0), y1 = MIN(y + rh, h);

    for (int j = y0; j < y1; j++) {
        uint8_t *p = buf + (size_t)j * w * 2;
        for (int i = x0; i < x1; i += 2) {
            p[2 * i + 0] = c.u;
            p[2 * i + 1] = c.y;
            p[2 * i + 2] = c.v;
            p[2 * i + 3] = c.y;
        }
    }
}

/* Police 5x7, juste ce que la mire écrit. */
static const char FONT_CHARS[] = "0123456789BEFKMPRSTVX";
static const uint8_t FONT[][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },   /* 0 */
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },   /* 1 */
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },   /* 2 */
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },   /* 3 */
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },   /* 4 */
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },   /* 5 */
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },   /* 6 */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },   /* 7 */
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },   /* 8 */
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },   /* 9 */
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },   /* B */
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },   /* E */
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },   /* F */
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },   /* K */
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },   /* M */
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },   /* P */
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },   /* R */
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },   /* S */
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },   /* T */
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },   /* V */
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },   /* X */
};

/* Largeur en pixels d'un texte à l'échelle sc. */
static int text_w(const char *s, int sc)
{
    return (int)strlen(s) * 6 * sc;
}

static void text(uint8_t *buf, int w, int h, int x, int y, const char *s,
                 int sc, Yuv c)
{
    for (; *s; s++, x += 6 * sc) {
        const char *k = strchr(FONT_CHARS, *s);
        if (!k || *s == '\0')
            continue;                  /* espace et inconnus : vide */
        const uint8_t *g = FONT[k - FONT_CHARS];
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g[row] & (0x10 >> col))
                    fill(buf, w, h, x + col * sc, y + row * sc, sc, sc, c);
    }
}

/* Mire fixe : 7 barres, rampe de gris, texte du mode. */
static void draw_background(struct sremfb_kvm_testsrc *t)
{
    static const int bars[7][3] = {
        { 235, 235, 235 }, { 235, 235, 16 }, { 16, 235, 235 },
        { 16, 235, 16 }, { 235, 16, 235 }, { 235, 16, 16 }, { 16, 16, 235 },
    };
    int w = t->w, h = t->h, top = h * 2 / 3;
    char label[64];

    for (int i = 0; i < 7; i++) {
        int x0 = w * i / 7, x1 = w * (i + 1) / 7;
        fill(t->bg, w, h, x0, 0, x1 - x0 + 1, top,
             rgb(bars[i][0], bars[i][1], bars[i][2]));
    }
    for (int x = 0; x < w; x += 2) {
        int g = 255 * x / MAX(w - 2, 1);
        fill(t->bg, w, h, x, top, 2, h - top, rgb(g, g, g));
    }
    g_snprintf(label, sizeof(label), "SREMFB KVM TEST %dX%dP%u", w, h, t->fps);
    int sc = w >= 1280 ? TEST_FONT_SC : 2;
    int tw = text_w(label, sc), th = 7 * sc;
    int tx = (w - tw) / 2, ty = top + (h - top - th) / 2;
    fill(t->bg, w, h, tx - 2 * sc, ty - 2 * sc, tw + 4 * sc, th + 4 * sc,
         rgb(0, 0, 0));
    text(t->bg, w, h, tx, ty, label, sc, rgb(255, 255, 255));
}

/* Une frame animée dans buf : mire + carré mobile + compteur. */
static void draw_frame(struct sremfb_kvm_testsrc *t, uint8_t *buf)
{
    int w = t->w, h = t->h, sc = w >= 1280 ? TEST_FONT_SC : 2;
    int box = MIN(TEST_BOX, MIN(w, h) / 4) & ~1;
    char cnt[32];
    static const int hues[6][3] = {
        { 255, 64, 0 }, { 255, 255, 0 }, { 0, 255, 64 },
        { 0, 160, 255 }, { 128, 0, 255 }, { 255, 0, 160 },
    };
    const int *hue = hues[(t->count / 25) % 6];

    memcpy(buf, t->bg, (size_t)w * h * 2);

    /* carré qui rebondit */
    t->bx += t->bdx;
    t->by += t->bdy;
    if (t->bx < 0 || t->bx + box > w) {
        t->bdx = -t->bdx;
        t->bx = CLAMP(t->bx, 0, w - box);
    }
    if (t->by < 0 || t->by + box > h) {
        t->bdy = -t->bdy;
        t->by = CLAMP(t->by, 0, h - box);
    }
    fill(buf, w, h, t->bx & ~1, t->by, box, box, rgb(hue[0], hue[1], hue[2]));

    /* compteur */
    g_snprintf(cnt, sizeof(cnt), "%08llu", (unsigned long long)t->count);
    fill(buf, w, h, 0, 0, text_w(cnt, sc) + 4 * sc, 11 * sc, rgb(0, 0, 0));
    text(buf, w, h, 2 * sc, 2 * sc, cnt, sc, rgb(255, 255, 255));
}

/* ------------------------------------------------------------- tick */

static gboolean tick(gpointer data)
{
    struct sremfb_kvm_testsrc *t = data;
    SremfbKvmState *st = t->st;
    int idx = -1;

    for (int i = 0; i < SREMFB_KVM_NBUF; i++)
        if (st->buf[i].queued && i != st->prev) {
            idx = i;
            break;
        }
    if (idx < 0)
        return G_SOURCE_CONTINUE;

    if (t->file) {
        size_t len = (size_t)t->w * t->h * 2;
        memcpy(st->buf[idx].map,
               g_mapped_file_get_contents(t->mf) + t->pos * len, len);
        t->pos = (t->pos + 1) % t->nframes;
    } else {
        draw_frame(t, st->buf[idx].map);
    }
    t->count++;
    sremfb_kvm_frame(st, idx);
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------- signaux */

static gboolean phase_cb(gpointer data)
{
    struct sremfb_kvm_testsrc *t = data;

    t->phase_id = 0;
    if (!t->file) {
        t->alt = !t->alt;
        if (t->alt) {
            t->w = TEST_ALT_W;
            t->h = TEST_ALT_H;
            if (t->base_w == TEST_ALT_W && t->base_h == TEST_ALT_H) {
                t->w = 1280;
                t->h = 720;
            }
        } else {
            t->w = t->base_w;
            t->h = t->base_h;
        }
    }
    t->signal = TRUE;
    g_message("test : signal rétabli en %dx%d", t->w, t->h);
    sremfb_kvm_source_changed(t->st);
    return G_SOURCE_REMOVE;
}

static gboolean on_usr1(gpointer data)
{
    struct sremfb_kvm_testsrc *t = data;

    if (t->phase_id)
        return G_SOURCE_CONTINUE;
    g_message("test : SIGUSR1, changement de mode simulé");
    t->signal = FALSE;
    sremfb_kvm_source_changed(t->st);
    t->phase_id = g_timeout_add(TEST_GAP_MS, phase_cb, t);
    return G_SOURCE_CONTINUE;
}

static gboolean on_usr2(gpointer data)
{
    struct sremfb_kvm_testsrc *t = data;

    g_clear_handle_id(&t->phase_id, g_source_remove);
    t->signal = !t->signal;
    g_message("test : SIGUSR2, cible %s", t->signal ? "rallumée" : "éteinte");
    sremfb_kvm_source_changed(t->st);
    return G_SOURCE_CONTINUE;
}

/* --------------------------------------------------------------- API */

/* "WxH" ou "WxH@fps" */
static gboolean parse_mode(const char *s, int *w, int *h, unsigned *fps)
{
    char *end;
    long v;

    v = strtol(s, &end, 10);
    if (end == s || (*end != 'x' && *end != 'X'))
        return FALSE;
    *w = (int)v;
    s = end + 1;
    v = strtol(s, &end, 10);
    if (end == s)
        return FALSE;
    *h = (int)v;
    *fps = 30;
    if (*end == '@') {
        s = end + 1;
        v = strtol(s, &end, 10);
        if (end == s)
            return FALSE;
        *fps = (unsigned)CLAMP(v, 1, 240);
    }
    return *end == '\0';
}

struct sremfb_kvm_testsrc *sremfb_kvm_test_new(SremfbKvmState *st,
                                               const char *spec)
{
    struct sremfb_kvm_testsrc *t = g_new0(struct sremfb_kvm_testsrc, 1);
    const char *mode = NULL;

    t->st = st;
    if (g_str_has_prefix(spec, "test:")) {
        mode = spec + 5;
    } else {
        /* file:chemin@WxH[@fps] : le chemin peut contenir des '@', on
         * découpe par la droite */
        const char *rest = spec + 5;
        const char *at = strrchr(rest, '@');
        if (at && !strchr(at, 'x') && !strchr(at, 'X') && at > rest) {
            const char *at2 = g_strrstr_len(rest, at - rest, "@");
            at = at2 ? at2 : at;
        }
        if (!at || at == rest) {
            g_printerr("SREMFB_KVM_DEVICE %s : attendu file:chemin@WxH[@fps]\n",
                       spec);
            g_free(t);
            return NULL;
        }
        t->file = TRUE;
        t->path = g_strndup(rest, (gsize)(at - rest));
        mode = at + 1;
    }
    if (!parse_mode(mode, &t->base_w, &t->base_h, &t->fps) ||
        t->base_w < 64 || t->base_h < 64 || t->base_w > 4096 ||
        t->base_h > 4096 || (t->base_w & 1)) {
        g_printerr("SREMFB_KVM_DEVICE %s : mode invalide (WxH[@fps], largeur "
                   "paire, 64..4096)\n", spec);
        sremfb_kvm_test_free(t);
        return NULL;
    }
    t->w = t->base_w;
    t->h = t->base_h;
    t->signal = TRUE;

    if (t->file) {
        GError *err = NULL;
        size_t len = (size_t)t->w * t->h * 2;
        t->mf = g_mapped_file_new(t->path, FALSE, &err);
        if (!t->mf) {
            g_printerr("rejeu %s : %s\n", t->path, err->message);
            g_error_free(err);
            sremfb_kvm_test_free(t);
            return NULL;
        }
        t->nframes = g_mapped_file_get_length(t->mf) / len;
        if (t->nframes == 0) {
            g_printerr("rejeu %s : plus petit qu'une frame UYVY %dx%d "
                       "(%zu octets)\n", t->path, t->w, t->h, len);
            sremfb_kvm_test_free(t);
            return NULL;
        }
        if (g_mapped_file_get_length(t->mf) % len)
            g_message("rejeu %s : %zu octets en trop ignorés", t->path,
                      g_mapped_file_get_length(t->mf) % len);
        g_message("source : rejeu UYVY %s, %zu frames %dx%d@%u", t->path,
                  t->nframes, t->w, t->h, t->fps);
    } else {
        g_message("source : mire de test %dx%d@%u (SIGUSR1 = changement de "
                  "mode, SIGUSR2 = cible éteinte/rallumée)", t->w, t->h, t->fps);
    }
    t->usr1_id = g_unix_signal_add(SIGUSR1, on_usr1, t);
    t->usr2_id = g_unix_signal_add(SIGUSR2, on_usr2, t);
    return t;
}

void sremfb_kvm_test_free(struct sremfb_kvm_testsrc *t)
{
    if (!t)
        return;
    sremfb_kvm_test_stop(t);
    g_clear_handle_id(&t->usr1_id, g_source_remove);
    g_clear_handle_id(&t->usr2_id, g_source_remove);
    g_clear_handle_id(&t->phase_id, g_source_remove);
    g_clear_pointer(&t->mf, g_mapped_file_unref);
    g_free(t->path);
    g_free(t);
}

gboolean sremfb_kvm_test_query(struct sremfb_kvm_testsrc *t, int *w, int *h,
                               unsigned *fps)
{
    *w = t->w;
    *h = t->h;
    *fps = t->fps;
    return t->signal;
}

gboolean sremfb_kvm_test_start(struct sremfb_kvm_testsrc *t)
{
    SremfbKvmState *st = t->st;
    size_t len = (size_t)t->w * t->h * 2;

    sremfb_kvm_test_stop(t);
    for (int i = 0; i < SREMFB_KVM_NBUF; i++) {
        t->mem[i] = g_malloc0(len);
        st->buf[i].map = t->mem[i];
        st->buf[i].len = len;
        st->buf[i].queued = TRUE;
    }
    if (!t->file) {
        t->bg = g_malloc0(len);
        draw_background(t);
        t->bx = t->w / 5;
        t->by = t->h / 5;
        t->bdx = MAX(t->w / 240, 2) & ~1;
        t->bdy = MAX(t->h / 180, 1);
    }
    t->timer_id = g_timeout_add(MAX(1000 / t->fps, 1), tick, t);
    return TRUE;
}

void sremfb_kvm_test_stop(struct sremfb_kvm_testsrc *t)
{
    g_clear_handle_id(&t->timer_id, g_source_remove);
    for (int i = 0; i < SREMFB_KVM_NBUF; i++)
        g_clear_pointer(&t->mem[i], g_free);
    g_clear_pointer(&t->bg, g_free);
}
