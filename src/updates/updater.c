/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "updates/updater.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "blocking/listparse.h"
#include "core/log.h"
#include "core/platform.h"
#include "dns/dns.h"
#include "updates/http.h"

/* What is remembered about a list between runs, in lists/.<name>.meta
 * (a dotfile, so the list loader ignores it). */
typedef struct {
    char etag[128];
    char last_modified[64];
    int64_t last_success;
} list_meta_t;

static void read_meta(const char *path, list_meta_t *m) {
    memset(m, 0, sizeof *m);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *v = eq + 1;
        if (!strcmp(line, "etag"))
            snprintf(m->etag, sizeof m->etag, "%s", v);
        else if (!strcmp(line, "last_modified"))
            snprintf(m->last_modified, sizeof m->last_modified, "%s", v);
        else if (!strcmp(line, "last_success"))
            m->last_success = strtoll(v, NULL, 10);
    }
    fclose(f);
}

static void write_meta(const char *path, const list_meta_t *m) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "etag=%s\nlast_modified=%s\nlast_success=%lld\n", m->etag, m->last_modified,
            (long long)m->last_success);
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        log_printf("cannot save %s: %s", path, strerror(errno));
        unlink(tmp);
    }
}

/* Resolves every host a download visits straight at the upstreams: the system
 * resolver is this very payload, and the lists must never be able to block
 * their own update host. */
static int resolve(void *ctx, const char *host, char *ip, size_t n) {
    updater_t *u = ctx;
    uint8_t q[512], resp[DNS_MAX_PACKET], addr[4];
    size_t len = dns_build_query((uint16_t)arc4random(), host, DNS_TYPE_A, q, sizeof q);
    int via;
    if (!len)
        return -1;
    size_t got = fwd_query(&u->fwd, u->cfg->upstreams, u->cfg->upstream_count, u->cfg->timeout_ms, q,
                           len, len, resp, sizeof resp, &via);
    if (!got || dns_first_a(resp, got, addr) != 0)
        return -1;
    return inet_ntop(AF_INET, addr, ip, (socklen_t)n) ? 0 : -1;
}

static void count_domain(void *ud, const char *domain) {
    (void)domain;
    (*(size_t *)ud)++;
}

static void failed(updater_t *u, size_t i, const char *error, int clock_error) {
    const char *name = u->cfg->lists[i].name;
    log_printf("list %s: %s", name, error);
    if (u->failure_notified[i])
        return;
    u->failure_notified[i] = 1;
    plat_notify("ps5-adblock: cannot update list %s: %s%s", name, error,
                clock_error ? ". Check the console date and time." : "");
}

static void update_one(updater_t *u, size_t i, int64_t now) {
    const list_source_t *src = &u->cfg->lists[i];
    char txt[PATH_MAX], tmp[PATH_MAX], meta_path[PATH_MAX];
    snprintf(txt, sizeof txt, "%s/lists/%s.txt", u->data_dir, src->name);
    snprintf(tmp, sizeof tmp, "%s/lists/.%s.tmp", u->data_dir, src->name);
    snprintf(meta_path, sizeof meta_path, "%s/lists/.%s.meta", u->data_dir, src->name);

    list_meta_t meta;
    read_meta(meta_path, &meta);
    int existed = access(txt, F_OK) == 0;
    /* A timestamp from the future (the clock was ahead, or a corrupt file)
     * counts as stale; checking it first also keeps the subtraction in range. */
    int sane = meta.last_success >= 0 && meta.last_success <= now;
    if (existed && sane && now - meta.last_success < (int64_t)u->cfg->update_hours * 3600)
        return; /* fresh */

    http_get_t req = {
        .url = src->url,
        .dest = tmp,
        .etag = existed ? meta.etag : NULL, /* conditional only if we still have the file */
        .last_modified = existed ? meta.last_modified : NULL,
        .resolve = resolve,
        .resolve_ctx = u,
        .max_bytes = UPDATER_MAX_BYTES,
        .timeout_s = UPDATER_TIMEOUT_S,
        .ca_file = u->ca_file,
        .cancel = &u->stop,
    };
    http_result_t r;
    if (http_download(&req, &r) != 0) {
        if (!u->stop)
            failed(u, i, r.error, r.clock_error);
        return;
    }

    if (!r.not_modified) {
        size_t domains = 0;
        list_stats_t st = {0};
        if (list_read_file(tmp, count_domain, &domains, &st) != 0 || domains == 0) {
            unlink(tmp);
            failed(u, i, "the download contains no domains", 0);
            return;
        }
        if (rename(tmp, txt) != 0) {
            unlink(tmp);
            failed(u, i, strerror(errno), 0);
            return;
        }
        snprintf(meta.etag, sizeof meta.etag, "%s", r.etag);
        snprintf(meta.last_modified, sizeof meta.last_modified, "%s", r.last_modified);
        log_printf("list %s: updated, %zu domains (%ld bytes)", src->name, domains, r.bytes);
        if (!existed && !u->cfg->quiet)
            plat_notify("ps5-adblock: downloaded %s (%zu domains)", src->name, domains);
    } else {
        log_printf("list %s: unchanged", src->name);
    }
    meta.last_success = now;
    write_meta(meta_path, &meta);
    u->failure_notified[i] = 0;
}

void updater_run_once(updater_t *u, int64_t now) {
    if (u->cfg->update_hours <= 0)
        return;
    for (size_t i = 0; i < u->cfg->list_count && !u->stop; i++)
        update_one(u, i, now);
}

void updater_init(updater_t *u, const config_t *cfg, const char *data_dir) {
    memset(u, 0, sizeof *u);
    u->cfg = cfg;
    u->data_dir = data_dir;
    u->first_delay_ms = UPDATER_FIRST_DELAY_MS;
    u->check_ms = UPDATER_CHECK_MS;
    fwd_init(&u->fwd);
}

/* Waits are measured on the monotonic clock, so setting the console's date
 * never stretches or cuts one short. */
static void wake_init(updater_t *u) {
    u->wake_clock = CLOCK_REALTIME;
#ifdef __APPLE__ /* no pthread_condattr_setclock: wait_ms uses a relative timeout */
    pthread_cond_init(&u->wake, NULL);
#else
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0)
        u->wake_clock = CLOCK_MONOTONIC;
    pthread_cond_init(&u->wake, &attr);
    pthread_condattr_destroy(&attr);
#endif
}

/* Sleeps up to ms, or until updater_stop. 1 if stopping. */
static int wait_ms(updater_t *u, int64_t ms) {
    int64_t until = now_ms() + ms;
    pthread_mutex_lock(&u->lock);
    for (int64_t left = ms; !u->stop && left > 0; left = until - now_ms()) {
        struct timespec ts;
#ifdef __APPLE__
        ts.tv_sec = (time_t)(left / 1000);
        ts.tv_nsec = (long)(left % 1000) * 1000000;
        pthread_cond_timedwait_relative_np(&u->wake, &u->lock, &ts);
#else
        clock_gettime(u->wake_clock, &ts);
        int64_t ns = ts.tv_nsec + (left % 1000) * 1000000;
        ts.tv_sec += (time_t)(left / 1000 + ns / 1000000000);
        ts.tv_nsec = (long)(ns % 1000000000);
        pthread_cond_timedwait(&u->wake, &u->lock, &ts);
#endif
    }
    int stop = u->stop;
    pthread_mutex_unlock(&u->lock);
    return stop;
}

static void *run(void *arg) {
    updater_t *u = arg;
    if (wait_ms(u, u->first_delay_ms))
        return NULL;
    do {
        updater_run_once(u, (int64_t)time(NULL));
    } while (!wait_ms(u, u->check_ms));
    return NULL;
}

int updater_start(updater_t *u) {
    if (u->cfg->list_count == 0 || u->cfg->update_hours <= 0) {
        log_printf("list downloads off (%s)", u->cfg->list_count ? "update_hours=0" : "no [lists]");
        return 0;
    }
    pthread_mutex_init(&u->lock, NULL);
    wake_init(u);
    if (pthread_create(&u->thread, NULL, run, u) != 0) {
        plat_notify("ps5-adblock: cannot start the list updater");
        pthread_cond_destroy(&u->wake);
        pthread_mutex_destroy(&u->lock);
        return -1;
    }
    u->running = 1;
    return 0;
}

void updater_stop(updater_t *u) {
    if (!u->running)
        return;
    pthread_mutex_lock(&u->lock);
    u->stop = 1;
    pthread_cond_signal(&u->wake);
    pthread_mutex_unlock(&u->lock);
    pthread_join(u->thread, NULL);
    pthread_cond_destroy(&u->wake);
    pthread_mutex_destroy(&u->lock);
    fwd_close(&u->fwd);
    u->running = 0;
}
