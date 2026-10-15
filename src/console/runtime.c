/* SPDX-License-Identifier: GPL-3.0-or-later
 * Single instance follows nanoDNS (c) Drakmor, GPLv3. */
#include "console/runtime.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "core/log.h"
#include "core/platform.h"

static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

int runtime_take_over(const char *name, int timeout_ms) {
    int waited = 0, killed = -1, pid;
    while ((pid = plat_find_pid(name)) != -1) {
        if (pid == PLAT_PID_UNKNOWN) {
            /* Better to start and let a busy port 53 be reported than to refuse. */
            log_printf("cannot list processes, not checking for a previous instance");
            return 0;
        }
        if (pid != killed) { /* a second stale copy gets stopped too */
            log_printf("stopping previous instance (pid %d)", pid);
            if (plat_kill(pid) != 0)
                log_printf("cannot stop pid %d: %s", pid, strerror(errno));
            killed = pid;
        }
        if (waited >= timeout_ms) {
            log_printf("previous instance (pid %d) did not stop", pid);
            return -1;
        }
        sleep_ms(250);
        waited += 250;
    }
    return 0;
}

/* --- autostart --- */

static int stat_file(const char *path, struct stat *st) {
    return stat(path, st) == 0 && S_ISREG(st->st_mode);
}

/* Copies src to dst through dst.tmp + fsync + rename, then gives dst src's
 * mtime: a power cut never leaves a truncated ELF where the HEN runs it.
 * -1 with errno set on failure. */
static int copy_file(const char *src, const char *dst, time_t mtime) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    FILE *in = fopen(src, "rb");
    if (!in)
        return -1;
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        int err = errno;
        fclose(in);
        errno = err;
        return -1;
    }
    char buf[16384];
    size_t n;
    int err = 0;
    while (!err && (n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n)
            err = errno ? errno : EIO;
    if (!err && ferror(in))
        err = EIO;
    if (!err && (fflush(out) != 0 || fsync(fileno(out)) != 0))
        err = errno;
    fclose(in);
    if (fclose(out) != 0 && !err)
        err = errno;
    struct timeval tv[2] = {{mtime, 0}, {mtime, 0}};
    if (!err && (utimes(tmp, tv) != 0 || rename(tmp, dst) != 0))
        err = errno;
    if (err) {
        unlink(tmp);
        errno = err;
        return -1;
    }
    return 0;
}

/* dst needs refreshing from a source with this mtime and size. */
static int stale(const char *dst, const struct stat *src) {
    struct stat st;
    return !stat_file(dst, &st) || st.st_mtime < src->st_mtime ||
           (st.st_mtime == src->st_mtime && st.st_size != src->st_size);
}

static int ensure_marker(const char *dir) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, AUTOSTART_MARKER);
    if (access(path, F_OK) == 0)
        return 0;
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    return 1;
}

static void consider(const char *path, char *best, struct stat *best_st, int *found) {
    struct stat st;
    if (stat_file(path, &st) && (!*found || st.st_mtime > best_st->st_mtime)) {
        snprintf(best, PATH_MAX, "%s", path);
        *best_st = st;
        *found = 1;
    }
}

/* Remembers the first failure for the summary and keeps going. */
static void note_error(char *first, size_t n, const char *what, const char *path) {
    if (!first[0])
        snprintf(first, n, "autostart: %s %s: %s", what, path, strerror(errno));
}

int autostart_sync(int enable, const char *const *hen_dirs, const char *const *copies,
                   const char *own_copy, char *msg, size_t n) {
    char path[PATH_MAX];
    char error[256] = "";
    int changed = 0;

    if (!hen_dirs[0]) {
        snprintf(msg, n, "autostart: no HEN folder found (etaHEN or onionHEN)");
        return enable ? -1 : 0;
    }

    if (!enable) {
        for (const char *const *d = hen_dirs; *d; d++) {
            snprintf(path, sizeof path, "%s/%s", *d, AUTOSTART_MARKER);
            if (unlink(path) == 0)
                changed++;
            else if (errno != ENOENT)
                note_error(error, sizeof error, "cannot remove", path);
        }
        snprintf(msg, n, "%s", error[0] ? error : "autostart off");
        return error[0] ? -1 : changed;
    }

    char best[PATH_MAX];
    struct stat best_st;
    int found = 0;
    for (const char *const *d = hen_dirs; *d; d++) {
        snprintf(path, sizeof path, "%s/%s", *d, AUTOSTART_ELF);
        consider(path, best, &best_st, &found);
    }
    for (const char *const *c = copies; *c; c++)
        consider(*c, best, &best_st, &found);
    if (own_copy)
        consider(own_copy, best, &best_st, &found);
    if (!found) {
        snprintf(msg, n, "no copy on disk: autostart needs make install or the installer");
        return AUTOSTART_NO_COPY;
    }

    for (const char *const *d = hen_dirs; *d; d++) {
        if (mkdir(*d, 0755) != 0 && errno != EEXIST) {
            note_error(error, sizeof error, "cannot create", *d);
            continue;
        }
        snprintf(path, sizeof path, "%s/%s", *d, AUTOSTART_ELF);
        if (strcmp(path, best) != 0 && stale(path, &best_st)) {
            if (copy_file(best, path, best_st.st_mtime) != 0) {
                note_error(error, sizeof error, "cannot write", path);
                continue;
            }
            changed++;
        }
        int m = ensure_marker(*d);
        if (m < 0) {
            snprintf(path, sizeof path, "%s/%s", *d, AUTOSTART_MARKER);
            note_error(error, sizeof error, "cannot create", path);
        } else {
            changed += m;
        }
    }
    for (const char *const *c = copies; *c; c++) {
        struct stat st;
        if (stat_file(*c, &st) && strcmp(*c, best) != 0 && stale(*c, &best_st)) {
            if (copy_file(best, *c, best_st.st_mtime) != 0)
                note_error(error, sizeof error, "cannot write", *c);
            else
                changed++;
        }
    }
    if (error[0]) {
        snprintf(msg, n, "%s", error);
        return -1;
    }
    snprintf(msg, n, "autostart on, %d file(s) updated from %s", changed, best);
    return changed;
}

/* --- notices --- */

void runtime_ready_notice(const config_t *c, size_t domains) {
    if (c->quiet)
        return;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &c->bind, ip, sizeof ip);
    plat_notify("ps5-adblock %s: blocking %zu domains, DNS %s", ADBLOCK_VERSION, domains, ip);
}

void runtime_port_error_notice(int port) {
    plat_notify("ps5-adblock: cannot open DNS port %d. Set DNS to Automatic to get online, "
                "then send the payload again.",
                port);
}

/* --- log persistence --- */

static char log_path[PATH_MAX];
static size_t flushed_total;
static int64_t last_flush;

static int log_write_failed; /* told the user once */

static void write_failed(void) {
    if (!log_write_failed)
        plat_notify("ps5-adblock: cannot write %s (%s)", log_path, strerror(errno));
    log_write_failed = 1;
}

static void write_log(void) {
    char tmp[PATH_MAX + 4], line[LOG_LINE_LEN];
    snprintf(tmp, sizeof tmp, "%s.tmp", log_path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        write_failed();
        return;
    }
    size_t total = log_total();
    size_t kept = log_line(0, line, sizeof line);
    for (size_t i = 0; i < kept; i++) {
        log_line(i, line, sizeof line);
        fprintf(f, "%s\n", line);
    }
    if (fclose(f) == 0 && rename(tmp, log_path) == 0) {
        flushed_total = total;
    } else {
        write_failed();
        unlink(tmp);
    }
}

void runtime_log_start(const char *dir, int64_t now) {
    char prev[PATH_MAX + 32];
    snprintf(log_path, sizeof log_path, "%s/last.log", dir);
    snprintf(prev, sizeof prev, "%s/last.previous.log", dir);
    (void)rename(log_path, prev);
    flushed_total = 0; /* startup lines are already in the ring and still unwritten */
    last_flush = now;
}

void runtime_log_tick(int64_t now) {
    if (!log_path[0] || now - last_flush < LOG_FLUSH_MS || log_total() == flushed_total)
        return;
    last_flush = now;
    write_log();
}

void runtime_log_flush(void) {
    if (log_path[0] && log_total() != flushed_total)
        write_log();
}
