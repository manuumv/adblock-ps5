/* SPDX-License-Identifier: GPL-3.0-or-later
 * Shell-style masks for override rules and exceptions. */
#ifndef ADBLOCK_RULES_H
#define ADBLOCK_RULES_H

#include <stddef.h>
#include <stdint.h>

#include "dns/dns.h"

typedef struct {
    char mask[DNS_NAME_MAX]; /* lowercase, no trailing dot */
    uint8_t ip[4];
} rule_t;

/* 1 if name matches mask. Supports *, ?, [abc], [a-z] and [!...]. Both are
 * expected lowercase; an unterminated [ is a literal. */
int mask_match(const char *mask, const char *name);

/* First rule whose mask matches name, or NULL. */
const rule_t *rules_find(const rule_t *rules, size_t n, const char *name);

/* 1 if any mask matches name. */
int masks_match_any(const char (*masks)[DNS_NAME_MAX], size_t n, const char *name);

#endif
