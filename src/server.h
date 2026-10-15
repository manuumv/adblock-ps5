/* SPDX-License-Identifier: GPL-3.0-or-later
 * The DNS server loop: listeners, per-query decision, answers. */
#ifndef ADBLOCK_SERVER_H
#define ADBLOCK_SERVER_H

#include <stdint.h>
#include <sys/socket.h>

#include "config.h"
#include "forward.h"

#define LISTENER_RETRY_MS 1000

typedef struct {
    int fd;          /* -1 while closed */
    int family;      /* AF_INET or AF_INET6 */
    int enabled;     /* reopen after failures */
    int64_t retry_at; /* when to try reopening, 0 = not scheduled */
} listener_t;

typedef struct {
    const config_t *cfg;
    forwarder_t fwd;
    int port;
    listener_t listeners[2]; /* IPv4, IPv6 */
} server_t;

/* Opens the listeners. A listener that fails to open is retried every
 * LISTENER_RETRY_MS (IPv6 only if it opened once). Port 0 picks a free port,
 * kept for reopening. Returns the number of listeners open. */
int server_open(server_t *s, const config_t *cfg, int port);

/* One loop iteration: reopens due listeners, waits up to timeout_ms for
 * queries and answers them. */
void server_poll(server_t *s, int timeout_ms);

void server_close(server_t *s);

/* 1 if a query from this address may be answered: loopback, private
 * (10/8, 172.16/12, 192.168/16, fc00::/7) or link-local. Keeps the console
 * from acting as an open resolver when bound to 0.0.0.0. */
int server_source_allowed(const struct sockaddr *from);

#endif
