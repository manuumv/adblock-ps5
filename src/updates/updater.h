/* SPDX-License-Identifier: GPL-3.0-or-later
 * Keeps the [lists] of config.ini downloaded into lists/<name>.txt on a
 * schedule, on its own thread. It only writes files: the reload watcher in
 * dns/server.c picks them up. */
#ifndef ADBLOCK_UPDATER_H
#define ADBLOCK_UPDATER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#include "core/config.h"
#include "dns/forward.h"

#define UPDATER_FIRST_DELAY_MS (2 * 60 * 1000)
#define UPDATER_CHECK_MS (60 * 60 * 1000)
#define UPDATER_MAX_BYTES (64L * 1024 * 1024)
#define UPDATER_TIMEOUT_S 120L

typedef struct {
    const config_t *cfg;
    const char *data_dir;
    int64_t first_delay_ms, check_ms; /* tests shorten these */
    const char *ca_file;              /* tests trust their own CA; NULL in production */

    forwarder_t fwd;                    /* resolves list hosts straight at the upstreams */
    int failure_notified[CFG_MAX_LISTS]; /* one notice per failing list until it recovers */

    pthread_t thread;
    int running;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    clockid_t wake_clock; /* what wake's deadlines are measured on */
    atomic_int stop; /* read by the thread and libcurl's progress callback without the lock */
} updater_t;

/* Prepares u with production timings. */
void updater_init(updater_t *u, const config_t *cfg, const char *data_dir);

/* Starts the thread unless there is nothing to do (no lists or
 * update_hours=0). The first pass runs first_delay_ms after start. */
int updater_start(updater_t *u);

/* Stops the thread, aborting a download in progress. */
void updater_stop(updater_t *u);

/* One pass: downloads every list that is missing or older than update_hours,
 * as of now (seconds since the epoch). Runs on the caller's thread. */
void updater_run_once(updater_t *u, int64_t now);

#endif
