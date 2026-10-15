/* SPDX-License-Identifier: GPL-3.0-or-later
 * Connected sockets and draining adapted from nanoDNS (c) Drakmor, GPLv3; the
 * per-upstream deadline replaces its single shared budget. */
#include "dns/forward.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "core/log.h"

int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void fwd_init(forwarder_t *f) {
    memset(f, 0, sizeof *f);
    for (size_t i = 0; i < CFG_MAX_UPSTREAMS; i++)
        f->fd[i] = -1;
}

static void close_slot(forwarder_t *f, size_t i) {
    if (f->fd[i] >= 0) {
        close(f->fd[i]);
        f->fd[i] = -1;
    }
}

void fwd_close(forwarder_t *f) {
    for (size_t i = 0; i < CFG_MAX_UPSTREAMS; i++)
        close_slot(f, i);
}

static int same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

/* The slot's socket, (re)connected to upstream. -1 on failure. */
static int slot_socket(forwarder_t *f, size_t i, const struct sockaddr_in *upstream) {
    if (f->fd[i] >= 0 && same_peer(&f->peer[i], upstream))
        return f->fd[i];
    close_slot(f, i);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        log_printf("upstream socket: %s", strerror(errno));
        return -1;
    }
    if (connect(fd, (const struct sockaddr *)upstream, sizeof *upstream) != 0) {
        log_printf("upstream connect: %s", strerror(errno));
        close(fd);
        return -1;
    }
    f->fd[i] = fd;
    f->peer[i] = *upstream;
    return fd;
}

/* Discards late answers to earlier queries still queued on the socket. */
static void drain(int fd) {
    uint8_t junk[512];
    while (recv(fd, junk, sizeof junk, MSG_DONTWAIT) >= 0) {
    }
}

/* A response to sent: same ID, QR set, and the same single question. */
static int is_answer_to(const uint8_t *resp, size_t n, const uint8_t *sent, size_t question_end) {
    return n >= question_end && resp[0] == sent[0] && resp[1] == sent[1] && (resp[2] & 0x80) &&
           resp[4] == 0 && resp[5] == 1 &&
           memcmp(resp + DNS_HEADER_LEN, sent + DNS_HEADER_LEN, question_end - DNS_HEADER_LEN) == 0;
}

size_t fwd_query(forwarder_t *f, const struct sockaddr_in *upstreams, size_t n, int timeout_ms,
                 const uint8_t *req, size_t len, size_t question_end, uint8_t *out, size_t cap,
                 int *via) {
    static uint8_t sent[DNS_MAX_PACKET]; /* single-threaded caller */
    int64_t start = now_ms();
    if (n > CFG_MAX_UPSTREAMS)
        n = CFG_MAX_UPSTREAMS;
    if (len > sizeof sent || question_end < DNS_HEADER_LEN || question_end > len)
        return 0;

    /* The client chose its ID and so knows it; upstreams see a random one, so
     * forging an answer means guessing it. */
    uint32_t id = arc4random();
    memcpy(sent, req, len);
    sent[0] = (uint8_t)(id >> 8);
    sent[1] = (uint8_t)id;

    for (size_t i = 0; i < n; i++) {
        int64_t deadline = start + (int64_t)timeout_ms * (int64_t)(i + 1) / (int64_t)n;
        int fd = slot_socket(f, i, &upstreams[i]);
        if (fd < 0)
            continue;
        drain(fd);
        if (send(fd, sent, len, 0) < 0) {
            close_slot(f, i);
            continue;
        }

        for (;;) {
            int64_t left = deadline - now_ms();
            if (left <= 0)
                break;
            struct pollfd p = {.fd = fd, .events = POLLIN};
            int rc = poll(&p, 1, (int)left);
            if (rc < 0 && errno == EINTR)
                continue;
            if (rc <= 0)
                break;
            ssize_t got = recv(fd, out, cap, 0);
            if (got < 0) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                close_slot(f, i); /* ECONNREFUSED and friends: try the next one */
                break;
            }
            if ((size_t)got >= cap)
                break; /* may have been cut short: never relay a truncated packet */
            if (is_answer_to(out, (size_t)got, sent, question_end)) {
                out[0] = req[0];
                out[1] = req[1];
                *via = (int)i;
                return (size_t)got;
            }
        }
    }
    return 0;
}
