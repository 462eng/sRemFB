/* SPDX-License-Identifier: EUPL-1.2 */
/*
 * sremfb-kvm — le binaire KVM over IP : capture HDMI (TC358743 sur
 * Raspberry Pi 4, /dev/video0), entrées clavier/souris vers la cible par
 * le gadget USB HID, servies aux clients sRemFB (sremfb-view). Le cœur
 * (sessions, files, RAW/LZ4, PING/PONG, épisodes H.264) est celui de
 * sremfb-server ; ce fichier lit la configuration (§4.2 de
 * doc/SPEC-kvm-backend.md), monte la source kvm/ et passe la main à
 * sremfb_serve().
 */
#include <glib.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

#include <linux/videodev2.h>

#include "kvm.h"

#ifndef SREMFB_VERSION
#define SREMFB_VERSION "dev"
#endif

#define KVM_DEFAULT_DEVICE  "/dev/video0"
#define KVM_DEFAULT_EDID    "/usr/share/sremfb/edid-1080p50.bin"
#define KVM_DEFAULT_ENCODER "/dev/video11"

static const char *env_or(const char *name, const char *def)
{
    const char *v = getenv(name);
    return v ? v : def;
}

static void usage(const char *argv0)
{
    g_printerr(
        "usage : %s [-V] [--probe] [--port N] [--allow CIDR,CIDR...]\n"
        "  --probe   ouvre capture, EDID, encodeur et nœuds HID, imprime un\n"
        "            rapport et sort (0 = tout est là)\n"
        "  env : SREMFB_PORT, SREMFB_ALLOW, SREMFB_INPUT=0 (regard seul),\n"
        "        SREMFB_KVM_DEVICE (/dev/video0 | test:WxH[@fps] |\n"
        "                           file:chemin@WxH[@fps]),\n"
        "        SREMFB_KVM_EDID, SREMFB_KVM_FORMAT (uyvy|rgb24),\n"
        "        SREMFB_KVM_GEOMETRY (source|client),\n"
        "        SREMFB_KVM_RESIZE (scale|reconnect), SREMFB_KVM_TILE (16..128),\n"
        "        SREMFB_KVM_ENCODER, SREMFB_HID_KBD/MOUSE/POINTER,\n"
        "        SREMFB_NO_H264, SREMFB_FORCE_H264, SREMFB_NO_DITHER\n",
        argv0);
}

/* Lit une variable à choix ; exit(2) sur une valeur inconnue. */
static int env_choice(const char *name, const char *const *names, int def)
{
    const char *v = getenv(name);

    if (!v || !*v)
        return def;
    for (int i = 0; names[i]; i++)
        if (g_ascii_strcasecmp(v, names[i]) == 0)
            return i;
    g_printerr("%s=%s invalide (", name, v);
    for (int i = 0; names[i]; i++)
        g_printerr("%s%s", i ? "|" : "", names[i]);
    g_printerr(")\n");
    exit(2);
}

int main(int argc, char **argv)
{
    static const char *const fmt_names[] = { "uyvy", "rgb24", NULL };
    static const char *const geom_names[] = { "source", "client", NULL };
    static const char *const resize_names[] = { "scale", "reconnect", NULL };
    SremfbServer server = { .listen_fd = -1 };
    SremfbKvmState kvm = { .fd = -1, .prev = -1 };
    long port = SREMFB_DEFAULT_PORT;
    const char *allow = getenv("SREMFB_ALLOW");
    const char *env = getenv("SREMFB_PORT");
    gboolean probe = FALSE;

    /* messages en français : sans locale UTF-8 (service sans LANG), glib
     * remplacerait chaque accent par '?' */
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");        /* journaux : 30.2 fps, pas 30,2 */
    if (!g_get_charset(NULL))
        setlocale(LC_CTYPE, "C.UTF-8");

    if (env)
        port = atol(env);
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atol(argv[++i]);
        } else if (g_strcmp0(argv[i], "--allow") == 0 && i + 1 < argc) {
            allow = argv[++i];
        } else if (g_strcmp0(argv[i], "--probe") == 0) {
            probe = TRUE;
        } else if (g_strcmp0(argv[i], "-V") == 0 ||
                   g_strcmp0(argv[i], "--version") == 0) {
            g_print("sremfb-kvm %s\n", SREMFB_VERSION);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (port <= 0 || port > 65535) {
        g_printerr("port invalide\n");
        return 2;
    }
    server.port = (uint16_t)port;

    /* source */
    kvm.device = env_or("SREMFB_KVM_DEVICE", KVM_DEFAULT_DEVICE);
    if (!*kvm.device)
        kvm.device = KVM_DEFAULT_DEVICE;
    kvm.edid_path = env_or("SREMFB_KVM_EDID", KVM_DEFAULT_EDID);
    kvm.pixfmt = env_choice("SREMFB_KVM_FORMAT", fmt_names, 0) == 1 ?
                 V4L2_PIX_FMT_RGB24 : V4L2_PIX_FMT_UYVY;
    kvm.geometry = env_choice("SREMFB_KVM_GEOMETRY", geom_names, 0) == 1 ?
                   SREMFB_KVM_GEOM_CLIENT : SREMFB_KVM_GEOM_SOURCE;
    kvm.resize = env_choice("SREMFB_KVM_RESIZE", resize_names, 0) == 1 ?
                 SREMFB_KVM_RESIZE_RECONNECT : SREMFB_KVM_RESIZE_SCALE;
    kvm.tile = 32;
    if ((env = getenv("SREMFB_KVM_TILE")) && *env) {
        kvm.tile = atoi(env);
        if (kvm.tile < 16 || kvm.tile > 128 || (kvm.tile & 1)) {
            g_printerr("SREMFB_KVM_TILE=%s invalide (16..128, pair)\n", env);
            return 2;
        }
    }

    if (probe) {
        const char *hid[3] = {
            env_or("SREMFB_HID_KBD", "/dev/hidg0"),
            env_or("SREMFB_HID_MOUSE", "/dev/hidg1"),
            env_or("SREMFB_HID_POINTER", "/dev/hidg2"),
        };
        g_print("sremfb-kvm %s --probe\n", SREMFB_VERSION);
        return sremfb_kvm_probe(&kvm,
                                env_or("SREMFB_KVM_ENCODER", KVM_DEFAULT_ENCODER),
                                hid);
    }

    g_message("sremfb-kvm %s", SREMFB_VERSION);

    /* pas de téléport USB côté KVM : le cœur n'écrit rien dans /run/sremfb */
    setenv("SREMFB_NO_USB", "1", 0);

    /* entrées actives par défaut (c'est un KVM) ; SREMFB_INPUT=0 = regard seul */
    env = getenv("SREMFB_INPUT");
    if (!env || g_strcmp0(env, "0") != 0)
        sremfb_input_init(&server);
    else
        g_message("entrées désactivées (SREMFB_INPUT=0) : regard seul");

    if (allow && *allow && !net_allow_parse(&server, allow)) {
        g_printerr("SREMFB_ALLOW invalide : %s\n", allow);
        return 2;
    }

    server.source = &sremfb_kvm_ops;
    server.src = &kvm;
    if (sremfb_kvm_start(&server) != 0) {
        g_printerr("sremfb-kvm : source %s indisponible\n", kvm.device);
        return 1;
    }

    int rc = sremfb_serve(&server);

    sremfb_kvm_stop(&server);
    return rc;
}
