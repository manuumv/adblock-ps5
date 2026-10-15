/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "log.h"
#include "platform.h"
#include "server.h"

static volatile sig_atomic_t running = 1;
static config_t cfg; /* large: static, not on the stack */
static server_t srv;

static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

int main(void) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (plat_init() != 0)
        return 1;

    const char *dir = plat_data_dir();
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, "[ps5-adblock] cannot create %s: %s\n", dir, strerror(errno));

    char path[512];
    snprintf(path, sizeof path, "%s/config.ini", dir);
    int loaded = config_load(path, &cfg);
    log_set_debug(cfg.debug);

    log_printf("ps5-adblock %s starting", ADBLOCK_VERSION);
    if (loaded == 1)
        log_printf("created default config at %s", path);
    else if (loaded < 0)
        log_printf("using built-in defaults");
    size_t shown = cfg.warning_count < CFG_MAX_WARNINGS ? cfg.warning_count : CFG_MAX_WARNINGS;
    for (size_t i = 0; i < shown; i++)
        log_printf("%s", cfg.warnings[i]);
    if (cfg.warning_count > shown)
        log_printf("%zu more config warnings", cfg.warning_count - shown);
    log_printf("%zu upstream(s), %zu override(s), %zu exception(s), timeout %d ms",
               cfg.upstream_count, cfg.rule_count, cfg.exception_count, cfg.timeout_ms);

    if (((const uint8_t *)&cfg.bind)[0] != 127)
        log_printf("serving the LAN: only private and link-local addresses are answered");

    if (server_open(&srv, &cfg, plat_dns_port()) == 0)
        log_printf("no listener open yet, retrying every %d ms", LISTENER_RETRY_MS);

    while (running)
        server_poll(&srv, 1000);

    log_printf("shutting down");
    server_close(&srv);
    plat_fini();
    return 0;
}
