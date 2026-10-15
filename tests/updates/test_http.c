#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "https_fixture.h"
#include "updates/http.h"

static https_fixture_t fx;
static int port;
static char dest[256];

static char resolved[256]; /* every host the download asked for, space-separated */

/* Stands in for the updater's resolver: only the fixture's hosts exist. */
static int fake_resolve(void *ctx, const char *host, char *ip, size_t n) {
    (void)ctx;
    strncat(resolved, host, sizeof resolved - strlen(resolved) - 2);
    strcat(resolved, " ");
    if (strcmp(host, FIXTURE_HOST) != 0 && strcmp(host, FIXTURE_MIRROR) != 0)
        return -1;
    snprintf(ip, n, "127.0.0.1");
    return 0;
}

static int64_t now_ms_(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int get(const char *path, const char *ca, const char *etag, long max_bytes, long timeout_s,
               http_result_t *r) {
    char url[256];
    snprintf(url, sizeof url, "https://" FIXTURE_HOST ":%d%s", port, path);
    http_get_t req = {.url = url, .dest = dest, .etag = etag, .resolve = fake_resolve,
                      .max_bytes = max_bytes, .timeout_s = timeout_s, .ca_file = ca};
    memset(r, 0, sizeof *r);
    resolved[0] = '\0';
    int rc = http_download(&req, r);
    if (getenv("HTTP_TEST_VERBOSE"))
        fprintf(stderr, "  %s -> rc=%d status=%ld error='%s'\n", path, rc, r->status, r->error);
    return rc;
}

static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void test_ok_and_not_modified(void) {
    http_result_t r;
    assert(get("/list.txt", fx.cert, NULL, 1 << 20, 10, &r) == 0);
    assert(r.status == 200 && !r.not_modified && r.bytes == 25);
    assert(r.etag[0] == '"' && strstr(r.last_modified, "2026"));
    assert(file_size(dest) == 25);

    char etag[sizeof r.etag];
    snprintf(etag, sizeof etag, "%s", r.etag);
    unlink(dest);
    assert(get("/list.txt", fx.cert, etag, 1 << 20, 10, &r) == 0);
    assert(r.status == 304 && r.not_modified);
    assert(file_size(dest) == -1); /* untouched */
}

static void test_https_redirect_followed(void) {
    http_result_t r;
    assert(get("/redirect-https", fx.cert, NULL, 1 << 20, 10, &r) == 0 && r.status == 200);
    assert(file_size(dest) == 25); /* the redirect's own body is not kept */
    unlink(dest);
}

static void test_redirect_to_another_host_is_pinned(void) {
    /* mirror.test is unknown to the system resolver: this only works if the
     * second hop is resolved through req->resolve too. */
    http_result_t r;
    assert(get("/redirect-mirror", fx.cert, NULL, 1 << 20, 10, &r) == 0 && r.status == 200);
    assert(!strcmp(resolved, FIXTURE_HOST " " FIXTURE_MIRROR " ") && file_size(dest) == 25);
    unlink(dest);

    /* A redirect target our resolver does not know fails; it never falls back to the system's. */
    assert(get("/redirect-nowhere", fx.cert, NULL, 1 << 20, 10, &r) == -1);
    assert(strstr(r.error, "nowhere.invalid") && file_size(dest) == -1);

    assert(get("/redirect-loop", fx.cert, NULL, 1 << 20, 10, &r) == -1);
    assert(strstr(r.error, "redirects") && file_size(dest) == -1);
}

static void test_unsafe_validator_dropped(void) {
    http_result_t r;
    assert(get("/bad-etag", fx.cert, NULL, 1 << 20, 10, &r) == 0);
    assert(r.etag[0] == '\0'); /* control characters: never stored or sent back */
    unlink(dest);
}

static void test_failures_remove_dest(void) {
    http_result_t r;
    assert(get("/big", fx.cert, NULL, 1 << 20, 10, &r) == -1); /* 2 MiB > 1 MiB */
    assert(strstr(r.error, "size") && file_size(dest) == -1);

    int64_t t0 = now_ms_();
    assert(get("/slow", fx.cert, NULL, 1 << 20, 1, &r) == -1);
    assert(now_ms_() - t0 < 3000 && file_size(dest) == -1);

    assert(get("/broken", fx.cert, NULL, 1 << 20, 10, &r) == -1 && file_size(dest) == -1);
    assert(get("/missing", fx.cert, NULL, 1 << 20, 10, &r) == -1 && r.status == 404);
    assert(get("/redirect-http", fx.cert, NULL, 1 << 20, 10, &r) == -1); /* never downgrade */
    assert(file_size(dest) == -1);
}

static void test_untrusted_certificate(void) {
    http_result_t r;
    assert(get("/list.txt", fx.other_cert, NULL, 1 << 20, 10, &r) == -1);
    assert(!r.clock_error && r.error[0] && file_size(dest) == -1);
}

static void test_plain_http_refused(void) {
    http_get_t req = {.url = "http://example.org/list.txt", .dest = dest, .max_bytes = 1024, .timeout_s = 5};
    http_result_t r;
    assert(http_download(&req, &r) == -1 && strstr(r.error, "https"));
}

static void test_cancel(void) {
    char url[256];
    snprintf(url, sizeof url, "https://" FIXTURE_HOST ":%d/slow", port);
    atomic_int cancel = 1;
    http_get_t req = {.url = url, .dest = dest, .resolve = fake_resolve, .max_bytes = 1 << 20,
                      .timeout_s = 60, .ca_file = fx.cert, .cancel = &cancel};
    http_result_t r;
    int64_t t0 = now_ms_();
    assert(http_download(&req, &r) == -1);
    assert(now_ms_() - t0 < 3000 && file_size(dest) == -1);
}

static void test_bad_urls_refused(void) {
    static const char *bad[] = {
        "https://user@evil.example/x", /* no userinfo */
        "https:///x",
        "https://-x.example/x",        /* would change the meaning of a CURLOPT_RESOLVE entry */
        "https://a_b.example/x",
        "https://list.test:0/x",
        "https://[::1]/x",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        http_get_t req = {.url = bad[i], .dest = dest, .resolve = fake_resolve, .max_bytes = 1024, .timeout_s = 5};
        http_result_t r;
        resolved[0] = '\0';
        assert(http_download(&req, &r) == -1 && strstr(r.error, "https") && !resolved[0]);
    }
}

static void test_clock_error(void) {
    if (!fx.have_expired) {
        puts("  skip clock error test (openssl cannot backdate certificates)");
        return;
    }
    pid_t pid;
    int saved = port;
    port = fixture_serve(&fx, fx.expired_cert, fx.expired_key, &pid);
    http_result_t r;
    assert(get("/list.txt", fx.expired_cert, NULL, 1 << 20, 10, &r) == -1);
    assert(r.clock_error);
    fixture_stop(pid);
    port = saved;
}

int main(void) {
    http_init();
    fixture_init(&fx);
    fixture_write(&fx, "list.txt", "a.example\nb.example.org\n\n");
    snprintf(dest, sizeof dest, "%s/dest.txt", fx.dir);
    pid_t pid;
    port = fixture_serve(&fx, fx.cert, fx.key, &pid);

    test_ok_and_not_modified();
    test_https_redirect_followed();
    test_redirect_to_another_host_is_pinned();
    test_unsafe_validator_dropped();
    test_failures_remove_dest();
    test_untrusted_certificate();
    test_plain_http_refused();
    test_cancel();
    test_bad_urls_refused();
    test_clock_error();

    fixture_stop(pid);
    fixture_cleanup(&fx);
    puts("test_http: ok");
    return 0;
}
