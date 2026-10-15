/* SPDX-License-Identifier: GPL-3.0-or-later
 * Blocklist file formats: hosts, one domain per line, and pure-domain Adblock
 * rules (||domain^). */
#ifndef ADBLOCK_LISTPARSE_H
#define ADBLOCK_LISTPARSE_H

#include <stddef.h>

#include "dns.h"

enum {
    LP_DOMAIN = 1,       /* out holds a normalized domain */
    LP_SKIP = 0,         /* blank, comment, or a local name such as localhost */
    LP_UNSUPPORTED = -1, /* valid Adblock syntax this blocker cannot apply */
    LP_INVALID = -2,     /* not a domain */
};

typedef struct {
    size_t lines;
    size_t domains;
    size_t unsupported;
    size_t invalid;
} list_stats_t;

/* Parses one line (with or without its line ending). */
int list_parse_line(const char *line, char out[DNS_NAME_MAX]);

/* Calls fn for every domain in the file and adds to st. Lines longer than
 * LIST_LINE_MAX count as invalid. -1 (errno set) if the file cannot be
 * opened, is not a regular file, is reached through a symlink, or is over
 * 256 MiB. */
#define LIST_LINE_MAX 512
int list_read_file(const char *path, void (*fn)(void *ud, const char *domain), void *ud,
                   list_stats_t *st);

#endif
