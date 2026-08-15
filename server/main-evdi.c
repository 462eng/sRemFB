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
    unsigned n = 2;
    {
        gchar *cnt = NULL;
        if (g_file_get_contents(
                "/sys/module/evdi/parameters/initial_device_count",
                &cnt, NULL, NULL)) {
            n = (unsigned)CLAMP(atoi(cnt), 1, 16);
            g_free(cnt);
        }
    }
    sremfb_evdi_reset(n);

    /* Cold-boot race: the evdi module and its /sys permissions
     * (sremfb-evdi-perms.service, a *system* unit) may not be ready when
     * we start — we are After=graphical-session.target, they are not.
     * Waiting for them here, instead of exiting into a Restart=always
     * crash loop, keeps a single process patient and re-runs the reset
     * once the permissions become writable. Bounded, so a genuinely
     * missing module still fails cleanly. */
    for (int waited = 0; !sremfb_evdi_probe(); waited++) {
        if (waited == 0)
            g_message("no evdi device yet — waiting for the module and its "
                      "permissions (evdi-dkms + sremfb-evdi-perms)");
        if (waited >= 60) {
            g_printerr("no evdi device after 60s — load the module first "
                       "(modprobe evdi, package evdi-dkms; "
                       "initial_device_count in /etc/modprobe.d/sremfb.conf "
                       "sets how many screens can connect at once)\n");
            return 1;
        }
        g_usleep(1000 * 1000);         /* 1 s */
        sremfb_evdi_reset(n);          /* retry once the perms are up */
    }

    evdi.devices = g_ptr_array_new();
    server.source = &sremfb_evdi_ops;
    server.src = &evdi;

    int rc = sremfb_serve(&server);

    sremfb_evdi_close_all(&server);
    return rc;
}
