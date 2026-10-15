/* SPDX-License-Identifier: GPL-3.0-or-later
 * Same mask syntax as nanoDNS (c) Drakmor, matched iteratively: one saved
 * star position bounds the work to O(len(mask) * len(name)). */
#include "blocking/rules.h"

/* Length of the mask token at p if it matches c, else 0. A token is one
 * literal, '?', or a complete [...] class. */
static size_t token_match(const char *p, char c) {
    if (*p == '?')
        return 1;
    if (*p != '[')
        return *p == c ? 1 : 0;

    const char *s = p + 1;
    int negate = *s == '!';
    if (negate)
        s++;
    const char *first = s;
    int hit = 0;
    while (*s && (*s != ']' || s == first)) {
        if (s[1] == '-' && s[2] && s[2] != ']') {
            if (c >= s[0] && c <= s[2])
                hit = 1;
            s += 3;
        } else {
            if (c == *s)
                hit = 1;
            s++;
        }
    }
    if (*s != ']')
        return c == '[' ? 1 : 0; /* unterminated: a literal '[' */
    return hit != negate ? (size_t)(s - p + 1) : 0;
}

int mask_match(const char *mask, const char *name) {
    const char *star = NULL; /* mask position after the last '*' */
    const char *retry = NULL; /* name position that star is currently absorbing up to */

    while (*name) {
        if (*mask == '*') {
            while (*mask == '*')
                mask++;
            if (!*mask)
                return 1;
            star = mask;
            retry = name;
            continue;
        }
        size_t k = *mask ? token_match(mask, *name) : 0;
        if (k) {
            mask += k;
            name++;
            continue;
        }
        if (!star)
            return 0;
        mask = star;
        name = ++retry;
    }
    while (*mask == '*')
        mask++;
    return *mask == '\0';
}

const rule_t *rules_find(const rule_t *rules, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++)
        if (mask_match(rules[i].mask, name))
            return &rules[i];
    return NULL;
}

int masks_match_any(const char (*masks)[DNS_NAME_MAX], size_t n, const char *name) {
    for (size_t i = 0; i < n; i++)
        if (mask_match(masks[i], name))
            return 1;
    return 0;
}
