/*
 * sremfb-server — exposes one EVDI virtual connector per connected
 * sremfb-client, over a single TCP port.
 *
 * Lifecycle, per client: connect + hello (geometry + MAC) -> a free EVDI
 * device is claimed and an EDID at that size is plugged in -> the
 * compositor lights it up like a real monitor (hotplug, position restored
 * from monitors.xml — the EDID serial derives from the client's MAC, so
 * each physical client keeps its own position) -> damage-driven frames
 * stream until the client goes away -> connector unplugged, device
 * released, like a cable being pulled.
 *
 * Config: --port N / SREMFB_PORT (default 4629),
 *         --allow CIDRs / SREMFB_ALLOW (IPv4 allowlist, empty = everyone).
 */
#include <errno.h>
#include <glib-unix.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sremfb-server.h"

static SremfbServer server = {
    .listen_fd = -1,
};

static gboolean mac_is_zero(const uint8_t mac[6])
{
    static const uint8_t zero[6];
    return memcmp(mac, zero, 6) == 0;
}

/* Client socket died (or never got a working monitor): unplug the
 * connector, release the EVDI device and forget the client. Its position
 * is safe in monitors.xml thanks to the MAC-derived EDID identity. */
void sremfb_client_lost(SremfbClient *c)
{
    g_clear_handle_id(&c->lost_id, g_source_remove);
    g_clear_handle_id(&c->watch_id, g_source_remove);
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    sremfb_evdi_unplug(c);
    sremfb_evdi_release(c);
    g_ptr_array_remove(c->srv->clients, c);
    g_message("[%s] client gone (%u still connected)", c->macstr,
              c->srv->clients->len);
    g_free(c);
}

static gboolean client_lost_idle(gpointer data)
{
    SremfbClient *c = data;

    c->lost_id = 0;
    sremfb_client_lost(c);
    return G_SOURCE_REMOVE;
}

/* Safe to call from within evdi event dispatch: defers the teardown
 * (which unregisters the grab buffer) to the main loop. */
void sremfb_schedule_client_lost(SremfbClient *c)
{
    if (!c->lost_id)
        c->lost_id = g_idle_add(client_lost_idle, c);
}

static gboolean on_client_io(gint fd, GIOCondition cond, gpointer data)
{
    SremfbClient *c = data;
    uint8_t scratch[256];
    ssize_t n;

    if (cond & G_IO_IN) {
        n = read(fd, scratch, sizeof(scratch));
        if (n > 0)
            return G_SOURCE_CONTINUE;   /* clients send nothing; ignore */
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            return G_SOURCE_CONTINUE;
    }
    /* EOF, error or HUP */
    c->watch_id = 0;
    sremfb_client_lost(c);
    return G_SOURCE_REMOVE;
}

static gboolean hello_valid(const struct sremfb_client_hello *h)
{
    if (h->magic != SREMFB_MAGIC || h->proto_ver != SREMFB_PROTO_VER)
        return FALSE;
    if (h->xres == 0 || h->yres == 0)
        return FALSE;
    if (h->pixfmt == SREMFB_PIX_XRGB8888 && h->bpp == 32)
        return TRUE;
    if (h->pixfmt == SREMFB_PIX_RGB565 && h->bpp == 16)
        return TRUE;
    return FALSE;
}

static SremfbClient *find_client_by_mac(SremfbServer *srv,
                                        const uint8_t mac[6])
{
    for (guint i = 0; i < srv->clients->len; i++) {
        SremfbClient *c = g_ptr_array_index(srv->clients, i);
        if (memcmp(c->hello.mac, mac, 6) == 0)
            return c;
    }
    return NULL;
}

static gboolean on_listen_ready(gint fd, GIOCondition cond, gpointer data)
{
    SremfbServer *srv = data;
    struct sremfb_client_hello hello;
    char peer[64];

    (void)cond;
    int cfd = net_accept_and_hello(srv, fd, &hello, peer, sizeof(peer));
    if (cfd < 0)
        return G_SOURCE_CONTINUE;

    if (!hello_valid(&hello)) {
        g_message("invalid client hello from %s, dropping", peer);
        net_send_server_hello(cfd, 0, 0, 0, SREMFB_STATUS_BAD_HELLO);
        close(cfd);
        return G_SOURCE_CONTINUE;
    }

    /* Same MAC reconnecting = the SBC came back before TCP keepalive
     * noticed the old connection died: replace it, like re-plugging the
     * same cable. */
    if (!mac_is_zero(hello.mac)) {
        SremfbClient *old = find_client_by_mac(srv, hello.mac);
        if (old) {
            g_message("[%s] replaced by a new connection from %s",
                      old->macstr, peer);
            sremfb_client_lost(old);
        }
    }

    if (srv->clients->len >= SREMFB_MAX_CLIENTS) {
        g_message("refusing %s: %u clients already connected", peer,
                  srv->clients->len);
        net_send_server_hello(cfd, 0, 0, 0, SREMFB_STATUS_NO_DEVICE);
        close(cfd);
        return G_SOURCE_CONTINUE;
    }

    SremfbClient *c = g_new0(SremfbClient, 1);
    c->srv = srv;
    c->fd = cfd;
    c->hello = hello;
    c->lz4 = (hello.flags & SREMFB_HELLO_FLAG_LZ4) != 0;
    g_strlcpy(c->peer, peer, sizeof(c->peer));
    g_snprintf(c->macstr, sizeof(c->macstr),
               "%02x:%02x:%02x:%02x:%02x:%02x",
               hello.mac[0], hello.mac[1], hello.mac[2],
               hello.mac[3], hello.mac[4], hello.mac[5]);

    g_message("[%s] client %s: fb %ux%u %ubpp pixfmt %u lz4=%c "
              "model \"%.13s\"",
              c->macstr, peer, hello.xres, hello.yres, hello.bpp,
              hello.pixfmt, c->lz4 ? 'y' : 'n',
              hello.model[0] ? hello.model : "(none)");

    if (!sremfb_evdi_acquire(c)) {
        g_message("[%s] no free evdi device — raise initial_device_count "
                  "in /etc/modprobe.d/sremfb.conf (or "
                  "echo 1 > /sys/devices/evdi/add)", c->macstr);
        net_send_server_hello(cfd, 0, 0, 0, SREMFB_STATUS_NO_DEVICE);
        close(cfd);
        g_free(c);
        return G_SOURCE_CONTINUE;
    }

    g_ptr_array_add(srv->clients, c);
    c->watch_id = g_unix_fd_add(cfd, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                on_client_io, c);
    sremfb_evdi_plug(c);
    return G_SOURCE_CONTINUE;
}

static gboolean on_shutdown_signal(gpointer data)
{
    SremfbServer *srv = data;

    g_message("shutting down");
    g_main_loop_quit(srv->loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
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

    /* recreate fresh devices (preserving their number) when we can:
     * survivors of a previous instance wedge mutter on reopen */
    {
        gchar *cnt = NULL;
        unsigned n = 2;
        if (g_file_get_contents("/sys/devices/evdi/count", &cnt, NULL, NULL)) {
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

    server.clients = g_ptr_array_new();
    server.devices = g_ptr_array_new();

    server.listen_fd = net_listen((uint16_t)port);
    if (server.listen_fd < 0) {
        g_printerr("cannot listen on port %ld: %s\n", port,
                   g_strerror(errno));
        return 1;
    }
    server.listen_watch_id = g_unix_fd_add(server.listen_fd, G_IO_IN,
                                           on_listen_ready, &server);

    g_unix_signal_add(SIGINT, on_shutdown_signal, &server);
    g_unix_signal_add(SIGTERM, on_shutdown_signal, &server);

    if (server.n_allow)
        g_message("allowlist active: %s", allow);
    else
        g_message("no allowlist — accepting any address");
    g_message("sremfb-server listening on port %ld", port);
    server.loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(server.loop);

    while (server.clients->len > 0)
        sremfb_client_lost(g_ptr_array_index(server.clients, 0));
    g_ptr_array_free(server.clients, TRUE);
    sremfb_evdi_close_all(&server);
    if (server.listen_watch_id)
        g_source_remove(server.listen_watch_id);
    if (server.listen_fd >= 0)
        close(server.listen_fd);
    g_main_loop_unref(server.loop);
    g_free(server.allow);
    return 0;
}
