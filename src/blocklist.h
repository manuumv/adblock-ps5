/* SPDX-License-Identifier: GPL-3.0-or-later
 * Large domain sets: sorted 64-bit hashes with suffix lookup. */
#ifndef ADBLOCK_BLOCKLIST_H
#define ADBLOCK_BLOCKLIST_H

#include <stddef.h>
#include <stdint.h>

#include "listparse.h"

typedef struct {
    uint64_t *hashes; /* sorted, unique */
    size_t count;
} domain_set_t;

typedef struct {
    domain_set_t blocked;
    domain_set_t allowed;
    list_stats_t stats; /* totals over every list file */
    size_t files;
    size_t skipped_files; /* list files beyond the 64 that are read */
    int truncated; /* max_domains was reached */
} blocklist_t;

/* Keyed hash of a domain. Set a random key once at startup, before any set is
 * built; sets are only valid under the key they were built with. */
void domain_hash_set_key(const uint8_t key[16]);
uint64_t domain_hash(const char *name, size_t len);

/* Sorts and deduplicates hashes in place and takes ownership of the array. */
void set_build(domain_set_t *s, uint64_t *hashes, size_t n);

/* 1 if name (normalized) or any parent domain of it is in the set. */
int set_contains_suffix(const domain_set_t *s, const char *name);

void set_free(domain_set_t *s);

/* Loads every .txt file in <data_dir>/lists (up to 64) into blocked and
 * <data_dir>/allowlist.txt (up to 100k entries) into allowed, keeping at most
 * max_domains blocked domains. A missing allowlist is
 * an empty set. NULL when memory runs out or a list file cannot be read. */
blocklist_t *blocklist_load(const char *data_dir, size_t max_domains);
void blocklist_free(blocklist_t *b);

/* Changes whenever a list file or the allowlist is added, removed or modified. */
uint64_t blocklist_fingerprint(const char *data_dir);

#endif
