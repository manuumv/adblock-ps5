#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "runtime.h"

static char root[] = "/tmp/adblock-test-autostart-XXXXXX";
static char eta[256], onion[256], pld_dir[256], pld[256], own_dir[256], own[256];
static char msg[256];

static void path_in(char *out, const char *dir, const char *name) {
    snprintf(out, 256, "%s/%s", dir, name);
}

static void write_file(const char *path, const char *text, time_t mtime) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
    struct timeval tv[2] = {{mtime, 0}, {mtime, 0}};
    assert(utimes(path, tv) == 0);
}

static int exists(const char *path) { return access(path, F_OK) == 0; }

static time_t mtime_of(const char *path) {
    struct stat st;
    assert(stat(path, &st) == 0);
    return st.st_mtime;
}

static void read_file(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r");
    assert(f);
    size_t got = fread(buf, 1, n - 1, f);
    buf[got] = '\0';
    fclose(f);
}

int main(void) {
    assert(mkdtemp(root));
    char p[256], q[256], buf[64];
    /* Two HENs installed: their payload folders may not exist yet. */
    snprintf(eta, sizeof eta, "%s/etaHEN/payloads", root);
    snprintf(onion, sizeof onion, "%s/OnionHEN/payloads", root);
    snprintf(p, sizeof p, "%s/etaHEN", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/OnionHEN", root);
    mkdir(p, 0755);
    snprintf(pld_dir, sizeof pld_dir, "%s/pldmgr", root);
    path_in(pld, pld_dir, AUTOSTART_ELF);
    snprintf(own_dir, sizeof own_dir, "%s/ps5-adblock", root);
    mkdir(own_dir, 0755);
    path_in(own, own_dir, AUTOSTART_ELF);
    const char *hens[] = {eta, onion, NULL};
    const char *copies[] = {pld, NULL};

    /* Sent over the network, nothing on disk: nothing to install from. */
    assert(autostart_sync(1, hens, copies, own, msg, sizeof msg) == AUTOSTART_NO_COPY);
    assert(strstr(msg, "make install"));
    path_in(p, eta, AUTOSTART_ELF);
    assert(!exists(p));

    /* make install put v1 in etaHEN: it spreads to onionHEN, both get the marker. */
    mkdir(eta, 0755);
    write_file(p, "elf-v1", 1000);
    assert(autostart_sync(1, hens, copies, own, msg, sizeof msg) == 3); /* onion elf + 2 markers */
    path_in(q, onion, AUTOSTART_ELF);
    read_file(q, buf, sizeof buf);
    assert(!strcmp(buf, "elf-v1") && mtime_of(q) == 1000);
    path_in(q, eta, AUTOSTART_MARKER);
    assert(exists(q));
    path_in(q, onion, AUTOSTART_MARKER);
    assert(exists(q));
    assert(!exists(pld)); /* never created */

    /* Second start: nothing to do, no churn. */
    assert(autostart_sync(1, hens, copies, own, msg, sizeof msg) == 0);

    /* pldmgr keeps an old copy; a newer one appears in onionHEN: both others update. */
    mkdir(pld_dir, 0755);
    write_file(pld, "elf-v0", 500);
    path_in(q, onion, AUTOSTART_ELF);
    write_file(q, "elf-v2", 2000);
    assert(autostart_sync(1, hens, copies, own, msg, sizeof msg) == 2); /* eta elf + pldmgr */
    read_file(pld, buf, sizeof buf);
    assert(!strcmp(buf, "elf-v2") && mtime_of(pld) == 2000);
    read_file(p, buf, sizeof buf);
    assert(!strcmp(buf, "elf-v2"));

    /* autostart=0: markers go, ELFs and the data folder stay. */
    path_in(q, own_dir, "config.ini");
    write_file(q, "[general]\nautostart=0\n", 3000);
    assert(autostart_sync(0, hens, copies, own, msg, sizeof msg) == 2);
    path_in(q, eta, AUTOSTART_MARKER);
    assert(!exists(q));
    path_in(q, onion, AUTOSTART_MARKER);
    assert(!exists(q));
    assert(exists(p) && exists(pld));
    path_in(q, own_dir, "config.ini");
    assert(exists(q));
    assert(autostart_sync(0, hens, copies, own, msg, sizeof msg) == 0);

    /* No HEN folder found: that is an error, not "autostart on". */
    const char *none[] = {NULL};
    assert(autostart_sync(1, none, copies, own, msg, sizeof msg) == -1);
    assert(strstr(msg, "no HEN"));

    /* One folder failing does not stop the others, and the error names it. */
    if (geteuid() != 0) { /* root writes anywhere */
        char locked[256], locked_dir[256];
        snprintf(locked, sizeof locked, "%s/locked", root);
        mkdir(locked, 0555);
        snprintf(locked_dir, sizeof locked_dir, "%s/payloads", locked);
        const char *mixed[] = {locked_dir, onion, NULL};
        path_in(q, onion, AUTOSTART_MARKER);
        assert(!exists(q));
        assert(autostart_sync(1, mixed, copies, own, msg, sizeof msg) == -1);
        assert(exists(q)); /* onion still got its marker */
        assert(strstr(msg, locked_dir));
        chmod(locked, 0755);
    }

    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -r '%s'", root);
    assert(system(cmd) == 0);
    puts("test_autostart: ok");
    return 0;
}
