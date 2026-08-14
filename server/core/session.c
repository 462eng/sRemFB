/*
 * Source-agnostic client session: accept + hello, the per-client TCP
 * lifecycle, USB-teleport peer files, and the listen/main-loop driver.
 * Where pixels come from is the frame source's business (srv->source):
 * this file only claims/releases the source and shuttles bytes.
 *
 * Lifecycle, per client: connect + hello (geometry + MAC) -> the source
 * is acquired (EVDI: a free device is claimed and an EDID plugged in ->
 * the compositor lights it up like a real monitor, position restored from
 * monitors.xml — the EDID serial derives from the client's MAC) ->
 * damage-driven frames stream until the client goes away -> the source is
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

#include "core.h"

static gboolean mac_is_zero(const uint8_t mac[6])
{
    static const uint8_t zero[6];
    return memcmp(mac, zero, 6) == 0;
}

/* ------------------------------------------------------ USB teleport */

/* The client exports USB devices over usbip (hello flag bit): a peer
 * file in /run/sremfb materializes "this client is streaming". The
 * vhci attach needs root and we run as the session user, so a root-side
 * systemd unit shipped with the package (sremfb-usb.{path,timer} ->
 * /usr/libexec/sremfb-usb-attach) reconciles the attachments with these
 * files: attach what streaming peers export, detach what left. */
#define SREMFB_USB_RUNDIR "/run/sremfb"

static void usb_peer_path(const SremfbClient *c, char *out, size_t len)
{
    char ip[64];
    char *colon;

    g_strlcpy(ip, c->peer, sizeof(ip));
    colon = strrchr(ip, ':');          /* peer is "ip:port" */
    if (colon)
        *colon = '\0';
    g_snprintf(out, len, SREMFB_USB_RUNDIR "/usb-%s", ip);
}

void sremfb_usb_peer_add(SremfbClient *c)
{
    char path[128];

    if (!c->usb_cap || getenv("SREMFB_NO_USB"))
        return;
    usb_peer_path(c, path, sizeof(path));
    if (!g_file_set_contents(path, c->macstr, -1, NULL)) {
        static gboolean warned;
        if (!warned) {
            warned = TRUE;
            g_message("[%s] cannot write %s — USB teleport needs the "
                      "packaged /run/sremfb dir and sremfb-usb units",
                      c->macstr, path);
        }
        return;
    }
    g_message("[%s] usb devices announced, attaching from %s", c->macstr,
              c->peer);
}

void sremfb_usb_peer_remove(SremfbClient *c)
{
    char path[128];

    if (!c->usb_cap)
        return;
    usb_peer_path(c, path, sizeof(path));
    unlink(path);
}

/* Server (re)start: no client streams yet, drop stale peers so the
 * reconciler detaches leftovers from a previous run. */
static void usb_peers_clear(void)
{
    GDir *d = g_dir_open(SREMFB_USB_RUNDIR, 0, NULL);
    const char *n;

    if (!d)
        return;
    while ((n = g_dir_read_name(d)))
        if (g_str_has_prefix(n, "usb-")) {
            gchar *p = g_build_filename(SREMFB_USB_RUNDIR, n, NULL);
            unlink(p);
            g_free(p);
        }
    g_dir_close(d);
}

/* Client socket died (or never got a working monitor): release the frame
 * source and forget the client. For EVDI the position is safe in
 * monitors.xml thanks to the MAC-derived EDID identity. */
void sremfb_client_lost(SremfbClient *c)
{
    g_clear_handle_id(&c->lost_id, g_source_remove);
    g_clear_handle_id(&c->watch_id, g_source_remove);
    sremfb_usb_peer_remove(c);
    sremfb_xmit_reset(c);
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    c->srv->source->release(c);
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

/* Upstream traffic: PONG echoes from feedback-capable clients (parsed and
 * fed to the pressure controller), anything else skipped byte-wise until
 * a magic lines back up (the same resync guard as the frame stream). */
static void client_parse_upstream(SremfbClient *c)
{
    for (;;) {
        /* hunt for the magic at the buffer start */
        while (c->recvlen >= 4 &&
               memcmp(c->recvbuf, &(uint32_t){SREMFB_MAGIC}, 4) != 0) {
            memmove(c->recvbuf, c->recvbuf + 1, --c->recvlen);
            if (++c->recv_garbage > 256) {
                g_message("[%s] upstream garbage, dropping", c->macstr);
                sremfb_schedule_client_lost(c);
                return;
            }
        }
        if (c->recvlen < sizeof(struct sremfb_client_msg))
            return;

        struct sremfb_client_msg msg;
        memcpy(&msg, c->recvbuf, sizeof(msg));
        c->recvlen -= sizeof(msg);
        memmove(c->recvbuf, c->recvbuf + sizeof(msg), c->recvlen);
        c->recv_garbage = 0;

        if (msg.type == SREMFB_CMSG_PONG && c->feedback)
            sremfb_ctl_on_pong(c, msg.t_echo_us);
        /* unknown types: ignore (forward compat) */
    }
}

static gboolean on_client_io(gint fd, GIOCondition cond, gpointer data)
{
    SremfbClient *c = data;
    ssize_t n;

    if (cond & G_IO_IN) {
        n = read(fd, c->recvbuf + c->recvlen,
                 sizeof(c->recvbuf) - c->recvlen);
        if (n > 0) {
            c->recvlen += (size_t)n;
            client_parse_upstream(c);
            return G_SOURCE_CONTINUE;
        }
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
        net_send_server_hello(cfd, 0, 0, 0, SREMFB_STATUS_BAD_HELLO, 0);
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
        net_send_server_hello(cfd, 0, 0, 0, SREMFB_STATUS_NO_DEVICE, 0);
        close(cfd);
        return G_SOURCE_CONTINUE;
    }

    SremfbClient *c = g_new0(SremfbClient, 1);
    c->srv = srv;
    c->fd = cfd;
    c->hello = hello;
    c->lz4 = (hello.flags & SREMFB_HELLO_FLAG_LZ4) != 0;
    c->feedback = (hello.flags & SREMFB_HELLO_FLAG_FEEDBACK) != 0;
    c->h264_cap = (hello.flags & SREMFB_HELLO_FLAG_H264) != 0;
    c->usb_cap = (hello.flags & SREMFB_HELLO_FLAG_USB) != 0;
    g_strlcpy(c->peer, peer, sizeof(c->peer));
    g_snprintf(c->macstr, sizeof(c->macstr),
               "%02x:%02x:%02x:%02x:%02x:%02x",
               hello.mac[0], hello.mac[1], hello.mac[2],
               hello.mac[3], hello.mac[4], hello.mac[5]);

    g_message("[%s] client %s: fb %ux%u %ubpp pixfmt %u lz4=%c ping=%c "
              "h264=%c usb=%c model \"%.13s\"",
              c->macstr, peer, hello.xres, hello.yres, hello.bpp,
              hello.pixfmt, c->lz4 ? 'y' : 'n', c->feedback ? 'y' : 'n',
              c->h264_cap ? 'y' : 'n', c->usb_cap ? 'y' : 'n',
              hello.model[0] ? hello.model : "(none)");

    int st = srv->source->acquire(c);
    if (st != SREMFB_STATUS_OK) {
        net_send_server_hello(cfd, 0, 0, 0, (uint16_t)st, 0);
        close(cfd);
        g_free(c);
        return G_SOURCE_CONTINUE;
    }

    g_ptr_array_add(srv->clients, c);
    c->watch_id = g_unix_fd_add(cfd, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                on_client_io, c);
    return G_SOURCE_CONTINUE;
}

static gboolean on_shutdown_signal(gpointer data)
{
    SremfbServer *srv = data;

    g_message("shutting down");
    g_main_loop_quit(srv->loop);
    return G_SOURCE_REMOVE;
}

/* Listen, run the main loop, tear down. The frame source (srv->source /
 * srv->src) and srv->port must already be set up by the binary's main().
 * Returns a process exit code. */
int sremfb_serve(SremfbServer *srv)
{
    usb_peers_clear();
    srv->clients = g_ptr_array_new();

    srv->listen_fd = net_listen(srv->port);
    if (srv->listen_fd < 0) {
        g_printerr("cannot listen on port %u: %s\n", srv->port,
                   g_strerror(errno));
        return 1;
    }
    srv->listen_watch_id = g_unix_fd_add(srv->listen_fd, G_IO_IN,
                                         on_listen_ready, srv);

    g_unix_signal_add(SIGINT, on_shutdown_signal, srv);
    g_unix_signal_add(SIGTERM, on_shutdown_signal, srv);

    if (srv->n_allow)
        g_message("allowlist active (%u net%s)", srv->n_allow,
                  srv->n_allow > 1 ? "s" : "");
    else
        g_message("no allowlist — accepting any address");
    g_message("sremfb-server listening on port %u", srv->port);
    srv->loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(srv->loop);

    while (srv->clients->len > 0)
        sremfb_client_lost(g_ptr_array_index(srv->clients, 0));
    g_ptr_array_free(srv->clients, TRUE);
    if (srv->listen_watch_id)
        g_source_remove(srv->listen_watch_id);
    if (srv->listen_fd >= 0)
        close(srv->listen_fd);
    g_main_loop_unref(srv->loop);
    g_free(srv->allow);
    return 0;
}
