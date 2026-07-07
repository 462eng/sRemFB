#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sremfb-server.h"

int net_listen(uint16_t port)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    int v6only = 0;
    struct sockaddr_in6 addr = {0};

    if (fd < 0) {
        /* no IPv6 support: fall back to IPv4 */
        struct sockaddr_in a4 = {0};
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        a4.sin_family = AF_INET;
        a4.sin_addr.s_addr = htonl(INADDR_ANY);
        a4.sin_port = htons(port);
        if (bind(fd, (struct sockaddr *)&a4, sizeof(a4)) < 0 ||
            listen(fd, 4) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    /* dual-stack: accept IPv4 too */
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* --------------------------------------------------------- allowlist */

/* Parses "192.0.2.0/24, 198.51.100.5" (comma/space separated IPv4 CIDRs;
 * a bare address means /32) into srv->allow. */
gboolean net_allow_parse(SremfbServer *srv, const char *spec)
{
    gchar **parts = g_strsplit_set(spec, ", \t", -1);
    GArray *nets = g_array_new(FALSE, FALSE, sizeof(SremfbAllowNet));
    gboolean ok = TRUE;

    for (gchar **p = parts; *p; p++) {
        if (**p == '\0')
            continue;
        gchar *slash = strchr(*p, '/');
        long prefix = 32;
        if (slash) {
            *slash = '\0';
            char *end = NULL;
            prefix = strtol(slash + 1, &end, 10);
            if (!end || *end || prefix < 0 || prefix > 32) {
                ok = FALSE;
                break;
            }
        }
        struct in_addr a;
        if (inet_pton(AF_INET, *p, &a) != 1) {
            ok = FALSE;
            break;
        }
        SremfbAllowNet net = {
            .addr = a.s_addr,
            .mask = prefix == 0 ? 0 : htonl(~0u << (32 - prefix)),
        };
        g_array_append_val(nets, net);
    }
    g_strfreev(parts);

    if (!ok || nets->len == 0) {
        g_array_free(nets, TRUE);
        return FALSE;
    }
    srv->n_allow = nets->len;
    srv->allow = (SremfbAllowNet *)g_array_free(nets, FALSE);
    return TRUE;
}

/* The allowlist is IPv4-only (the wire protocol targets a dedicated IPv4
 * LAN): v4-mapped addresses from the dual-stack listener are unwrapped,
 * the IPv6 loopback counts as 127.0.0.1, anything else v6 is denied when
 * an allowlist is set. */
static gboolean peer_allowed(const SremfbServer *srv,
                             const struct sockaddr_storage *ss)
{
    uint32_t v4;

    if (srv->n_allow == 0)
        return TRUE;

    if (ss->ss_family == AF_INET) {
        v4 = ((const struct sockaddr_in *)ss)->sin_addr.s_addr;
    } else if (ss->ss_family == AF_INET6) {
        const struct in6_addr *a6 =
            &((const struct sockaddr_in6 *)ss)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(a6))
            memcpy(&v4, a6->s6_addr + 12, 4);
        else if (IN6_IS_ADDR_LOOPBACK(a6))
            v4 = htonl(INADDR_LOOPBACK);
        else
            return FALSE;
    } else {
        return FALSE;
    }

    for (unsigned i = 0; i < srv->n_allow; i++)
        if ((v4 & srv->allow[i].mask) ==
            (srv->allow[i].addr & srv->allow[i].mask))
            return TRUE;
    return FALSE;
}

static void peer_to_string(const struct sockaddr_storage *ss,
                           char *out, size_t out_len)
{
    char host[INET6_ADDRSTRLEN] = "?";
    uint16_t port = 0;

    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)ss;
        inet_ntop(AF_INET, &a->sin_addr, host, sizeof(host));
        port = ntohs(a->sin_port);
    } else if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)ss;
        if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) {
            struct in_addr v4;
            memcpy(&v4, a->sin6_addr.s6_addr + 12, 4);
            inet_ntop(AF_INET, &v4, host, sizeof(host));
        } else {
            inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof(host));
        }
        port = ntohs(a->sin6_port);
    }
    g_snprintf(out, out_len, "%s:%u", host, port);
}

/* ------------------------------------------------------------- wire */

static gboolean readn(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n == 0)
            return FALSE;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return FALSE;
        }
        p += n;
        len -= (size_t)n;
    }
    return TRUE;
}

/* Blocking-style send, only used for the pre-stream hellos (the socket
 * may already be non-blocking: wait for writability on EAGAIN). */
gboolean net_send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pf = { .fd = fd, .events = POLLOUT };
                if (poll(&pf, 1, 5000) <= 0)
                    return FALSE;
                continue;
            }
            return FALSE;
        }
        p += n;
        len -= (size_t)n;
    }
    return TRUE;
}

/* One non-blocking send: >0 bytes written, 0 on EAGAIN, -1 fatal. */
ssize_t net_send_some(int fd, const void *buf, size_t len)
{
    for (;;) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n >= 0)
            return n;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
}

/* Accepts a pending connection, enforces the allowlist and performs the
 * (short, blocking) client hello read. Returns the configured client fd,
 * or -1. */
int net_accept_and_hello(SremfbServer *srv, int listen_fd,
                         struct sremfb_client_hello *hello,
                         char *peer, size_t peer_len)
{
    struct sockaddr_storage ss = {0};
    socklen_t sl = sizeof(ss);

    int fd = accept(listen_fd, (struct sockaddr *)&ss, &sl);
    if (fd < 0)
        return -1;
    peer_to_string(&ss, peer, peer_len);

    if (!peer_allowed(srv, &ss)) {
        g_message("refusing %s: not in SREMFB_ALLOW", peer);
        close(fd);
        return -1;
    }

    int one = 1;
    int idle = 10, intvl = 5, cnt = 3;
    int user_to = 6000;        /* ms: unACKed data for 6 s = link dead —
                                  kills the connection even while a big
                                  blocking send is in flight, so the
                                  virtual monitor unplugs fast */
    struct timeval snd_to = { .tv_sec = 20 };
    struct timeval rcv_to = { .tv_sec = 5 };

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    int sndbuf = 256 * 1024;   /* modest kernel buffer: the transmit
                                  queue (xmit.c) coalesces frames as
                                  soon as the socket pushes back, so a
                                  slow client gets *fresh* pixels
                                  instead of a deep stale backlog */
    setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_to, sizeof(user_to));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    if (!readn(fd, hello, sizeof(*hello))) {
        g_message("client hello read failed (%s)", peer);
        close(fd);
        return -1;
    }
    /* streaming writes are non-blocking from here on (xmit.c) */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

void net_fill_server_hello(struct sremfb_server_hello *sh,
                           uint16_t width, uint16_t height,
                           uint8_t pixfmt, uint16_t status, uint8_t flags)
{
    memset(sh, 0, sizeof(*sh));
    sh->magic = SREMFB_MAGIC;
    sh->proto_ver = SREMFB_PROTO_VER;
    sh->status = status;
    sh->width = width;
    sh->height = height;
    sh->pixfmt = pixfmt;
    sh->flags = flags;
}

gboolean net_send_server_hello(int fd, uint16_t width, uint16_t height,
                               uint8_t pixfmt, uint16_t status,
                               uint8_t flags)
{
    struct sremfb_server_hello sh;

    net_fill_server_hello(&sh, width, height, pixfmt, status, flags);
    return net_send_all(fd, &sh, sizeof(sh));
}
