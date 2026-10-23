/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * STUB du lot A — remplacé par le lot B (rapports HID sur /dev/hidg0..2,
 * SPEC-kvm-backend.md §4.7). Même API que core/input.c ; ici rien n'est
 * injecté : sremfb_input_start() ne confirme jamais SREMFB_SRV_FLAG_INPUT,
 * donc aucun client n'envoie de message INPUT.
 *
 * sremfb_kvm_abs_map() (contrat de kvm.h, §4.7.4) est déjà implémentée
 * d'après la spec pour que le lot B puisse la reprendre telle quelle.
 */
#include <math.h>

#include "kvm.h"

void sremfb_input_init(SremfbServer *srv)
{
    (void)srv;
    g_message("entrées HID : stub, lot B");
}

uint8_t sremfb_input_start(SremfbClient *c)
{
    (void)c;
    return 0;
}

void sremfb_input_stop(SremfbClient *c)
{
    (void)c;
}

void sremfb_input_msg(SremfbClient *c, const struct sremfb_input_msg *m)
{
    (void)c;
    (void)m;
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
    *hx = CLAMP((int)((sx + 0.5) * 32768.0 / sw), 0, 32767);
    *hy = CLAMP((int)((sy + 0.5) * 32768.0 / sh), 0, 32767);
}
