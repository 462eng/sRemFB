/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * Damage logiciel par tuiles (§4.4). Une capture HDMI n'a pas de damage
 * natif : chaque frame est comparée octet pour octet à la précédente
 * (même buffer V4L2, retenu hors du driver), par tuiles de tile x tile
 * pixels. Comparaison exacte — le lien est numérique, des pixels
 * identiques le restent ; pas de seuil.
 *
 * La comparaison se fait dans le format capturé (UYVY : 2 octets/pixel),
 * avant toute conversion : seules les tuiles sales sont ensuite converties
 * en BGRx (convert_uyvy.c) et envoyées.
 */
#include <string.h>
#include <linux/videodev2.h>

#include "kvm.h"

unsigned sremfb_kvm_src_bpp(uint32_t fourcc)
{
    return fourcc == V4L2_PIX_FMT_RGB24 ? 3 : 2;
}

void sremfb_kvm_tiles_alloc(SremfbKvmState *st)
{
    int t = st->tile > 0 ? st->tile : 32;

    g_clear_pointer(&st->tile_dirty, g_free);
    st->tiles_x = st->src_w > 0 ? (st->src_w + t - 1) / t : 0;
    st->tiles_y = st->src_h > 0 ? (st->src_h + t - 1) / t : 0;
    if (st->tiles_x && st->tiles_y)
        st->tile_dirty = g_malloc0((size_t)st->tiles_x * st->tiles_y);
}

/* Pour chaque ligne de tuiles, ligne de pixels par ligne de pixels : un
 * memcmp par tuile encore propre. Une ligne de tuiles entièrement sale
 * s'arrête au plus tôt. */
int sremfb_kvm_diff(SremfbKvmState *st, const uint8_t *cur, const uint8_t *prev)
{
    int t = st->tile, n = 0;
    size_t bpp = sremfb_kvm_src_bpp(st->src_fourcc);
    size_t stride = st->src_stride ? st->src_stride : (size_t)st->src_w * bpp;
    size_t ntiles = (size_t)st->tiles_x * st->tiles_y;

    if (!st->tile_dirty || ntiles == 0)
        return 0;
    if (!prev) {
        memset(st->tile_dirty, 1, ntiles);
        return (int)ntiles;
    }
    memset(st->tile_dirty, 0, ntiles);

    for (int ty = 0; ty < st->tiles_y; ty++) {
        uint8_t *row = st->tile_dirty + (size_t)ty * st->tiles_x;
        int y0 = ty * t, y1 = MIN(y0 + t, st->src_h);
        int clean = st->tiles_x;

        for (int y = y0; y < y1 && clean > 0; y++) {
            const uint8_t *a = cur + (size_t)y * stride;
            const uint8_t *b = prev + (size_t)y * stride;
            for (int tx = 0; tx < st->tiles_x; tx++) {
                if (row[tx])
                    continue;
                int x0 = tx * t, x1 = MIN(x0 + t, st->src_w);
                size_t off = (size_t)x0 * bpp, len = (size_t)(x1 - x0) * bpp;
                if (memcmp(a + off, b + off, len) != 0) {
                    row[tx] = 1;
                    clean--;
                }
            }
        }
        n += st->tiles_x - clean;
    }
    return n;
}

/* Tuiles sales -> rectangles en pixels source : fusion des tuiles
 * consécutives d'une ligne, puis fusion verticale des rectangles de mêmes
 * x1/x2 adjacents ; au-delà de `max`, une seule bounding box (comme
 * dirty_add() du cœur). x1 est pair (tile l'est) ; x2 aussi sauf si la
 * largeur source est impaire (RGB24 seulement). */
int sremfb_kvm_rects(const SremfbKvmState *st, struct sremfb_rect *out, int max)
{
    int t = st->tile, n = 0;
    gboolean overflow = FALSE;
    struct sremfb_rect bb = { st->src_w, st->src_h, 0, 0 };

    if (!st->tile_dirty || max <= 0)
        return 0;

    for (int ty = 0; ty < st->tiles_y; ty++) {
        const uint8_t *row = st->tile_dirty + (size_t)ty * st->tiles_x;
        int y0 = ty * t, y1 = MIN(y0 + t, st->src_h);

        for (int tx = 0; tx < st->tiles_x; tx++) {
            if (!row[tx])
                continue;
            int tx1 = tx;
            while (tx1 + 1 < st->tiles_x && row[tx1 + 1])
                tx1++;
            struct sremfb_rect r = {
                .x1 = tx * t, .y1 = y0,
                .x2 = MIN((tx1 + 1) * t, st->src_w), .y2 = y1,
            };
            tx = tx1;

            bb.x1 = MIN(bb.x1, r.x1);
            bb.y1 = MIN(bb.y1, r.y1);
            bb.x2 = MAX(bb.x2, r.x2);
            bb.y2 = MAX(bb.y2, r.y2);
            if (overflow)
                continue;

            /* prolonge un rectangle de la ligne de tuiles précédente */
            gboolean merged = FALSE;
            for (int i = n - 1; i >= 0; i--) {
                if (out[i].y2 == r.y1 && out[i].x1 == r.x1 &&
                    out[i].x2 == r.x2) {
                    out[i].y2 = r.y2;
                    merged = TRUE;
                    break;
                }
            }
            if (merged)
                continue;
            if (n < max)
                out[n++] = r;
            else
                overflow = TRUE;
        }
    }
    if (overflow) {
        out[0] = bb;
        return 1;
    }
    return n;
}
