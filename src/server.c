/* SPDX-License-Identifier: GPL-3.0-or-later
 * Listener retry and the override/forward flow adapted from nanoDNS (c) Drakmor, GPLv3. */
#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dns.h"
#include "log.h"

#define OVERRIDE_TTL 60

static int open_listener(server_t *s, listener_t *l) {
    struct sockaddr_storage ss;
    socklen_t sl;
    memset(&ss, 0, sizeof ss);
    if (l->family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        a->sin_family = AF_INET;
        a->sin_port = htons((uint16_t)s->port);
        a->sin_addr = s->cfg->bind;
        sl = sizeof *a;
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons((uint16_t)s->port);
        a->sin6_addr = s->cfg->bind6;
        sl = sizeof *a;
    }

    int fd = socket(l->family, SOCK_DGRAM, 0);
    if (fd < 0)
        goto fail;
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (l->family == AF_INET6)
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&ss, sl) != 0) {
        close(fd);
        goto fail;
    }
    if (s->port == 0) {
        sl = sizeof ss;
        if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0)
            s->port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
    }
    l->fd = fd;
    l->retry_at = 0;
    log_printf("listening on %s port %d", l->family == AF_INET ? "IPv4" : "IPv6", s->port);
    return 0;

fail:
    log_printf("%s listener: %s", l->family == AF_INET ? "IPv4" : "IPv6", strerror(errno));
    return -1;
}

static void invalidate(listener_t *l) {
    if (l->fd >= 0) {
        close(l->fd);
        l->fd = -1;
    }
    if (l->enabled)
        l->retry_at = now_ms() + LISTENER_RETRY_MS;
}

int server_open(server_t *s, const config_t *cfg, int port) {
    memset(s, 0, sizeof *s);
    s->cfg = cfg;
    s->port = port;
    fwd_init(&s->fwd);
    s->listeners[0] = (listener_t){.fd = -1, .family = AF_INET, .enabled = 1};
    s->listeners[1] = (listener_t){.fd = -1, .family = AF_INET6};

    int open = 0;
    if (open_listener(s, &s->listeners[0]) == 0)
        open++;
    else
        invalidate(&s->listeners[0]);
    if (cfg->bind6_enabled) {
        if (open_listener(s, &s->listeners[1]) == 0) {
            s->listeners[1].enabled = 1;
            open++;
        } else {
            log_printf("continuing without IPv6");
        }
    }
    return open;
}

void server_close(server_t *s) {
    for (int i = 0; i < 2; i++) {
        s->listeners[i].enabled = 0;
        invalidate(&s->listeners[i]);
    }
    fwd_close(&s->fwd);
}

static int ipv4_local(const uint8_t *a) {
    return a[0] == 127 || a[0] == 10 || (a[0] == 172 && (a[1] & 0xf0) == 16) ||
           (a[0] == 192 && a[1] == 168) || (a[0] == 169 && a[1] == 254);
}

int server_source_allowed(const struct sockaddr *from) {
    if (from->sa_family == AF_INET)
        return ipv4_local((const uint8_t *)&((const struct sockaddr_in *)from)->sin_addr);
    if (from->sa_family != AF_INET6)
        return 0;
    const uint8_t *a = ((const struct sockaddr_in6 *)from)->sin6_addr.s6_addr;
    static const uint8_t loopback[16] = {[15] = 1};
    static const uint8_t mapped[12] = {[10] = 0xff, [11] = 0xff};
    if (!memcmp(a, loopback, 16))
        return 1;
    if (!memcmp(a, mapped, 12))
        return ipv4_local(a + 12);
    return (a[0] & 0xfe) == 0xfc || (a[0] == 0xfe && (a[1] & 0xc0) == 0x80);
}

/* name with anything unprintable replaced, for logs. */
static const char *printable(const char *name) {
    static char out[DNS_NAME_MAX];
    size_t i = 0;
    for (; name[i] && i < sizeof out - 1; i++)
        out[i] = name[i] > ' ' && name[i] < 0x7f ? name[i] : '?';
    out[i] = '\0';
    return out;
}

/* Decides and builds the answer for one packet. Returns its length, 0 for none. */
static size_t answer(server_t *s, const uint8_t *pkt, size_t len, uint8_t *out, size_t cap) {
    const config_t *c = s->cfg;
    dns_query_t q;

    switch (dns_parse_query(pkt, len, &q)) {
    case DNS_DROP:
        return 0;
    case DNS_FORMERR:
        log_printf("malformed query (%zu bytes)", len);
        return dns_error(pkt, DNS_RCODE_FORMERR, out, cap);
    case DNS_NOTIMP:
        return dns_error(pkt, DNS_RCODE_NOTIMP, out, cap);
    }

    if (log_debug_enabled())
        log_printf("query %s type %u", printable(q.qname), q.qtype);

    if (q.qclass == DNS_CLASS_IN && !masks_match_any(c->exceptions, c->exception_count, q.qname)) {
        const rule_t *r = rules_find(c->rules, c->rule_count, q.qname);
        if (r) {
            if (q.qtype == DNS_TYPE_A || q.qtype == DNS_TYPE_ANY)
                return dns_answer(pkt, &q, DNS_RCODE_NOERROR, DNS_TYPE_A, r->ip, 4, OVERRIDE_TTL, out, cap);
            return dns_answer(pkt, &q, DNS_RCODE_NOERROR, 0, NULL, 0, 0, out, cap);
        }
    }

    int via;
    size_t n = fwd_query(&s->fwd, c->upstreams, c->upstream_count, c->timeout_ms, pkt, len,
                         q.question_end, out, cap, &via);
    if (n)
        return n;
    log_printf("all upstreams failed for %s", printable(q.qname));
    return dns_answer(pkt, &q, DNS_RCODE_SERVFAIL, 0, NULL, 0, 0, out, cap);
}

static int transient(int err) {
    switch (err) {
    case EINTR:
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case ECONNREFUSED: /* stale ICMP error for an earlier client */
    case ENOBUFS:
    case ENOMEM:
    case EMFILE:
    case ENFILE:
        return 1;
    default:
        return 0;
    }
}

static void serve_one(server_t *s, listener_t *l) {
    static uint8_t pkt[DNS_MAX_PACKET], out[DNS_MAX_PACKET]; /* single-threaded loop */
    struct sockaddr_storage from;
    socklen_t fl = sizeof from;

    ssize_t n = recvfrom(l->fd, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
    if (n < 0) {
        if (transient(errno))
            return;
        log_printf("listener error: %s, reopening", strerror(errno));
        invalidate(l);
        return;
    }
    if (!server_source_allowed((struct sockaddr *)&from))
        return;
    size_t len = answer(s, pkt, (size_t)n, out, sizeof out);
    if (len)
        (void)sendto(l->fd, out, len, 0, (struct sockaddr *)&from, fl);
}

void server_poll(server_t *s, int timeout_ms) {
    struct pollfd pfd[2];
    listener_t *by_slot[2];
    nfds_t count = 0;
    int64_t now = now_ms();

    for (int i = 0; i < 2; i++) {
        listener_t *l = &s->listeners[i];
        if (l->fd < 0 && l->enabled && l->retry_at && now >= l->retry_at && open_listener(s, l) != 0)
            l->retry_at = now + LISTENER_RETRY_MS;
        if (l->fd >= 0) {
            pfd[count] = (struct pollfd){.fd = l->fd, .events = POLLIN};
            by_slot[count++] = l;
        }
    }

    int rc = poll(count ? pfd : NULL, count, timeout_ms);
    if (rc <= 0)
        return;
    for (nfds_t i = 0; i < count; i++) {
        if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            log_printf("listener lost, reopening");
            invalidate(by_slot[i]);
        } else if (pfd[i].revents & POLLIN) {
            serve_one(s, by_slot[i]);
        }
    }
}
