/* SPDX-License-Identifier: GPL-3.0-or-later
 * platform.h for macOS and Linux development builds. Environment variables
 * stand in for the console:
 *   ADBLOCK_DATA       data directory (default ./data)
 *   ADBLOCK_PORT       DNS port (default 5300; 5353 belongs to mDNS on macOS)
 *   ADBLOCK_PEER_PID   PID of a "previous instance" for plat_find_pid
 *   ADBLOCK_HEN_DIRS   comma-separated HEN payload folders
 *   ADBLOCK_COPIES     comma-separated payload manager copies */
#include "core/platform.h"

#include <arpa/inet.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_PATHS 8

int plat_init(void) { return 0; }

void plat_fini(void) {}

const char *plat_data_dir(void) {
    const char *dir = getenv("ADBLOCK_DATA");
    return dir && *dir ? dir : "./data";
}

int plat_dns_port(void) {
    const char *port = getenv("ADBLOCK_PORT");
    int p = port ? atoi(port) : 0;
    return p > 0 && p < 65536 ? p : 5300;
}

int plat_find_pid(const char *name) {
    (void)name;
    const char *env = getenv("ADBLOCK_PEER_PID");
    pid_t pid = env ? (pid_t)atoi(env) : 0;
    if (pid <= 0 || pid == getpid())
        return -1;
    /* Reap it if it is our own exited child (tests), so it stops counting. */
    if (waitpid(pid, NULL, WNOHANG) == pid)
        return -1;
    return kill(pid, 0) == 0 ? (int)pid : -1;
}

int plat_kill(int pid) { return kill((pid_t)pid, SIGTERM); }

void plat_notify(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[notify] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* Splits a comma-separated variable into a NULL-terminated list. */
static const char *const *env_list(const char *var, char *buf, size_t n, const char **out) {
    size_t count = 0;
    const char *env = getenv(var);
    if (env) {
        snprintf(buf, n, "%s", env);
        for (char *tok = strtok(buf, ","); tok && count < MAX_PATHS; tok = strtok(NULL, ","))
            out[count++] = tok;
    }
    out[count] = NULL;
    return out;
}

/* A fake system DNS for the tests: ADBLOCK_SYSTEM_DNS="a.b.c.d[,e.f.g.h]" is
 * its starting value. Unset, a computer has no system DNS for us to redirect. */
static uint32_t fake_dns[2];
static int fake_dns_ready;

static uint32_t parse_ip(const char *s) {
    struct in_addr a;
    return s && inet_pton(AF_INET, s, &a) == 1 ? a.s_addr : 0;
}

int plat_system_dns_get(uint32_t servers[2]) {
    if (!fake_dns_ready) {
        const char *env = getenv("ADBLOCK_SYSTEM_DNS");
        char buf[64];
        if (!env)
            return -1;
        snprintf(buf, sizeof buf, "%s", env);
        char *second = strchr(buf, ',');
        if (second)
            *second++ = '\0';
        fake_dns[0] = parse_ip(buf);
        fake_dns[1] = parse_ip(second);
        fake_dns_ready = 1;
    }
    servers[0] = fake_dns[0];
    servers[1] = fake_dns[1];
    return 0;
}

int plat_system_dns_set(const uint32_t servers[2]) {
    uint32_t unused[2];
    if (plat_system_dns_get(unused) != 0)
        return -1;
    fake_dns[0] = servers[0];
    fake_dns[1] = servers[1];
    return 0;
}

int plat_console_dns(char primary[16], char secondary[16]) {
    (void)primary, (void)secondary;
    return -1; /* a computer's resolver settings are not ours to judge */
}

const char *const *plat_hen_dirs(void) {
    static char buf[4096];
    static const char *list[MAX_PATHS + 1];
    return env_list("ADBLOCK_HEN_DIRS", buf, sizeof buf, list);
}

const char *const *plat_payload_copies(void) {
    static char buf[4096];
    static const char *list[MAX_PATHS + 1];
    return env_list("ADBLOCK_COPIES", buf, sizeof buf, list);
}
