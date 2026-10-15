#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "forward.h"

enum mode { SILENT, ANSWER, WRONG_ID, WRONG_THEN_RIGHT, WRONG_QUESTION, ID_SPY };

static uint8_t seen_id[2]; /* the ID the ID_SPY upstream received */

typedef struct {
    int fd;
    enum mode mode;
    struct sockaddr_in addr;
    pthread_t thread;
} fake_upstream_t;

static volatile int stop;

static void *serve(void *arg) {
    fake_upstream_t *u = arg;
    uint8_t buf[512];
    while (!stop) {
        struct sockaddr_storage from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(u->fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || u->mode == SILENT)
            continue;
        buf[2] |= 0x80; /* QR */
        if (u->mode == ID_SPY)
            memcpy(seen_id, buf, 2);
        if (u->mode == WRONG_QUESTION)
            buf[13] ^= 0x20; /* flip the case of the first name byte */
        if (u->mode == WRONG_ID || u->mode == WRONG_THEN_RIGHT) {
            buf[1] ^= 0xff;
            sendto(u->fd, buf, (size_t)n, 0, (struct sockaddr *)&from, fl);
            buf[1] ^= 0xff;
        }
        if (u->mode != WRONG_ID) {
            buf[3] = 0x80; /* RA */
            buf[11] = 0x42; /* marker in ARCOUNT low byte */
            sendto(u->fd, buf, (size_t)n, 0, (struct sockaddr *)&from, fl);
        }
    }
    return NULL;
}

static void start(fake_upstream_t *u, enum mode mode) {
    u->mode = mode;
    u->fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(u->fd >= 0);
    struct timeval tv = {0, 100 * 1000};
    setsockopt(u->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    memset(&u->addr, 0, sizeof u->addr);
    u->addr.sin_family = AF_INET;
    u->addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(u->fd, (struct sockaddr *)&u->addr, sizeof u->addr) == 0);
    socklen_t len = sizeof u->addr;
    getsockname(u->fd, (struct sockaddr *)&u->addr, &len);
    assert(pthread_create(&u->thread, NULL, serve, u) == 0);
}

/* Query for example.com A, ID 0xbeef. */
static const uint8_t QUERY[] = {
    0xbe, 0xef, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0x00, 0x01, 0x00, 0x01,
};

static fake_upstream_t silent, answer, wrong, wrong_then_right, wrong_question, spy;
static forwarder_t fwd;

static size_t ask(const fake_upstream_t *a, const fake_upstream_t *b, int timeout_ms,
                  uint8_t *out, int *via, int64_t *elapsed) {
    struct sockaddr_in ups[2] = {a->addr, b ? b->addr : a->addr};
    int64_t t0 = now_ms();
    size_t n = fwd_query(&fwd, ups, b ? 2 : 1, timeout_ms, QUERY, sizeof QUERY, sizeof QUERY,
                         out, 512, via);
    *elapsed = now_ms() - t0;
    return n;
}

static void test_first_silent_second_answers(void) {
    uint8_t out[512];
    int via = -1;
    int64_t ms;
    size_t n = ask(&silent, &answer, 600, out, &via, &ms);
    assert(n == sizeof QUERY);
    assert(via == 1);
    assert(out[0] == 0xbe && out[1] == 0xef && out[11] == 0x42);
    assert(ms >= 250 && ms < 600); /* waited its 300 ms share, not the whole budget */
}

static void test_wrong_id_is_skipped(void) {
    uint8_t out[512];
    int via = -1;
    int64_t ms;
    size_t n = ask(&wrong, &answer, 600, out, &via, &ms);
    assert(n == sizeof QUERY && via == 1);
}

static void test_wrong_then_right_same_upstream(void) {
    uint8_t out[512];
    int via = -1;
    int64_t ms;
    size_t n = ask(&wrong_then_right, &silent, 600, out, &via, &ms);
    assert(n == sizeof QUERY && via == 0);
    assert(ms < 200);
}

static void test_all_fail(void) {
    uint8_t out[512];
    int via = -1;
    int64_t ms;
    assert(ask(&silent, &wrong, 400, out, &via, &ms) == 0);
    assert(ms >= 350 && ms < 600);
}

static void test_unreachable_port_moves_on_fast(void) {
    /* Nothing listens here: ICMP port unreachable surfaces as ECONNREFUSED. */
    fake_upstream_t closed = silent;
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(probe, (struct sockaddr *)&a, sizeof a);
    socklen_t len = sizeof a;
    getsockname(probe, (struct sockaddr *)&a, &len);
    close(probe);
    closed.addr = a;

    forwarder_t fresh;
    fwd_init(&fresh);
    struct sockaddr_in ups[2] = {closed.addr, answer.addr};
    uint8_t out[512];
    int via = -1;
    size_t n = fwd_query(&fresh, ups, 2, 1000, QUERY, sizeof QUERY, sizeof QUERY, out, sizeof out, &via);
    assert(n == sizeof QUERY && via == 1);
    fwd_close(&fresh);
}

static void test_reuse_after_late_answer(void) {
    /* A late answer left in a socket's queue must not be taken for the next query. */
    uint8_t out[512];
    int via = -1;
    int64_t ms;
    assert(ask(&wrong, &answer, 600, out, &via, &ms) == sizeof QUERY);
    assert(ask(&wrong, &answer, 600, out, &via, &ms) == sizeof QUERY && via == 1);
}

static void test_wrong_question_rejected(void) {
    uint8_t out[512];
    int via = -1;
    int64_t ms;
    size_t n = ask(&wrong_question, &answer, 600, out, &via, &ms);
    assert(n == sizeof QUERY && via == 1);
}

static void test_upstream_sees_random_id(void) {
    /* The client's ID must not reach the upstream (it is known to the client,
     * so a LAN attacker could forge answers); the client gets its own back. */
    uint8_t out[512];
    int via = -1, differ = 0;
    int64_t ms;
    for (int i = 0; i < 4; i++) {
        assert(ask(&spy, NULL, 600, out, &via, &ms) == sizeof QUERY);
        assert(out[0] == 0xbe && out[1] == 0xef);
        if (seen_id[0] != 0xbe || seen_id[1] != 0xef)
            differ++;
    }
    assert(differ >= 3);
}

int main(void) {
    start(&silent, SILENT);
    start(&answer, ANSWER);
    start(&wrong, WRONG_ID);
    start(&wrong_then_right, WRONG_THEN_RIGHT);
    start(&wrong_question, WRONG_QUESTION);
    start(&spy, ID_SPY);
    fwd_init(&fwd);

    test_first_silent_second_answers();
    test_wrong_id_is_skipped();
    test_wrong_then_right_same_upstream();
    test_all_fail();
    test_unreachable_port_moves_on_fast();
    test_reuse_after_late_answer();
    test_wrong_question_rejected();
    test_upstream_sees_random_id();

    fwd_close(&fwd);
    stop = 1;
    pthread_join(silent.thread, NULL);
    pthread_join(answer.thread, NULL);
    pthread_join(wrong.thread, NULL);
    pthread_join(wrong_then_right.thread, NULL);
    pthread_join(wrong_question.thread, NULL);
    pthread_join(spy.thread, NULL);
    puts("test_forward: ok");
    return 0;
}
