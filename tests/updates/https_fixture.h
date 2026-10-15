/* Test helpers for tests/updates: self-signed certificates for "list.test" and
 * "mirror.test", and the Python HTTPS server in https_server.py. Header-only,
 * test code only. */
#ifndef ADBLOCK_TEST_HTTPS_FIXTURE_H
#define ADBLOCK_TEST_HTTPS_FIXTURE_H

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define FIXTURE_HOST "list.test"
#define FIXTURE_MIRROR "mirror.test" /* a second host on the same certificate, for redirects */

typedef struct {
    char dir[64];      /* scratch directory */
    char docroot[256]; /* served at / (list.txt) */
    char cert[256], key[256];             /* valid certificate for list.test */
    char other_cert[256], other_key[256]; /* a different self-signed certificate */
    char expired_cert[256], expired_key[256];
    int have_expired; /* this openssl can backdate certificates */
} https_fixture_t;

static void fixture_path(char *out, const https_fixture_t *f, const char *name) {
    snprintf(out, 256, "%s/%s", f->dir, name);
}

static int make_cert(const char *cert, const char *key, const char *extra) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "openssl req -x509 -newkey rsa:2048 -nodes -subj /CN=" FIXTURE_HOST
             " -addext subjectAltName=DNS:" FIXTURE_HOST ",DNS:" FIXTURE_MIRROR " %s -keyout '%s' -out '%s' >/dev/null 2>&1",
             extra, key, cert);
    return system(cmd) == 0;
}

static void fixture_init(https_fixture_t *f) {
    snprintf(f->dir, sizeof f->dir, "/tmp/adblock-test-https-XXXXXX");
    assert(mkdtemp(f->dir));
    fixture_path(f->docroot, f, "www");
    char cmd[512];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", f->docroot);
    assert(system(cmd) == 0);
    fixture_path(f->cert, f, "cert.pem");
    fixture_path(f->key, f, "key.pem");
    fixture_path(f->other_cert, f, "other.pem");
    fixture_path(f->other_key, f, "other-key.pem");
    fixture_path(f->expired_cert, f, "expired.pem");
    fixture_path(f->expired_key, f, "expired-key.pem");
    assert(make_cert(f->cert, f->key, "-days 2"));
    assert(make_cert(f->other_cert, f->other_key, "-days 2"));
    /* OpenSSL 3.4+ can set the validity window explicitly. */
    f->have_expired = make_cert(f->expired_cert, f->expired_key,
                                "-not_before 20200101000000Z -not_after 20200102000000Z");
}

static void fixture_write(const https_fixture_t *f, const char *name, const char *text) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", f->docroot, name);
    FILE *out = fopen(path, "w");
    assert(out);
    fputs(text, out);
    fclose(out);
}

/* Starts https_server.py with cert/key; returns its port, *pid gets the child. */
static int fixture_serve(const https_fixture_t *f, const char *cert, const char *key, pid_t *pid) {
    char portfile[256];
    snprintf(portfile, sizeof portfile, "%s/port-%ld", f->dir, (long)random());
    *pid = fork();
    assert(*pid >= 0);
    if (*pid == 0) {
        /* Do not hold the test's stdout/stderr: a crashed test must not leave
         * a pipe open through its orphaned server. */
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, 1);
        dup2(devnull, 2);
        execlp("python3", "python3", "-I", "tests/updates/https_server.py", cert, key, f->docroot,
               portfile, (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 100; i++) {
        FILE *pf = fopen(portfile, "r");
        if (pf) {
            int port = 0;
            assert(fscanf(pf, "%d", &port) == 1);
            fclose(pf);
            return port;
        }
        usleep(50 * 1000);
    }
    assert(!"https server did not start");
    return 0;
}

static void fixture_stop(pid_t pid) {
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

static void fixture_cleanup(https_fixture_t *f) {
    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", f->dir);
    (void)system(cmd);
}

#endif
