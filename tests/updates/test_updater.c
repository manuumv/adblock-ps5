#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "dns/dns.h"
#include "dns/server.h"
#include "https_fixture.h"
#include "updates/http.h"
#include "updates/updater.h"

static https_fixture_t fx;
static int port;
static config_t cfg, dns_cfg;
static updater_t up;
static char data[256], lists[256];
static atomic_int dns_stop, dns_queries;
static int dns_fd;

/* Fake upstream: answers every A query with 127.0.0.1, so "list.test" resolves
 * only through the upstream, never through /etc/hosts. */
static void *fake_dns(void *arg) {
    (void)arg;
    uint8_t pkt[512], out[512];
    static const uint8_t lo[4] = {127, 0, 0, 1};
    while (!dns_stop) {
        struct sockaddr_storage from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(dns_fd, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
        dns_query_t q;
        if (n <= 0 || dns_parse_query(pkt, (size_t)n, &q) != DNS_OK)
            continue;
        dns_queries++;
        size_t len = dns_answer(pkt, &q, DNS_RCODE_NOERROR, DNS_TYPE_A, lo, 4, 60, out, sizeof out);
        sendto(dns_fd, out, len, 0, (struct sockaddr *)&from, fl);
    }
    return NULL;
}

static void read_file(const char *path, char *buf, size_t n) {
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    size_t got = fread(buf, 1, n - 1, f);
    buf[got] = '\0';
    fclose(f);
}

static int exists(const char *path) { return access(path, F_OK) == 0; }

static void list_path(char *out, const char *name) { snprintf(out, 256, "%s/%s.txt", lists, name); }

static void set_list(size_t i, const char *name, const char *path) {
    snprintf(cfg.lists[i].name, sizeof cfg.lists[i].name, "%s", name);
    snprintf(cfg.lists[i].url, sizeof cfg.lists[i].url, "https://" FIXTURE_HOST ":%d%s", port, path);
}

/* Runs one pass with stderr captured; returns what plat_notify printed. */
static void run_capture(int64_t now, char *notices, size_t n) {
    char path[300];
    snprintf(path, sizeof path, "%s/stderr.txt", fx.dir);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    int saved = dup(2);
    dup2(fd, 2);
    updater_run_once(&up, now);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(fd);
    read_file(path, notices, n);
    if (getenv("UPDATER_TEST_VERBOSE"))
        fprintf(stderr, "  notices@%lld: [%s]\n", (long long)now, notices);
}

static void test_download_schedule_and_304(void) {
    char p[256], buf[4096], notices[4096];
    cfg.list_count = 1;
    set_list(0, "good", "/list.txt");
    list_path(p, "good");
    int64_t now = 2000000000;

    run_capture(now, notices, sizeof notices);
    read_file(p, buf, sizeof buf);
    assert(!strcmp(buf, "a.example\nb.example.org\n"));
    assert(notices[0] == '\0'); /* the server announces the domain count once loaded */
    assert(dns_queries > 0);

    /* Fresh: not downloaded again (a local edit survives). */
    FILE *f = fopen(p, "w");
    fputs("local.example\n", f);
    fclose(f);
    run_capture(now + 3600, notices, sizeof notices);
    read_file(p, buf, sizeof buf);
    assert(!strcmp(buf, "local.example\n"));

    /* Stale: conditional request, 304, file untouched, no notice. */
    run_capture(now + 25 * 3600, notices, sizeof notices);
    read_file(p, buf, sizeof buf);
    assert(!strcmp(buf, "local.example\n") && notices[0] == '\0');

    /* The list changed upstream: new content replaces the file, quietly. */
    fixture_write(&fx, "list.txt", "c.example\n");
    run_capture(now + 50 * 3600, notices, sizeof notices);
    read_file(p, buf, sizeof buf);
    assert(!strcmp(buf, "c.example\n") && notices[0] == '\0');

    /* No temporary file is left behind; the state file is hidden from the loader. */
    snprintf(p, sizeof p, "%s/.good.tmp", lists);
    assert(!exists(p));
    snprintf(p, sizeof p, "%s/.good.meta", lists);
    assert(exists(p));
}

static void test_failures_keep_old_and_notify_once(void) {
    char p[256], buf[4096], notices[4096];
    cfg.list_count = 3;
    set_list(0, "html", "/html");
    set_list(1, "broken", "/broken");
    set_list(2, "flaky", "/list.txt");
    list_path(p, "broken");
    FILE *f = fopen(p, "w");
    fputs("old.example\n", f);
    fclose(f);
    char flaky_src[512];
    snprintf(flaky_src, sizeof flaky_src, "%s/list.txt", fx.docroot);
    unlink(flaky_src); /* flaky starts failing with a 404 */
    int64_t now = 2100000000;

    run_capture(now, notices, sizeof notices);
    list_path(p, "html");
    assert(!exists(p)); /* an HTML page has no domains: nothing is written */
    list_path(p, "broken");
    read_file(p, buf, sizeof buf);
    assert(!strcmp(buf, "old.example\n")); /* interrupted: previous content kept */
    assert(strstr(notices, "html") && strstr(notices, "broken") && strstr(notices, "flaky"));

    /* Still failing: logged again, but no second notice. */
    run_capture(now + 30 * 3600, notices, sizeof notices);
    assert(notices[0] == '\0');

    /* flaky recovers, then fails again: it is announced again. */
    fixture_write(&fx, "list.txt", "c.example\n");
    run_capture(now + 60 * 3600, notices, sizeof notices);
    assert(!strstr(notices, "flaky")); /* recovered quietly */
    unlink(flaky_src);
    run_capture(now + 90 * 3600, notices, sizeof notices);
    assert(strstr(notices, "flaky"));
    fixture_write(&fx, "list.txt", "c.example\n");
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static void test_bad_timestamps_are_stale(void) {
    /* Saved while the console clock was years ahead, or a corrupt state file:
     * the list must refresh instead of looking fresh forever. */
    static const char *saved[] = {"last_success=4000000000\n", "last_success=-9223372036854775808\n"};
    char p[256], meta[256], buf[4096], notices[4096];
    cfg.list_count = 1;
    set_list(0, "ahead", "/list.txt");
    list_path(p, "ahead");
    snprintf(meta, sizeof meta, "%s/.ahead.meta", lists);
    for (size_t i = 0; i < 2; i++) {
        write_file(p, "stale.example\n");
        write_file(meta, saved[i]);
        run_capture(2300000000, notices, sizeof notices);
        read_file(p, buf, sizeof buf);
        assert(!strcmp(buf, "c.example\n"));
    }
}

static void test_disabled(void) {
    char notices[4096];
    cfg.list_count = 1;
    set_list(0, "never", "/list.txt");
    cfg.update_hours = 0;
    run_capture(2200000000, notices, sizeof notices);
    char p[256];
    list_path(p, "never");
    assert(!exists(p));
    updater_t idle;
    updater_init(&idle, &cfg, data);
    assert(updater_start(&idle) == 0 && !idle.running);
    cfg.update_hours = 24;
}

static void test_first_pass_soon_when_lists_missing(void) {
    /* A list nobody has downloaded yet is fetched within seconds; once every
     * list is on disk, the first pass waits for the network to come up. */
    cfg.list_count = 1;
    set_list(0, "fresh-install", "/list.txt");
    updater_t u;
    updater_init(&u, &cfg, data);
    assert(u.first_delay_ms == UPDATER_SOON_MS);
    set_list(0, "good", "/list.txt"); /* downloaded by the first test */
    updater_init(&u, &cfg, data);
    assert(u.first_delay_ms == UPDATER_FIRST_DELAY_MS);
}

static void test_run_once_counts_failures(void) {
    cfg.list_count = 2;
    set_list(0, "missing-a", "/missing");
    set_list(1, "missing-b", "/missing");
    up.failure_notified[0] = up.failure_notified[1] = 0;
    assert(updater_run_once(&up, 2400000000) == 2); /* the thread retries sooner after failures */
}

static void test_dns_unaffected_by_slow_download(void) {
    /* A real server answering on its own port, while the updater thread is
     * stuck in a 10 KiB/s download. */
    server_t srv;
    dns_cfg = cfg;
    inet_pton(AF_INET, "127.0.0.1", &dns_cfg.bind);
    dns_cfg.bind6_enabled = 0;
    snprintf(dns_cfg.rules[0].mask, DNS_NAME_MAX, "fast.example");
    dns_cfg.rule_count = 1;
    assert(server_open(&srv, &dns_cfg, 0, NULL) == 1);

    cfg.list_count = 1;
    set_list(0, "slow", "/slow");
    updater_t slow;
    updater_init(&slow, &cfg, data);
    slow.ca_file = fx.cert;
    slow.first_delay_ms = 0;
    assert(updater_start(&slow) == 0 && slow.running);
    usleep(500 * 1000); /* the download is under way */

    int c = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)srv.port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    uint8_t q[512], out[512];
    int64_t worst = 0;
    for (int i = 0; i < 20; i++) {
        size_t len = dns_build_query((uint16_t)i, "fast.example", DNS_TYPE_A, q, sizeof q);
        int64_t t0 = now_ms();
        sendto(c, q, len, 0, (struct sockaddr *)&to, sizeof to);
        ssize_t n = -1;
        while (n < 0 && now_ms() - t0 < 1000) {
            server_poll(&srv, 10);
            n = recv(c, out, sizeof out, MSG_DONTWAIT);
        }
        assert(n > 0);
        if (now_ms() - t0 > worst)
            worst = now_ms() - t0;
        usleep(50 * 1000);
    }
    int64_t t0 = now_ms();
    updater_stop(&slow); /* aborts the download */
    printf("  worst DNS latency during a slow download: %lld ms, stop took %lld ms\n",
           (long long)worst, (long long)(now_ms() - t0));
    assert(worst < 50 && now_ms() - t0 < 2000);
    close(c);
    server_close(&srv);
}

int main(void) {
    http_init();
    fixture_init(&fx);
    fixture_write(&fx, "list.txt", "a.example\nb.example.org\n");
    pid_t pid;
    port = fixture_serve(&fx, fx.cert, fx.key, &pid);

    snprintf(data, sizeof data, "%s/data", fx.dir);
    snprintf(lists, sizeof lists, "%s/lists", data);
    mkdir(data, 0755);
    mkdir(lists, 0755);

    dns_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(dns_fd, (struct sockaddr *)&a, sizeof a) == 0);
    socklen_t al = sizeof a;
    getsockname(dns_fd, (struct sockaddr *)&a, &al);
    struct timeval tv = {0, 100 * 1000};
    setsockopt(dns_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    pthread_t dns_thread;
    pthread_create(&dns_thread, NULL, fake_dns, NULL);

    cfg.upstreams[0] = a;
    cfg.upstream_count = 1;
    cfg.timeout_ms = 1000;
    cfg.update_hours = 24;
    updater_init(&up, &cfg, data);
    up.ca_file = fx.cert;

    test_download_schedule_and_304();
    test_failures_keep_old_and_notify_once();
    test_bad_timestamps_are_stale();
    test_disabled();
    test_first_pass_soon_when_lists_missing();
    test_run_once_counts_failures();
    test_dns_unaffected_by_slow_download();

    dns_stop = 1;
    pthread_join(dns_thread, NULL);
    fixture_stop(pid);
    fixture_cleanup(&fx);
    puts("test_updater: ok");
    return 0;
}
