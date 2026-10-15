/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "blocklist.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* SipHash-2-4 (Aumasson & Bernstein, public domain reference). Keyed, so a
 * list author cannot craft a name whose hash equals another domain's (or a
 * whole TLD's) and block it unseen. */
static uint64_t key0, key1;

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND                                                                           \
    do {                                                                                   \
        v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32);                          \
        v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2;                                             \
        v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0;                                             \
        v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32);                          \
    } while (0)

static uint64_t le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

void domain_hash_set_key(const uint8_t key[16]) {
    key0 = le64(key);
    key1 = le64(key + 8);
}

uint64_t domain_hash(const char *name, size_t len) {
    const uint8_t *in = (const uint8_t *)name;
    uint64_t v0 = 0x736f6d6570736575ULL ^ key0, v1 = 0x646f72616e646f6dULL ^ key1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ key0, v3 = 0x7465646279746573ULL ^ key1;
    size_t end = len - len % 8;
    for (size_t i = 0; i < end; i += 8) {
        uint64_t m = le64(in + i);
        v3 ^= m;
        SIPROUND;
        SIPROUND;
        v0 ^= m;
    }
    uint64_t b = (uint64_t)len << 56;
    for (size_t i = 0; i < len % 8; i++)
        b |= (uint64_t)in[end + i] << (8 * i);
    v3 ^= b;
    SIPROUND;
    SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

void set_build(domain_set_t *s, uint64_t *hashes, size_t n) {
    if (n)
        qsort(hashes, n, sizeof *hashes, cmp_u64);
    size_t out = 0;
    for (size_t i = 0; i < n; i++)
        if (out == 0 || hashes[i] != hashes[out - 1])
            hashes[out++] = hashes[i];
    s->hashes = hashes;
    s->count = out;
}

static int contains(const domain_set_t *s, uint64_t h) {
    size_t lo = 0, hi = s->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->hashes[mid] < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < s->count && s->hashes[lo] == h;
}

int set_contains_suffix(const domain_set_t *s, const char *name) {
    if (!s->count || !*name)
        return 0;
    size_t len = strlen(name);
    for (const char *p = name; p; p = strchr(p, '.')) {
        if (*p == '.')
            p++;
        if (contains(s, domain_hash(p, len - (size_t)(p - name))))
            return 1;
    }
    return 0;
}

void set_free(domain_set_t *s) {
    free(s->hashes);
    s->hashes = NULL;
    s->count = 0;
}

#define MAX_LIST_FILES 64
#define ALLOWLIST_MAX 100000
#define MAX_DIR_ENTRIES 1024 /* bounds the fingerprint scan on the DNS thread */

typedef struct {
    uint64_t *hashes;
    size_t count, cap;
    int overflow;
} collector_t;

static void count_one(void *ud, const char *domain) {
    (void)domain;
    (*(size_t *)ud)++;
}

static void add_one(void *ud, const char *domain) {
    collector_t *c = ud;
    if (c->count < c->cap)
        c->hashes[c->count++] = domain_hash(domain, strlen(domain));
    else
        c->overflow = 1;
}

static int is_list_file(const char *name) {
    size_t n = strlen(name);
    return name[0] != '.' && n > 4 && !strcmp(name + n - 4, ".txt");
}

/* Paths of the list files in dir, at most MAX_LIST_FILES; *skipped counts
 * the list files beyond that. */
static size_t list_files(const char *dir, char paths[][PATH_MAX], size_t *skipped) {
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (is_list_file(e->d_name)) {
            if (n < MAX_LIST_FILES)
                snprintf(paths[n++], PATH_MAX, "%s/%s", dir, e->d_name);
            else
                (*skipped)++;
        }
    closedir(d);
    return n;
}

/* Two passes so the hash array is allocated once, at its exact size: no
 * realloc doubling at peak memory. A file that exists but cannot be read
 * fails the load (a half-applied set is worse than the previous one); a
 * missing file counts as empty. Returns -1 on failure. */
static int load_set(char paths[][PATH_MAX], size_t files, size_t max, domain_set_t *out,
                    list_stats_t *st, int *truncated) {
    size_t total = 0;
    list_stats_t ignored = {0};
    for (size_t i = 0; i < files; i++)
        if (list_read_file(paths[i], count_one, &total, &ignored) != 0 && errno != ENOENT)
            return -1;
    if (total > max) {
        total = max;
        *truncated = 1;
    }

    collector_t c = {.hashes = malloc(total * sizeof(uint64_t) + 1), .cap = total};
    if (!c.hashes)
        return -1;
    for (size_t i = 0; i < files; i++)
        if (list_read_file(paths[i], add_one, &c, st) != 0 && errno != ENOENT) {
            free(c.hashes); /* changed between the passes: do not publish half a set */
            return -1;
        }
    if (c.overflow)
        *truncated = 1;
    set_build(out, c.hashes, c.count);
    return 0;
}

blocklist_t *blocklist_load(const char *data_dir, size_t max_domains) {
    static char paths[MAX_LIST_FILES][PATH_MAX]; /* only the builder thread loads */
    char dir[PATH_MAX];
    blocklist_t *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;

    snprintf(dir, sizeof dir, "%s/lists", data_dir);
    b->files = list_files(dir, paths, &b->skipped_files);
    if (load_set(paths, b->files, max_domains, &b->blocked, &b->stats, &b->truncated) != 0)
        goto fail;

    snprintf(paths[0], PATH_MAX, "%s/allowlist.txt", data_dir);
    list_stats_t allow_stats = {0};
    int allow_truncated = 0;
    if (load_set(paths, 1, ALLOWLIST_MAX, &b->allowed, &allow_stats, &allow_truncated) != 0)
        goto fail;
    return b;

fail:
    blocklist_free(b);
    return NULL;
}

void blocklist_free(blocklist_t *b) {
    if (!b)
        return;
    set_free(&b->blocked);
    set_free(&b->allowed);
    free(b);
}

/* splitmix64 finalizer: mixes each file's fields before they are summed, so
 * changes in two files cannot cancel out. */
static uint64_t mix(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static uint64_t file_print(const char *path, const char *name) {
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    uint64_t h = domain_hash(name, strlen(name));
    h = mix(h ^ (uint64_t)st.st_size);
    h = mix(h ^ (uint64_t)st.st_mtime);
    return mix(h ^ (uint64_t)st.st_ino);
}

uint64_t blocklist_fingerprint(const char *data_dir) {
    char path[PATH_MAX];
    uint64_t sum = 0;

    snprintf(path, sizeof path, "%s/allowlist.txt", data_dir);
    sum += file_print(path, "allowlist.txt");

    snprintf(path, sizeof path, "%s/lists", data_dir);
    DIR *d = opendir(path);
    if (!d)
        return sum;
    struct dirent *e;
    size_t seen = 0;
    while ((e = readdir(d)) && seen++ < MAX_DIR_ENTRIES) /* summed: order does not matter */
        if (is_list_file(e->d_name)) {
            char file[PATH_MAX];
            snprintf(file, sizeof file, "%s/lists/%s", data_dir, e->d_name);
            sum += file_print(file, e->d_name);
        }
    closedir(d);
    return sum;
}
