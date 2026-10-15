/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char ring[LOG_LINES][LOG_LINE_LEN];
static size_t next;  /* index of the slot written next */
static size_t count; /* lines kept, up to LOG_LINES */
static size_t total; /* lines ever written */
static int debug;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void log_set_debug(int on) { debug = on; }

int log_debug_enabled(void) { return debug; }

void log_printf(const char *fmt, ...) {
    char line[LOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&lock);
    memcpy(ring[next], line, sizeof line);
    next = (next + 1) % LOG_LINES;
    if (count < LOG_LINES)
        count++;
    total++;
    pthread_mutex_unlock(&lock);

    if (debug)
        fprintf(stderr, "[ps5-adblock] %s\n", line);
}

size_t log_line(size_t i, char *out, size_t n) {
    pthread_mutex_lock(&lock);
    size_t kept = count;
    if (i < kept && n) {
        size_t slot = (next + LOG_LINES - kept + i) % LOG_LINES;
        snprintf(out, n, "%s", ring[slot]);
    }
    pthread_mutex_unlock(&lock);
    return kept;
}

size_t log_total(void) {
    pthread_mutex_lock(&lock);
    size_t t = total;
    pthread_mutex_unlock(&lock);
    return t;
}
