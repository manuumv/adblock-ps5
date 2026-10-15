/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "blocking/blocklist.h"
#include "core/config.h"
#include "core/log.h"
#include "core/platform.h"
#include "console/runtime.h"
#include "dns/server.h"
#include "updates/http.h"
#include "updates/updater.h"

#define TAKE_OVER_MS 5000

static volatile sig_atomic_t running = 1;
static config_t cfg; /* large: static, not on the stack */
static server_t srv;
static updater_t updater;

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

    http_init(); /* libcurl's global init is not thread-safe: before any thread starts */

    uint8_t key[16];
    arc4random_buf(key, sizeof key);
    domain_hash_set_key(key); /* before any thread builds a set */

    const char *dir = plat_data_dir();
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        plat_notify("ps5-adblock: cannot create %s (%s), running with defaults", dir,
                    strerror(errno));

    char path[512];
    snprintf(path, sizeof path, "%s/lists", dir);
    (void)mkdir(path, 0755); /* where users drop blocklists */
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

    if (runtime_take_over(ADBLOCK_PROCESS_NAME, TAKE_OVER_MS) != 0) {
        plat_notify("ps5-adblock: an older copy is still running and did not stop. "
                    "Reboot the console, then send the payload again.");
        plat_fini();
        return 1;
    }
    /* Rotate once the old copy is gone, so it cannot rewrite last.log afterwards. */
    runtime_log_start(dir, now_ms());

    char msg[CFG_WARNING_LEN];
    snprintf(path, sizeof path, "%s/%s", dir, AUTOSTART_ELF);
    int synced = autostart_sync(cfg.autostart, plat_hen_dirs(), plat_payload_copies(), path,
                                msg, sizeof msg);
    log_printf("%s", msg);
    if (synced == -1)
        plat_notify("ps5-adblock: %s", msg); /* it may not start after a reboot */
    else if (synced == AUTOSTART_NO_COPY && !cfg.quiet)
        plat_notify("ps5-adblock: running, but it will not start by itself after a reboot "
                    "(see Install in the README)");

    if (server_open(&srv, &cfg, plat_dns_port(), dir) == 0)
        log_printf("no listener open yet, retrying every %d ms", LISTENER_RETRY_MS);

    updater_init(&updater, &cfg, dir);
    (void)updater_start(&updater);

    while (running) {
        server_poll(&srv, 1000);
        runtime_log_tick(now_ms());
    }

    log_printf("shutting down");
    updater_stop(&updater);
    server_close(&srv);
    runtime_log_flush();
    plat_fini();
    return 0;
}
