/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * Stubs de l'édition de liens de sremfb-kvm (§4.8) : le KVM n'a ni son
 * (phase 2 : audio HDMI I2S), ni bureau GNOME dont mémoriser la
 * disposition. Remplacent core/audio.c (PipeWire) et core/layout.c (gio,
 * DisplayConfig de mutter), que session.c appelle sans condition.
 */
#include "kvm.h"

void sremfb_audio_init(SremfbServer *srv)
{
    (void)srv;
}

uint8_t sremfb_audio_start(SremfbClient *c)
{
    (void)c;
    return 0;                          /* jamais SREMFB_SRV_FLAG_AUDIO */
}

void sremfb_audio_stop(SremfbClient *c)
{
    (void)c;
}

void sremfb_audio_shutdown(void)
{
}

void sremfb_layout_init(SremfbServer *srv)
{
    (void)srv;
}

void sremfb_layout_client(SremfbClient *c)
{
    (void)c;
}

void sremfb_layout_client_gone(SremfbClient *c)
{
    (void)c;
}
