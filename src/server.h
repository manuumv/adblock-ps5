/* SPDX-License-Identifier: GPL-3.0-or-later
 * The DNS server loop: listeners, per-query decision, answers, list reloads. */
#ifndef ADBLOCK_SERVER_H
#define ADBLOCK_SERVER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/socket.h>

#include "blocklist.h"
#include "config.h"
#include "forward.h"

#define LISTENER_RETRY_MS 1000
#define RELOAD_CHECK_MS 30000

typedef struct {
    int fd;          /* -1 while closed */
    int family;      /* AF_INET or AF_INET6 */
    int enabled;     /* reopen after failures */
    int64_t retry_at; /* when to try reopening, 0 = not scheduled */
} listener_t;

typedef struct {
    uint64_t total;       /* parsed queries */
    uint64_t blocked;
    uint64_t allowlisted; /* matched the allowlist or an exception (then forwarded) */
    uint64_t overridden;
    uint64_t forwarded;   /* sent upstream, allowlisted ones included */
} server_stats_t;

typedef struct {
    const config_t *cfg;
    forwarder_t fwd;
    int port;
    listener_t listeners[2]; /* IPv4, IPv6 */
    server_stats_t stats;

    /* Lists. Only the DNS thread reads or replaces active; a builder thread
     * loads the next set into pending and then sets builder_done. */
    const char *data_dir; /* NULL: no lists */
    blocklist_t *active;
    blocklist_t *pending;
    pthread_t builder;
    int builder_running;
    atomic_int builder_done;
    int reload_check_ms;
    int64_t next_check;
    uint64_t loaded_print;
    int loaded_once;
} server_t;

typedef enum {
    VERDICT_FORWARD,
    VERDICT_ALLOWLISTED,
    VERDICT_OVERRIDE,
    VERDICT_BLOCKED,
} verdict_t;

/* Opens the listeners. A listener that fails to open is retried every
 * LISTENER_RETRY_MS (IPv6 only if it opened once). Port 0 picks a free port,
 * kept for reopening. Lists are loaded from data_dir in the background and
 * reloaded when they change. Returns the number of listeners open. */
int server_open(server_t *s, const config_t *cfg, int port, const char *data_dir);

/* One loop iteration: reopens due listeners, waits up to timeout_ms for
 * queries and answers them, and swaps in freshly loaded lists. */
void server_poll(server_t *s, int timeout_ms);

void server_close(server_t *s);

/* Decision for a normalized name, in order: allowlist and exceptions,
 * overrides (*rule set), lists and preset masks, else forward. b may be NULL. */
verdict_t server_decide(const config_t *c, const blocklist_t *b, const char *name,
                        const rule_t **rule);

/* 1 if a query from this address may be answered: loopback, private
 * (10/8, 172.16/12, 192.168/16, fc00::/7) or link-local. Keeps the console
 * from acting as an open resolver when bound to 0.0.0.0. */
int server_source_allowed(const struct sockaddr *from);

#endif
