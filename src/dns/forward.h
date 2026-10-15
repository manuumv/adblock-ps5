/* SPDX-License-Identifier: GPL-3.0-or-later
 * Forwarding queries to upstream resolvers over UDP. */
#ifndef ADBLOCK_FORWARD_H
#define ADBLOCK_FORWARD_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#include "core/config.h"

typedef struct {
    int fd[CFG_MAX_UPSTREAMS]; /* one connected socket per upstream, -1 until first use */
    struct sockaddr_in peer[CFG_MAX_UPSTREAMS]; /* what fd[i] is connected to */
    uint8_t sent[DNS_MAX_PACKET]; /* the query as sent, with its random ID */
} forwarder_t;

/* One forwarder per thread: it is not safe to share. */

void fwd_init(forwarder_t *f);
void fwd_close(forwarder_t *f);

/* Sends the query to each upstream in order, under a fresh random ID, until
 * one returns a response with that ID and the same question (bytes 12 to
 * question_end of req). Upstream i may use time up to timeout_ms * (i + 1) / n
 * from the start, so a silent upstream only consumes its share. Returns the
 * response length written to out, carrying the client's original ID (and the
 * upstream's index in *via), or 0 if none answered. */
size_t fwd_query(forwarder_t *f, const struct sockaddr_in *upstreams, size_t n, int timeout_ms,
                 const uint8_t *req, size_t len, size_t question_end, uint8_t *out, size_t cap,
                 int *via);

/* Monotonic milliseconds. */
int64_t now_ms(void);

#endif
