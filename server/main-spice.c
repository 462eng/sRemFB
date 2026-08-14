/*
 * sremfb-spice — the SPICE binary. Mirrors one QEMU/KVM VM (qxl display,
 * exposed over SPICE — e.g. Proxmox VE) to sremfb clients, reusing the
 * whole core (sessions, non-blocking per-client queues, RAW/LZ4,
 * RGB565+dither, adaptive H.264). One process = one VM.
 *
 * Config is environment-based (EnvironmentFile in the systemd template),
 * see /etc/sremfb-spice.conf. --port / --allow keep the sRemFB-side flags.
 */
#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "spice.h"

static SremfbResizeMode parse_resize(const char *s)
{
    if (!s || g_strcmp0(s, "agent") == 0)
        return SREMFB_RESIZE_AGENT;
    if (g_strcmp0(s, "scale") == 0)
        return SREMFB_RESIZE_SCALE;
    if (g_strcmp0(s, "off") == 0)
        return SREMFB_RESIZE_OFF;
    g_printerr("invalid SREMFB_RESIZE '%s' (agent|scale|off)\n", s);
    exit(2);
}

int main(int argc, char **argv)
{
    SremfbServer server = { .listen_fd = -1 };
    SremfbSpiceState spice = { 0 };
    long port = SREMFB_DEFAULT_PORT;
    const char *allow = getenv("SREMFB_ALLOW");
    const char *env;

    /* sRemFB listen side */
    if ((env = getenv("SREMFB_PORT")))
        port = atol(env);

    /* SPICE side (env, overridable by a couple of flags) */
    spice.cfg.host = g_strdup(getenv("SREMFB_SPICE_HOST"));
    spice.cfg.port = (env = getenv("SREMFB_SPICE_PORT")) ? atoi(env) : 0;
    spice.cfg.password = g_strdup(getenv("SREMFB_SPICE_PASSWORD"));
    spice.cfg.tls = (env = getenv("SREMFB_SPICE_TLS")) && atoi(env) != 0;
    spice.cfg.ca_file = g_strdup(getenv("SREMFB_SPICE_CA_FILE"));
    spice.cfg.display_id = (env = getenv("SREMFB_SPICE_DISPLAY")) ? atoi(env) : 0;
    spice.cfg.resize = parse_resize(getenv("SREMFB_RESIZE"));

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atol(argv[++i]);
        } else if (g_strcmp0(argv[i], "--allow") == 0 && i + 1 < argc) {
            allow = argv[++i];
        } else if (g_strcmp0(argv[i], "--spice-host") == 0 && i + 1 < argc) {
            g_free(spice.cfg.host);
            spice.cfg.host = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--spice-port") == 0 && i + 1 < argc) {
            spice.cfg.port = atoi(argv[++i]);
        } else {
            g_printerr("usage: %s [--port N] [--allow CIDR,...] "
                       "[--spice-host H] [--spice-port P]\n"
                       "  (env: SREMFB_SPICE_HOST/PORT/PASSWORD/TLS/CA_FILE/"
                       "DISPLAY, SREMFB_RESIZE, SREMFB_PORT, SREMFB_ALLOW)\n",
                       argv[0]);
            return 2;
        }
    }

    if (!spice.cfg.host || !*spice.cfg.host || spice.cfg.port <= 0) {
        g_printerr("SREMFB_SPICE_HOST and SREMFB_SPICE_PORT are required\n");
        return 2;
    }
    if (port <= 0 || port > 65535) {
        g_printerr("invalid port\n");
        return 2;
    }
    server.port = (uint16_t)port;

    if (allow && *allow && !net_allow_parse(&server, allow)) {
        g_printerr("invalid SREMFB_ALLOW value: %s\n", allow);
        return 2;
    }

    server.source = &sremfb_spice_ops;
    server.src = &spice;
    sremfb_spice_start(&server);      /* connect the SPICE session */

    return sremfb_serve(&server);
}
