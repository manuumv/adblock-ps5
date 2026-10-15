#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "server.h"

static config_t cfg;
static server_t srv;
static int client;
static volatile int stop;

/* A fake upstream that answers every query with ARCOUNT low byte = 0x42. */
static void *upstream(void *arg) {
    int fd = *(int *)arg;
    uint8_t buf[512];
    while (!stop) {
        struct sockaddr_storage from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 12)
            continue;
        buf[2] |= 0x80;
        buf[11] = 0x42;
        sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&from, fl);
    }
    return NULL;
}

static int udp_on_loopback(struct sockaddr_in *addr) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = {0, 100 * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    memset(addr, 0, sizeof *addr);
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, (struct sockaddr *)addr, sizeof *addr) == 0);
    socklen_t len = sizeof *addr;
    getsockname(fd, (struct sockaddr *)addr, &len);
    return fd;
}

static size_t build_query(uint8_t *q, uint16_t id, const char *name, uint16_t type) {
    memset(q, 0, 12);
    q[0] = (uint8_t)(id >> 8);
    q[1] = (uint8_t)id;
    q[2] = 0x01;
    q[5] = 1;
    size_t off = 12;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        q[off++] = (uint8_t)l;
        memcpy(q + off, p, l);
        off += l;
        p += l + (dot ? 1 : 0);
    }
    q[off++] = 0;
    q[off++] = (uint8_t)(type >> 8);
    q[off++] = (uint8_t)type;
    q[off++] = 0;
    q[off++] = 1;
    return off;
}

/* Sends pkt to the server, runs the loop until an answer arrives. */
static ssize_t exchange(const uint8_t *pkt, size_t len, uint8_t *out) {
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)srv.port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(sendto(client, pkt, len, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)len);
    for (int i = 0; i < 30; i++) {
        server_poll(&srv, 50);
        ssize_t n = recv(client, out, 512, MSG_DONTWAIT);
        if (n > 0)
            return n;
    }
    return -1;
}

static void test_override_answer(void) {
    uint8_t q[512], out[512];
    size_t len = build_query(q, 0x1111, "x.ads.example", 1);
    ssize_t n = exchange(q, len, out);
    assert(n == (ssize_t)len + 16);
    assert(out[0] == 0x11 && out[1] == 0x11);
    assert((out[3] & 0x0f) == 0 && out[7] == 1);
    assert(memcmp(out + n - 4, "\0\0\0\0", 4) == 0);
}

static void test_override_other_type_nodata(void) {
    uint8_t q[512], out[512];
    size_t len = build_query(q, 0x2222, "x.ads.example", 28);
    ssize_t n = exchange(q, len, out);
    assert(n == (ssize_t)len && out[7] == 0 && (out[3] & 0x0f) == 0);
}

static void test_exception_goes_upstream(void) {
    uint8_t q[512], out[512];
    size_t len = build_query(q, 0x3333, "keep.ads.example", 1);
    ssize_t n = exchange(q, len, out);
    assert(n == (ssize_t)len && out[11] == 0x42);
}

static void test_forwarded(void) {
    uint8_t q[512], out[512];
    size_t len = build_query(q, 0x4444, "example.com", 1);
    ssize_t n = exchange(q, len, out);
    assert(n == (ssize_t)len && out[11] == 0x42 && out[0] == 0x44);
}

static void test_formerr(void) {
    uint8_t q[512], out[512];
    size_t len = build_query(q, 0x5555, "example.com", 1);
    ssize_t n = exchange(q, len - 6, out); /* cut inside the name */
    assert(n == 12 && (out[3] & 0x0f) == 1 && out[0] == 0x55);
}

static void test_short_packet_dropped(void) {
    uint8_t out[512];
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)srv.port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    sendto(client, "abc", 3, 0, (struct sockaddr *)&to, sizeof to);
    server_poll(&srv, 50);
    assert(recv(client, out, sizeof out, MSG_DONTWAIT) < 0);
}

static void test_servfail_when_upstreams_fail(void) {
    struct sockaddr_in dead;
    int fd = udp_on_loopback(&dead); /* bound but never read */
    config_t *c = (config_t *)srv.cfg;
    struct sockaddr_in saved = c->upstreams[0];
    c->upstreams[0] = dead;
    c->timeout_ms = 200;

    uint8_t q[512], out[512];
    size_t len = build_query(q, 0x6666, "example.com", 1);
    ssize_t n = exchange(q, len, out);
    assert(n == (ssize_t)len && (out[3] & 0x0f) == 2);

    c->upstreams[0] = saved;
    c->timeout_ms = 1000;
    close(fd);
}

static void test_listener_reopens(void) {
    /* Replace the listener with something that is no longer a socket, as a
     * console suspend can leave it: readable, but recvfrom fails (ENOTSOCK). */
    int p[2];
    assert(pipe(p) == 0);
    assert(write(p[1], "x", 1) == 1);
    assert(dup2(p[0], srv.listeners[0].fd) >= 0);
    close(p[0]);
    close(p[1]);

    uint8_t q[512], out[512];
    size_t len = build_query(q, 0x7777, "x.ads.example", 1);
    int64_t until = now_ms() + 3000;
    ssize_t n = -1;
    while (n < 0 && now_ms() < until) {
        server_poll(&srv, 100);
        if (srv.listeners[0].fd >= 0)
            n = exchange(q, len, out);
    }
    assert(n == (ssize_t)len + 16 && out[0] == 0x77);
}

static int allowed4(const char *ip) {
    struct sockaddr_in a = {.sin_family = AF_INET};
    inet_pton(AF_INET, ip, &a.sin_addr);
    return server_source_allowed((struct sockaddr *)&a);
}

static int allowed6(const char *ip) {
    struct sockaddr_in6 a = {.sin6_family = AF_INET6};
    inet_pton(AF_INET6, ip, &a.sin6_addr);
    return server_source_allowed((struct sockaddr *)&a);
}

static void test_source_acl(void) {
    /* Only the console itself and the local network may query: an open
     * resolver on the internet side would be a reflection amplifier. */
    assert(allowed4("127.0.0.1") && allowed4("10.1.2.3") && allowed4("172.16.0.1"));
    assert(allowed4("172.31.255.254") && allowed4("192.168.1.20") && allowed4("169.254.3.4"));
    assert(!allowed4("8.8.8.8") && !allowed4("172.32.0.1") && !allowed4("192.169.0.1"));
    assert(!allowed4("100.64.0.1"));
    assert(allowed6("::1") && allowed6("fe80::1") && allowed6("fd12:3456::1"));
    assert(allowed6("::ffff:192.168.1.20") && !allowed6("::ffff:8.8.8.8"));
    assert(!allowed6("2001:db8::1"));
}

int main(void) {
    test_source_acl();
    struct sockaddr_in up_addr;
    int up_fd = udp_on_loopback(&up_addr);
    pthread_t up_thread;
    assert(pthread_create(&up_thread, NULL, upstream, &up_fd) == 0);

    inet_pton(AF_INET, "127.0.0.1", &cfg.bind);
    cfg.bind6_enabled = 0;
    cfg.upstreams[0] = up_addr;
    cfg.upstream_count = 1;
    cfg.timeout_ms = 1000;
    snprintf(cfg.rules[0].mask, DNS_NAME_MAX, "*.ads.example");
    cfg.rule_count = 1;
    snprintf(cfg.exceptions[0], DNS_NAME_MAX, "keep.ads.example");
    cfg.exception_count = 1;

    assert(server_open(&srv, &cfg, 0) == 1);
    assert(srv.port > 0);
    struct sockaddr_in caddr;
    client = udp_on_loopback(&caddr);

    test_override_answer();
    test_override_other_type_nodata();
    test_exception_goes_upstream();
    test_forwarded();
    test_formerr();
    test_short_packet_dropped();
    test_servfail_when_upstreams_fail();
    test_listener_reopens();

    server_close(&srv);
    stop = 1;
    pthread_join(up_thread, NULL);
    close(up_fd);
    close(client);
    puts("test_server: ok");
    return 0;
}
