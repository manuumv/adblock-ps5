#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

static config_t cfg; /* large: keep it off the stack */
static char dir[] = "/tmp/adblock-test-config-XXXXXX";
static char path[256];

static void write_file(const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static int has_warning(const char *needle) {
    size_t n = cfg.warning_count < CFG_MAX_WARNINGS ? cfg.warning_count : CFG_MAX_WARNINGS;
    for (size_t i = 0; i < n; i++)
        if (strstr(cfg.warnings[i], needle))
            return 1;
    return 0;
}

static const char *ip4(struct in_addr a) {
    static char buf[INET_ADDRSTRLEN];
    return inet_ntop(AF_INET, &a, buf, sizeof buf);
}

static void check_defaults(void) {
    assert(strcmp(ip4(cfg.bind), "127.0.0.1") == 0);
    assert(cfg.bind6_enabled);
    assert(cfg.upstream_count == 2);
    assert(strcmp(ip4(cfg.upstreams[0].sin_addr), "1.1.1.1") == 0);
    assert(strcmp(ip4(cfg.upstreams[1].sin_addr), "9.9.9.9") == 0);
    assert(ntohs(cfg.upstreams[0].sin_port) == 53);
    assert(cfg.timeout_ms == 1500);
}

static void test_missing_file_is_created(void) {
    unlink(path);
    assert(config_load(path, &cfg) == 1);
    assert(access(path, R_OK) == 0);
    check_defaults();
    assert(cfg.warning_count == 0);

    /* Second start reads it back without warnings and does not rewrite it. */
    write_file("[general]\ndebug=1\n");
    assert(config_load(path, &cfg) == 0);
    assert(cfg.debug == 1);
}

static void test_default_file_contents(void) {
    unlink(path);
    assert(config_load(path, &cfg) == 1);
    FILE *f = fopen(path, "r");
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    assert(strstr(buf, "[general]") && strstr(buf, "[upstream]"));
    assert(strstr(buf, "[overrides]") && strstr(buf, "[exceptions]"));
    assert(strstr(buf, "[blocking]"));
}

static void test_comments_and_values(void) {
    write_file("# comment\n"
               "; another\n"
               "[general]\n"
               "  debug = 1   # inline\n"
               "bind=0.0.0.0\n"
               "bind6=off\n"
               "[upstream]\n"
               "server=8.8.8.8 ; google\n"
               "timeout_ms=900\n"
               "[overrides]\n"
               "*.Ads.Example.COM.=0.0.0.0\n"
               "nas.home=192.168.1.10\n"
               "[exceptions]\n"
               "*.STUN.playstation.net\n");
    assert(config_load(path, &cfg) == 0);
    assert(cfg.warning_count == 0);
    assert(cfg.debug == 1);
    assert(strcmp(ip4(cfg.bind), "0.0.0.0") == 0);
    assert(!cfg.bind6_enabled);
    assert(cfg.upstream_count == 1);
    assert(strcmp(ip4(cfg.upstreams[0].sin_addr), "8.8.8.8") == 0);
    assert(cfg.timeout_ms == 900);
    assert(cfg.rule_count == 2);
    assert(strcmp(cfg.rules[0].mask, "*.ads.example.com") == 0);
    assert(cfg.rules[1].ip[0] == 192 && cfg.rules[1].ip[3] == 10);
    assert(cfg.exception_count == 1);
    assert(strcmp(cfg.exceptions[0], "*.stun.playstation.net") == 0);
}

static void test_invalid_lines(void) {
    write_file("[general]\n"            /* 1 */
               "bind=not-an-ip\n"       /* 2 */
               "unknown=1\n"            /* 3 */
               "[upstream]\n"           /* 4 */
               "server=1.2.3\n"         /* 5 */
               "timeout_ms=abc\n"       /* 6 */
               "[overrides]\n"          /* 7 */
               "ok.example=1.2.3.4\n"   /* 8 */
               "noequals\n"             /* 9 */
               "bad mask!=1.1.1.1\n"    /* 10 */
               "[overrides]\n"          /* 11 */
               "ads.example.com=999.1.1.1\n" /* 12 */
               "[nope]\n");             /* 13 */
    assert(config_load(path, &cfg) == 0);
    assert(has_warning("line 2"));
    assert(has_warning("line 3"));
    assert(has_warning("line 5"));
    assert(has_warning("line 6"));
    assert(has_warning("line 9"));
    assert(has_warning("line 10"));
    assert(has_warning("line 12"));
    assert(has_warning("line 13"));
    assert(!has_warning("line 8"));
    assert(strcmp(ip4(cfg.bind), "127.0.0.1") == 0); /* default kept */
    assert(cfg.timeout_ms == 1500);
    assert(cfg.rule_count == 1 && strcmp(cfg.rules[0].mask, "ok.example") == 0);
    /* No valid server line: defaults with a warning. */
    assert(cfg.upstream_count == 2);
    assert(has_warning("upstream"));
}

static void test_limits(void) {
    char text[16384] = "[upstream]\n";
    for (int i = 1; i <= 10; i++) {
        char line[64];
        snprintf(line, sizeof line, "server=10.0.0.%d\n", i);
        strcat(text, line);
    }
    write_file(text);
    assert(config_load(path, &cfg) == 0);
    assert(cfg.upstream_count == 8);
    assert(has_warning("line 10") && has_warning("line 11"));

    FILE *f = fopen(path, "w");
    fputs("[overrides]\n", f);
    for (int i = 0; i < 300; i++)
        fprintf(f, "host%d.example=0.0.0.0\n", i);
    fputs("[exceptions]\n", f);
    for (int i = 0; i < 300; i++)
        fprintf(f, "ex%d.example\n", i);
    fclose(f);
    assert(config_load(path, &cfg) == 0);
    assert(cfg.rule_count == CFG_MAX_RULES);
    assert(cfg.exception_count == CFG_MAX_EXCEPTIONS);
    assert(cfg.warning_count == 88);
}

static void test_embedded_nul(void) {
    /* A NUL byte must not make the parser swallow the following line. */
    static const char text[] = "[general]\ndebug=1\0junk\n[upstream]\nserver=8.8.8.8\n";
    FILE *f = fopen(path, "w");
    fwrite(text, 1, sizeof text - 1, f);
    fclose(f);
    assert(config_load(path, &cfg) == 0);
    assert(cfg.debug == 1);
    assert(cfg.upstream_count == 1);
    assert(strcmp(ip4(cfg.upstreams[0].sin_addr), "8.8.8.8") == 0);
}

static int has_exception(const char *mask) {
    for (size_t i = 0; i < cfg.exception_count; i++)
        if (!strcmp(cfg.exceptions[i], mask))
            return 1;
    return 0;
}

static void test_blocking_section(void) {
    unlink(path);
    assert(config_load(path, &cfg) == 1);
    assert(cfg.block_nxdomain == 0);
    assert(cfg.max_domains == 1500000);
    assert(cfg.block_mask_count == 0);

    write_file("[blocking]\n"             /* 1 */
               "block_mode=nxdomain\n"    /* 2 */
               "max_domains=200000\n"     /* 3 */
               "preset=update-block\n"    /* 4 */
               "preset=nope\n"            /* 5 */
               "block_mode=weird\n"       /* 6 */
               "max_domains=5\n"          /* 7 */
               "[exceptions]\n"
               "mine.example\n");
    assert(config_load(path, &cfg) == 0);
    assert(cfg.block_nxdomain == 1);
    assert(cfg.max_domains == 200000);
    assert(cfg.block_mask_count == 8);
    assert(has_exception("stun.*.playstation.net") && has_exception("mine.example"));
    assert(has_warning("line 5") && has_warning("line 6") && has_warning("line 7"));
    assert(cfg.warning_count == 3);

    write_file("[blocking]\npreset=update-block\npreset=update-block\n");
    assert(config_load(path, &cfg) == 0);
    assert(cfg.block_mask_count == 8); /* enabling twice adds nothing */
}

static void test_unreadable_path(void) {
    assert(config_load("/nonexistent-dir/x/config.ini", &cfg) == -1);
    check_defaults();
}

static void test_example_file(void) {
    /* config.ini.example at the repo root must load without warnings. */
    snprintf(path, sizeof path, "config.ini.example");
    assert(config_load(path, &cfg) == 0);
    assert(cfg.warning_count == 0);
    check_defaults();
}

int main(void) {
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/config.ini", dir);
    test_missing_file_is_created();
    test_default_file_contents();
    test_comments_and_values();
    test_invalid_lines();
    test_limits();
    test_embedded_nul();
    test_blocking_section();
    test_unreadable_path();
    unlink(path);
    rmdir(dir);
    test_example_file();
    puts("test_config: ok");
    return 0;
}
