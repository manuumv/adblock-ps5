/* SPDX-License-Identifier: GPL-3.0-or-later
 * config.ini: defaults, creation on first start, parsing with per-line warnings. */
#ifndef ADBLOCK_CONFIG_H
#define ADBLOCK_CONFIG_H

#include <netinet/in.h>
#include <stddef.h>

#include "rules.h"

#define CFG_MAX_UPSTREAMS 8
#define CFG_MAX_RULES 256
#define CFG_MAX_EXCEPTIONS 256
#define CFG_MAX_BLOCK_MASKS 64
#define CFG_DEFAULT_MAX_DOMAINS 1500000
#define CFG_MAX_WARNINGS 32
#define CFG_WARNING_LEN 160

typedef struct {
    int debug;
    int quiet;     /* 1: no start notification (errors are always shown) */
    int autostart; /* 1: keep the payload in every HEN's autostart folder */
    struct in_addr bind;
    struct in6_addr bind6;
    int bind6_enabled;

    struct sockaddr_in upstreams[CFG_MAX_UPSTREAMS];
    size_t upstream_count;
    int timeout_ms;

    rule_t rules[CFG_MAX_RULES];
    size_t rule_count;
    char exceptions[CFG_MAX_EXCEPTIONS][DNS_NAME_MAX]; /* also the presets' allow masks */
    size_t exception_count;

    int block_nxdomain;   /* block_mode=nxdomain; default null (0.0.0.0 / ::) */
    size_t max_domains;   /* cap on domains loaded from lists */
    char block_masks[CFG_MAX_BLOCK_MASKS][DNS_NAME_MAX]; /* from presets */
    size_t block_mask_count;

    /* The first CFG_MAX_WARNINGS warnings; warning_count counts them all. */
    char warnings[CFG_MAX_WARNINGS][CFG_WARNING_LEN];
    size_t warning_count;
} config_t;

/* Fills c with defaults, creates path with a commented default file when it
 * does not exist, then applies it. Returns 1 if the file was created, 0 if it
 * was read, -1 if it could not be created or read (c keeps the defaults). */
int config_load(const char *path, config_t *c);

#endif
