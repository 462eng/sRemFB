/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * Tests unitaires autonomes de diff.c et convert_uyvy.c (make check-kvm) :
 * valeurs de référence BT.601 limited, aller-retour avec les coefficients
 * de core/convert.c, diff de tuiles exact sur des images synthétiques
 * (bords non multiples de la tuile, stride avec marge, débordement).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/videodev2.h>

#include "kvm.h"

static int fails, checks;

#define CHECK(cond, ...) do {                                   \
        checks++;                                               \
        if (!(cond)) {                                          \
            fails++;                                            \
            printf("ÉCHEC %s:%d : ", __FILE__, __LINE__);       \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

/* ---------------------------------------------------------- conversion */

/* Convertit une paire UYVY, rend les deux pixels BGRx. */
static void conv_pair(uint8_t u, uint8_t y0, uint8_t v, uint8_t y1,
                      gboolean full, uint8_t out[8])
{
    uint8_t src[4] = { u, y0, v, y1 };
    SremfbKvmState st = {
        .src_w = 2, .src_h = 1, .src_fourcc = V4L2_PIX_FMT_UYVY,
        .src_stride = 4, .full_range = full, .master = out,
    };
    struct sremfb_rect r = { 0, 0, 2, 1 };

    memset(out, 0xAA, 8);
    sremfb_kvm_convert_rect(&st, src, &r);
}

static void expect_rgb(const uint8_t *px, int r, int g, int b, int tol,
                       const char *what)
{
    CHECK(abs(px[2] - r) <= tol && abs(px[1] - g) <= tol &&
          abs(px[0] - b) <= tol && px[3] == 0xFF,
          "%s : attendu R%d G%d B%d, obtenu R%d G%d B%d X%d", what, r, g, b,
          px[2], px[1], px[0], px[3]);
}

/* RGB -> YUV comme sremfb_convert_bgrx_to_i420() (core/convert.c). */
static void rgb_to_yuv(int r, int g, int b, uint8_t *y, uint8_t *u, uint8_t *v)
{
    *y = (uint8_t)((66 * r + 129 * g + 25 * b + 128 + 4096) >> 8);
    *u = (uint8_t)((-38 * r - 74 * g + 112 * b + 128 + 32768) >> 8);
    *v = (uint8_t)((112 * r - 94 * g - 18 * b + 128 + 32768) >> 8);
}

static void test_convert(void)
{
    uint8_t px[8];

    conv_pair(128, 16, 128, 16, FALSE, px);
    expect_rgb(px, 0, 0, 0, 0, "noir 16/128");
    expect_rgb(px + 4, 0, 0, 0, 0, "noir 16/128 (2e pixel)");
    conv_pair(128, 235, 128, 235, FALSE, px);
    expect_rgb(px, 255, 255, 255, 0, "blanc 235/128");
    conv_pair(128, 126, 128, 126, FALSE, px);
    expect_rgb(px, 128, 128, 128, 0, "gris 126/128");
    /* rouge BT.601 limited : Y 81, Cb 90, Cr 240 */
    conv_pair(90, 81, 240, 81, FALSE, px);
    expect_rgb(px, 255, 0, 0, 0, "rouge 81/90/240");
    /* deux pixels différents dans la paire : la chroma est partagée */
    conv_pair(128, 16, 128, 235, FALSE, px);
    expect_rgb(px, 0, 0, 0, 0, "paire noir|blanc, gauche");
    expect_rgb(px + 4, 255, 255, 255, 0, "paire noir|blanc, droite");
    /* saturations : hors plage limited */
    conv_pair(128, 0, 128, 255, FALSE, px);
    expect_rgb(px, 0, 0, 0, 0, "Y 0 saturé à 0");
    expect_rgb(px + 4, 255, 255, 255, 0, "Y 255 saturé à 255");
    /* full range */
    conv_pair(128, 0, 128, 255, TRUE, px);
    expect_rgb(px, 0, 0, 0, 0, "full : noir 0");
    expect_rgb(px + 4, 255, 255, 255, 0, "full : blanc 255");

    /* aller-retour avec l'encodeur du cœur, quantification limited */
    static const int cols[][3] = {
        { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 },
        { 0, 255, 255 }, { 255, 0, 255 }, { 200, 100, 50 }, { 30, 60, 90 },
        { 128, 128, 128 }, { 0, 0, 0 }, { 255, 255, 255 },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(cols); i++) {
        uint8_t y, u, v;
        char what[64];
        rgb_to_yuv(cols[i][0], cols[i][1], cols[i][2], &y, &u, &v);
        conv_pair(u, y, v, y, FALSE, px);
        g_snprintf(what, sizeof(what), "aller-retour %d,%d,%d", cols[i][0],
                   cols[i][1], cols[i][2]);
        expect_rgb(px, cols[i][0], cols[i][1], cols[i][2], 3, what);
    }

    /* rect partiel : x impair élargi au macro-pixel, le reste intact */
    {
        uint8_t src[4 * 4];            /* 8x1 UYVY */
        uint8_t master[8 * 4];
        SremfbKvmState st = {
            .src_w = 8, .src_h = 1, .src_fourcc = V4L2_PIX_FMT_UYVY,
            .src_stride = 16, .master = master,
        };
        struct sremfb_rect r = { 3, 0, 5, 1 };
        for (int i = 0; i < 4; i++) {
            src[4 * i] = 128; src[4 * i + 1] = 235;
            src[4 * i + 2] = 128; src[4 * i + 3] = 235;
        }
        memset(master, 0, sizeof(master));
        sremfb_kvm_convert_rect(&st, src, &r);
        CHECK(master[1 * 4] == 0 && master[6 * 4] == 0,
              "rect [3,5) : pixels 1 et 6 non touchés");
        CHECK(master[2 * 4] == 255 && master[5 * 4] == 255,
              "rect [3,5) : élargi à [2,6)");
    }

    /* RGB24 -> BGRx */
    {
        uint8_t src[6] = { 10, 20, 30, 40, 50, 60 };
        uint8_t master[8];
        SremfbKvmState st = {
            .src_w = 2, .src_h = 1, .src_fourcc = V4L2_PIX_FMT_RGB24,
            .src_stride = 6, .master = master,
        };
        struct sremfb_rect r = { 0, 0, 2, 1 };
        sremfb_kvm_convert_rect(&st, src, &r);
        expect_rgb(master, 10, 20, 30, 0, "RGB24 pixel 0");
        expect_rgb(master + 4, 40, 50, 60, 0, "RGB24 pixel 1");
    }
}

/* ---------------------------------------------------------------- diff */

typedef struct {
    SremfbKvmState st;
    uint8_t *a, *b;
} DiffCase;

static void dc_init(DiffCase *d, int w, int h, int tile, uint32_t fourcc,
                    size_t pad)
{
    memset(d, 0, sizeof(*d));
    d->st.src_w = w;
    d->st.src_h = h;
    d->st.tile = tile;
    d->st.src_fourcc = fourcc;
    d->st.src_stride = (size_t)w * sremfb_kvm_src_bpp(fourcc) + pad;
    sremfb_kvm_tiles_alloc(&d->st);
    size_t len = d->st.src_stride * (size_t)h;
    d->a = g_malloc(len);
    d->b = g_malloc(len);
    for (size_t i = 0; i < len; i++)
        d->a[i] = (uint8_t)(i * 7 + 3);
    memcpy(d->b, d->a, len);
}

static void dc_free(DiffCase *d)
{
    g_free(d->a);
    g_free(d->b);
    g_free(d->st.tile_dirty);
}

static void dc_poke(DiffCase *d, int x, int y)
{
    size_t bpp = sremfb_kvm_src_bpp(d->st.src_fourcc);
    d->b[(size_t)y * d->st.src_stride + (size_t)x * bpp] ^= 0x5A;
}

static void dc_reset(DiffCase *d)
{
    memcpy(d->b, d->a, d->st.src_stride * (size_t)d->st.src_h);
}

static gboolean rect_eq(const struct sremfb_rect *r, int x1, int y1, int x2,
                        int y2)
{
    return r->x1 == x1 && r->y1 == y1 && r->x2 == x2 && r->y2 == y2;
}

static void test_diff(void)
{
    DiffCase d;
    struct sremfb_rect r[16];
    int n, nr;

    /* 100x70, tuiles de 32 : 4x3 tuiles, dernière colonne de 4 px,
     * dernière ligne de 6 px */
    dc_init(&d, 100, 70, 32, V4L2_PIX_FMT_UYVY, 0);
    CHECK(d.st.tiles_x == 4 && d.st.tiles_y == 3, "grille 4x3 (%dx%d)",
          d.st.tiles_x, d.st.tiles_y);

    n = sremfb_kvm_diff(&d.st, d.b, d.a);
    nr = sremfb_kvm_rects(&d.st, r, 16);
    CHECK(n == 0 && nr == 0, "identiques : aucune tuile (%d, %d rects)", n, nr);

    n = sremfb_kvm_diff(&d.st, d.b, NULL);
    nr = sremfb_kvm_rects(&d.st, r, 16);
    CHECK(n == 12, "prev NULL : tout sale (%d)", n);
    CHECK(nr == 1 && rect_eq(&r[0], 0, 0, 100, 70),
          "prev NULL : un rect plein cadre (%d : %d,%d-%d,%d)", nr, r[0].x1,
          r[0].y1, r[0].x2, r[0].y2);

    /* dernier pixel : tuile (3,2) tronquée */
    dc_poke(&d, 99, 69);
    n = sremfb_kvm_diff(&d.st, d.b, d.a);
    nr = sremfb_kvm_rects(&d.st, r, 16);
    CHECK(n == 1 && d.st.tile_dirty[2 * 4 + 3], "pixel (99,69) : tuile (3,2)");
    CHECK(nr == 1 && rect_eq(&r[0], 96, 64, 100, 70),
          "pixel (99,69) : rect 96,64-100,70 (%d,%d-%d,%d)", r[0].x1,
          r[0].y1, r[0].x2, r[0].y2);
    dc_reset(&d);

    /* premier pixel, dernier octet d'une tuile (x 31, 2e octet UYVY) */
    d.b[0] ^= 1;
    d.b[31 * 2 + 1] ^= 1;
    n = sremfb_kvm_diff(&d.st, d.b, d.a);
    CHECK(n == 1 && d.st.tile_dirty[0], "octets 0 et 63 : seule la tuile (0,0)");
    dc_reset(&d);
    d.b[32 * 2] ^= 1;                  /* 1er octet de la tuile (1,0) */
    n = sremfb_kvm_diff(&d.st, d.b, d.a);
    CHECK(n == 1 && d.st.tile_dirty[1], "octet 64 : tuile (1,0)");
    dc_reset(&d);

    /* deux tuiles superposées : fusion verticale */
    dc_poke(&d, 40, 10);
    dc_poke(&d, 41 & ~1, 40);
    n = sremfb_kvm_diff(&d.st, d.b, d.a);
    nr = sremfb_kvm_rects(&d.st, r, 16);
    CHECK(n == 2 && nr == 1 && rect_eq(&r[0], 32, 0, 64, 64),
          "tuiles (1,0)+(1,1) : un rect 32,0-64,64 (%d tuiles, %d rects)", n,
          nr);
    dc_reset(&d);

    /* tuiles (0,0), (1,0), (3,0) : deux rects sur la ligne */
    dc_poke(&d, 0, 0);
    dc_poke(&d, 33 & ~1, 5);
    dc_poke(&d, 98, 31);
    n = sremfb_kvm_diff(&d.st, d.b, d.a);
    nr = sremfb_kvm_rects(&d.st, r, 16);
    CHECK(n == 3 && nr == 2 && rect_eq(&r[0], 0, 0, 64, 32) &&
          rect_eq(&r[1], 96, 0, 100, 32),
          "ligne 0 : 0,0-64,32 + 96,0-100,32 (%d rects)", nr);
    /* débordement : max 1 -> bounding box */
    nr = sremfb_kvm_rects(&d.st, r, 1);
    CHECK(nr == 1 && rect_eq(&r[0], 0, 0, 100, 32),
          "max 1 : bounding box 0,0-100,32 (%d,%d-%d,%d)", r[0].x1, r[0].y1,
          r[0].x2, r[0].y2);
    dc_reset(&d);

    /* diagonale : pas de fusion verticale (x différents) */
    dc_poke(&d, 0, 0);
    dc_poke(&d, 32, 32);
    dc_poke(&d, 64, 64);
    sremfb_kvm_diff(&d.st, d.b, d.a);
    nr = sremfb_kvm_rects(&d.st, r, 16);
    CHECK(nr == 3, "diagonale : 3 rects (%d)", nr);
    nr = sremfb_kvm_rects(&d.st, r, 2);
    CHECK(nr == 1 && rect_eq(&r[0], 0, 0, 96, 70),
          "diagonale, max 2 : bounding box 0,0-96,70");
    dc_free(&d);

    /* stride avec marge : la marge ne compte pas */
    dc_init(&d, 64, 64, 32, V4L2_PIX_FMT_UYVY, 16);
    for (int y = 0; y < 64; y++)
        d.b[(size_t)y * d.st.src_stride + 64 * 2 + 3] ^= 0xFF;
    n = sremfb_kvm_diff(&d.st, d.b, d.a);
    CHECK(n == 0, "marge de stride modifiée : aucune tuile (%d)", n);
    dc_free(&d);

    /* RGB24, largeur impaire, tuile de 16 */
    dc_init(&d, 33, 17, 16, V4L2_PIX_FMT_RGB24, 0);
    CHECK(d.st.tiles_x == 3 && d.st.tiles_y == 2, "RGB24 : grille 3x2");
    dc_poke(&d, 32, 16);
    n = sremfb_kvm_diff(&d.st, d.b, d.a);
    nr = sremfb_kvm_rects(&d.st, r, 16);
    CHECK(n == 1 && nr == 1 && rect_eq(&r[0], 32, 16, 33, 17),
          "RGB24 pixel (32,16) : rect 32,16-33,17");
    dc_free(&d);

    /* 1080p, tuile 32 : 60x34 tuiles (spec §4.4) */
    dc_init(&d, 1920, 1080, 32, V4L2_PIX_FMT_UYVY, 0);
    CHECK(d.st.tiles_x == 60 && d.st.tiles_y == 34, "1080p : 60x34 tuiles");
    dc_poke(&d, 1000, 500);
    gint64 t0 = g_get_monotonic_time();
    for (int i = 0; i < 20; i++)
        n = sremfb_kvm_diff(&d.st, d.b, d.a);
    gint64 t1 = g_get_monotonic_time();
    CHECK(n == 1, "1080p, un pixel : une tuile (%d)", n);
    printf("info : diff 1080p UYVY %.2f ms/frame\n", (t1 - t0) / 20 / 1000.0);
    {
        uint8_t *master = g_malloc((size_t)1920 * 1080 * 4);
        struct sremfb_rect full = { 0, 0, 1920, 1080 };
        d.st.master = master;
        t0 = g_get_monotonic_time();
        for (int i = 0; i < 20; i++)
            sremfb_kvm_convert_rect(&d.st, d.a, &full);
        t1 = g_get_monotonic_time();
        printf("info : conversion UYVY->BGRx 1080p %.2f ms/frame\n",
               (t1 - t0) / 20 / 1000.0);
        g_free(master);
    }
    dc_free(&d);
}

int main(void)
{
    test_convert();
    test_diff();
    printf("%s : %d vérifications, %d échec(s)\n", fails ? "ÉCHEC" : "OK",
           checks, fails);
    return fails ? 1 : 0;
}
