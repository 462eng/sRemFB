/*
 * sremfb-server — the EVDI binary. Exposes one EVDI virtual connector per
 * connected sremfb-client on a GNOME/Wayland host. All the client-session
 * machinery lives in core/; this file only parses config, stands up the
 * EVDI frame source and hands control to sremfb_serve().
 */
#include <glib.h>
#include <stdlib.h>

#include "evdi.h"

int main(int argc, char **argv)
{
    SremfbServer server = { .listen_fd = -1 };
    SremfbEvdiState evdi = { .selfheal_left = 8 };
    long port = SREMFB_DEFAULT_PORT;
    const char *allow = getenv("SREMFB_ALLOW");
    const char *env = getenv("SREMFB_PORT");

    if (env)
        port = atol(env);
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atol(argv[++i]);
        } else if (g_strcmp0(argv[i], "--allow") == 0 && i + 1 < argc) {
            allow = argv[++i];
        } else {
            g_printerr("usage: %s [--port N] [--allow CIDR,CIDR...]\n"
                       "  (env: SREMFB_PORT, SREMFB_ALLOW)\n", argv[0]);
            return 2;
        }
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

    /* recreate fresh devices when we can: survivors of a previous
     * instance wedge mutter on reopen. The number comes from the module
     * parameter (what a boot would create) — the *runtime* count is not
     * trustworthy: self-heal leftovers inflate it, an external
     * remove_all deflates it. */
    {
        gchar *cnt = NULL;
        unsigned n = 2;
        if (g_file_get_contents(
                "/sys/module/evdi/parameters/initial_device_count",
                &cnt, NULL, NULL)) {
            n = (unsigned)CLAMP(atoi(cnt), 1, 16);
            g_free(cnt);
        }
        sremfb_evdi_reset(n);
    }

    if (!sremfb_evdi_probe()) {
        g_printerr("no evdi device — load the module first (modprobe evdi, "
                   "package evdi-dkms; initial_device_count in "
                   "/etc/modprobe.d/sremfb.conf sets how many screens can "
                   "connect at once)\n");
        return 1;
    }

    evdi.devices = g_ptr_array_new();
    server.source = &sremfb_evdi_ops;
    server.src = &evdi;

    int rc = sremfb_serve(&server);

    sremfb_evdi_close_all(&server);
    return rc;
}
