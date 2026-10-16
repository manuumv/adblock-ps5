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
#include "core/platform.h"

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
    cfg.block_updates = 1;
    assert(runtime_ready_notice(&cfg, 500000) == 1);
    cfg.block_updates = 0;
    cfg.quiet = 1;
    assert(runtime_ready_notice(&cfg, 777) == 1);
    runtime_port_error_notice(53); /* shown even when quiet */
    cfg.quiet = 0;
    /* First start: no lists yet, they are about to download. Not final, so
     * the domain count is announced once they load. */
    cfg.list_count = 1;
    cfg.update_hours = 24;
    assert(runtime_ready_notice(&cfg, 0) == 0);
    cfg.list_count = 0;

    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(fd);
    char buf[2048];
    slurp(path, buf, sizeof buf);
    assert(strstr(buf, ADBLOCK_VERSION) && strstr(buf, "500000") && strstr(buf, "127.0.0.1"));
    assert(strstr(buf, "500000 domains and firmware updates")); /* the user can see updates are off */
    assert(!strstr(buf, "777"));
    assert(strstr(buf, "port 53"));
    assert(strstr(buf, "downloading block lists") && !strstr(buf, "blocking 0"));
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

/* Runs runtime_dns_check with notifications captured into out. */
static void dns_check(const char *primary, const char *secondary, char *state, char *out, size_t n) {
    char path[256];
    snprintf(path, sizeof path, "%s/dns.txt", dir);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    int saved = dup(2);
    dup2(fd, 2);
    runtime_dns_check(&cfg, primary, secondary, state, RUNTIME_DNS_STATE_LEN);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(fd);
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    size_t got = fread(out, 1, n - 1, f);
    out[got] = '\0';
    fclose(f);
    unlink(path);
}

static void test_console_dns_check(void) {
    char state[RUNTIME_DNS_STATE_LEN] = "", out[2048];
    inet_pton(AF_INET, "127.0.0.1", &cfg.bind);

    /* Already right at start: the ready notice says enough. */
    dns_check("127.0.0.1", "", state, out, sizeof out);
    assert(out[0] == '\0');

    /* DNS from the router: no lookup reaches the blocker. Said once, with the fix. */
    dns_check("192.168.1.1", "", state, out, sizeof out);
    assert(strstr(out, "nothing is blocked") && strstr(out, "192.168.1.1") && strstr(out, "Primary DNS"));
    dns_check("192.168.1.1", "", state, out, sizeof out);
    assert(out[0] == '\0');

    /* Fixed while running: confirmed once. An empty secondary reads as 0.0.0.0. */
    dns_check("127.0.0.1", "0.0.0.0", state, out, sizeof out);
    assert(strstr(out, "blocking is on"));
    dns_check("127.0.0.1", "", state, out, sizeof out);
    assert(out[0] == '\0');

    /* A public secondary lets lookups skip the blocker. */
    dns_check("127.0.0.1", "8.8.8.8", state, out, sizeof out);
    assert(strstr(out, "Secondary DNS") && strstr(out, "8.8.8.8"));

    /* A secondary that is also an upstream is the user's own fallback, such as a
     * DNS that blocks Sony while ps5-adblock is not running: not flagged. */
    cfg.upstreams[0].sin_family = AF_INET;
    inet_pton(AF_INET, "45.56.67.85", &cfg.upstreams[0].sin_addr);
    cfg.upstream_count = 1;
    dns_check("127.0.0.1", "45.56.67.85", state, out, sizeof out);
    assert(strstr(out, "blocking is on") && !strstr(out, "Secondary"));
    cfg.upstream_count = 0;

    /* Serving the LAN (bind=0.0.0.0): the console may use any of its own addresses. */
    cfg.bind.s_addr = htonl(INADDR_ANY);
    state[0] = '\0';
    dns_check("192.168.1.50", "", state, out, sizeof out);
    assert(out[0] == '\0');
}

static uint32_t ip(const char *s) {
    struct in_addr a;
    assert(inet_pton(AF_INET, s, &a) == 1);
    return a.s_addr;
}

static void assert_system_dns(const char *primary, const char *secondary) {
    uint32_t now[2];
    assert(plat_system_dns_get(now) == 0);
    assert(now[0] == ip(primary) && now[1] == (secondary ? ip(secondary) : 0));
}

static void test_dns_redirect(void) {
    static config_t c;
    inet_pton(AF_INET, "127.0.0.1", &c.bind);
    c.dns_redirect = 1;
    dns_redirect_t d;

    /* The console uses the user's Sony-blocking DNS from its settings. */
    setenv("ADBLOCK_SYSTEM_DNS", "45.56.67.85,8.8.8.8", 1);
    assert(runtime_dns_redirect(&d, &c) == 0 && d.active);
    assert_system_dns("127.0.0.1", NULL); /* no secondary to skip the blocker */

    /* A network reconnect puts the settings' DNS back: taken over again. */
    uint32_t settings[2] = {ip("45.56.67.85"), ip("8.8.8.8")};
    assert(plat_system_dns_set(settings) == 0);
    runtime_dns_keep(&d);
    assert_system_dns("127.0.0.1", NULL);
    runtime_dns_keep(&d); /* nothing to do */
    assert_system_dns("127.0.0.1", NULL);

    /* On exit the console gets its own DNS back. */
    runtime_dns_restore(&d);
    assert(!d.active);
    assert_system_dns("45.56.67.85", "8.8.8.8");
    runtime_dns_restore(&d); /* harmless twice */
    assert_system_dns("45.56.67.85", "8.8.8.8");

    /* Serving the LAN on 0.0.0.0: the console still asks 127.0.0.1. */
    c.bind.s_addr = htonl(INADDR_ANY);
    assert(runtime_dns_redirect(&d, &c) == 0);
    assert_system_dns("127.0.0.1", NULL);
    runtime_dns_restore(&d);
    inet_pton(AF_INET, "127.0.0.1", &c.bind);

    /* No secondary before: still none after. */
    uint32_t only_primary[2] = {ip("45.56.67.85"), 0};
    assert(plat_system_dns_set(only_primary) == 0);
    assert(runtime_dns_redirect(&d, &c) == 0);
    runtime_dns_restore(&d);
    assert_system_dns("45.56.67.85", NULL);

    /* A copy killed while redirecting left 127.0.0.1, and the settings cannot be
     * read: the redirect is cleared on exit, never left in place. */
    uint32_t leftover[2] = {ip("127.0.0.1"), 0};
    assert(plat_system_dns_set(leftover) == 0);
    assert(runtime_dns_redirect(&d, &c) == 0);
    runtime_dns_restore(&d);
    uint32_t now[2];
    assert(plat_system_dns_get(now) == 0 && now[0] == 0 && now[1] == 0); /* back to the settings */
    assert(plat_system_dns_set(settings) == 0);

    /* Turned off in config.ini: the system DNS is left alone. */
    c.dns_redirect = 0;
    assert(runtime_dns_redirect(&d, &c) == -1 && !d.active);
    assert_system_dns("45.56.67.85", "8.8.8.8");
}

int main(void) {
    assert(mkdtemp(dir));
    test_dns_redirect(); /* before anything else reads the fake system DNS */
    test_notices();
    test_log_flush();
    test_console_dns_check();
    rmdir(dir);
    puts("test_runtime: ok");
    return 0;
}
