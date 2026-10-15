#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/log.h"
#include "console/runtime.h"

static config_t cfg;
static char dir[] = "/tmp/adblock-test-runtime-XXXXXX";

static void slurp(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    assert(f);
    size_t got = fread(buf, 1, n - 1, f);
    buf[got] = '\0';
    fclose(f);
}

static void test_notices(void) {
    /* The host platform prints notifications to stderr: capture it. */
    char path[256];
    snprintf(path, sizeof path, "%s/stderr.txt", dir);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    int saved = dup(2);
    dup2(fd, 2);

    inet_pton(AF_INET, "127.0.0.1", &cfg.bind);
    runtime_ready_notice(&cfg, 500000);
    cfg.quiet = 1;
    runtime_ready_notice(&cfg, 777);
    runtime_port_error_notice(53); /* shown even when quiet */
    cfg.quiet = 0;

    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(fd);
    char buf[2048];
    slurp(path, buf, sizeof buf);
    assert(strstr(buf, ADBLOCK_VERSION) && strstr(buf, "500000") && strstr(buf, "127.0.0.1"));
    assert(!strstr(buf, "777"));
    assert(strstr(buf, "port 53"));
    unlink(path);
}

static void test_log_flush(void) {
    char last[256], prev[256], buf[64 * 1024];
    snprintf(last, sizeof last, "%s/last.log", dir);
    snprintf(prev, sizeof prev, "%s/last.previous.log", dir);
    FILE *f = fopen(last, "w");
    fputs("previous session\n", f);
    fclose(f);

    /* Start: the previous session's log is kept aside. */
    runtime_log_start(dir, 0);
    slurp(prev, buf, sizeof buf);
    assert(strstr(buf, "previous session"));
    assert(access(last, F_OK) != 0);

    log_printf("first event");
    runtime_log_tick(1000); /* too soon */
    assert(access(last, F_OK) != 0);
    runtime_log_tick(LOG_FLUSH_MS);
    slurp(last, buf, sizeof buf);
    assert(strstr(buf, "first event"));

    log_printf("second event");
    runtime_log_tick(LOG_FLUSH_MS + 500); /* at most every 10 minutes */
    slurp(last, buf, sizeof buf);
    assert(!strstr(buf, "second event"));
    runtime_log_tick(2 * LOG_FLUSH_MS);
    slurp(last, buf, sizeof buf);
    assert(strstr(buf, "second event"));

    /* No new lines: no write at all. */
    unlink(last);
    runtime_log_tick(4 * LOG_FLUSH_MS);
    assert(access(last, F_OK) != 0);

    /* Orderly exit writes pending lines at once. */
    log_printf("shutting down");
    runtime_log_flush();
    slurp(last, buf, sizeof buf);
    assert(strstr(buf, "shutting down"));

    unlink(prev);
    unlink(last);
}

int main(void) {
    assert(mkdtemp(dir));
    test_notices();
    test_log_flush();
    rmdir(dir);
    puts("test_runtime: ok");
    return 0;
}
