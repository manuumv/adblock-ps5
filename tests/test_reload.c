/* List reloads: new files are picked up, queries keep getting fast answers
 * while a large set loads, and a failed load keeps the previous set. */
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "server.h"

static config_t cfg;
static server_t srv;
static int client;
static char dir[] = "/tmp/adblock-test-reload-XXXXXX";

static void write_list(const char *name, const char *text) {
    char path[512];
    snprintf(path, sizeof path, "%s/lists/%s", dir, name);
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static size_t build_query(uint8_t *q, uint16_t id, const char *name) {
    memset(q, 0, 12);
    q[0] = (uint8_t)(id >> 8);
    q[1] = (uint8_t)id;
    q[2] = 0x01;
    q[5] = 1;
    size_t off = 12;
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        q[off++] = (uint8_t)l;
        memcpy(q + off, p, l);
        off += l;
        p += l + (dot ? 1 : 0);
    }
    q[off++] = 0;
    q[off++] = 0;
    q[off++] = 1; /* A */
    q[off++] = 0;
    q[off++] = 1;
    return off;
}

/* 1 if the server answers name with the blocked 0.0.0.0; *ms gets the latency. */
static int blocked(const char *name, int64_t *ms) {
    uint8_t q[512], out[512];
    size_t len = build_query(q, 0x4242, name);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)srv.port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int64_t t0 = now_ms();
    assert(sendto(client, q, len, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)len);
    for (int i = 0; i < 40; i++) {
        server_poll(&srv, 25);
        ssize_t n = recv(client, out, sizeof out, MSG_DONTWAIT);
        if (n > 0) {
            if (ms)
                *ms = now_ms() - t0;
            return n == (ssize_t)len + 16 && out[7] == 1;
        }
    }
    assert(!"no answer");
    return 0;
}

static int becomes_blocked(const char *name, int64_t within_ms) {
    int64_t until = now_ms() + within_ms;
    while (now_ms() < until)
        if (blocked(name, NULL))
            return 1;
    return 0;
}

int main(void) {
    assert(mkdtemp(dir));
    char path[512];
    snprintf(path, sizeof path, "%s/lists", dir);
    mkdir(path, 0755);
    write_list("a.txt", "first.example\n");

    inet_pton(AF_INET, "127.0.0.1", &cfg.bind);
    cfg.max_domains = CFG_DEFAULT_MAX_DOMAINS;
    /* Upstream: a bound socket that never answers; blocked names never reach it. */
    int sink = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(sink, (struct sockaddr *)&a, sizeof a);
    socklen_t al = sizeof a;
    getsockname(sink, (struct sockaddr *)&a, &al);
    cfg.upstreams[0] = a;
    cfg.upstream_count = 1;
    cfg.timeout_ms = 100;

    assert(server_open(&srv, &cfg, 0, dir) == 1);
    srv.reload_check_ms = 100;
    client = socket(AF_INET, SOCK_DGRAM, 0);

    /* Initial load. */
    assert(becomes_blocked("first.example", 3000));

    /* A new list file is picked up without a restart. */
    write_list("b.txt", "||second.example^\n");
    assert(becomes_blocked("x.second.example", 3000));
    assert(blocked("first.example", NULL));

    /* While a large set loads, queries are still answered quickly. */
    snprintf(path, sizeof path, "%s/lists/big.txt", dir);
    FILE *f = fopen(path, "w");
    for (int i = 0; i < 400000; i++)
        fprintf(f, "d%06d.big.example\n", i);
    fclose(f);
    int64_t worst = 0, ms = 0;
    int saw_builder = 0;
    int64_t until = now_ms() + 5000;
    while (now_ms() < until && !blocked("d399999.big.example", &ms)) {
        saw_builder |= srv.builder_running;
        assert(blocked("first.example", &ms)); /* old set still active meanwhile */
        if (ms > worst)
            worst = ms;
    }
    assert(blocked("d399999.big.example", NULL));
    printf("  worst answer latency during reload: %lld ms (builder seen: %d)\n", (long long)worst,
           saw_builder);
    assert(worst < 100);

    /* A list that cannot be read: the load fails and the previous set stays. */
    if (geteuid() != 0) { /* root reads anything */
        write_list("c.txt", "third.example\n");
        snprintf(path, sizeof path, "%s/lists/c.txt", dir);
        chmod(path, 0);
        int64_t settle = now_ms() + 1500;
        while (now_ms() < settle)
            server_poll(&srv, 25); /* failed loads are retried at every check meanwhile */
        assert(blocked("first.example", NULL) && blocked("d000001.big.example", NULL));
        assert(!blocked("third.example", NULL));
        chmod(path, 0644);
        assert(becomes_blocked("third.example", 10000)); /* generous for sanitizer builds */
    }

    server_close(&srv);
    close(client);
    close(sink);
    const char *files[] = {"a.txt", "b.txt", "big.txt", "c.txt"};
    for (size_t i = 0; i < 4; i++) {
        snprintf(path, sizeof path, "%s/lists/%s", dir, files[i]);
        unlink(path);
    }
    snprintf(path, sizeof path, "%s/lists", dir);
    rmdir(path);
    rmdir(dir);
    puts("test_reload: ok");
    return 0;
}
