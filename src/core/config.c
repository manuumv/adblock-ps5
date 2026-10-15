/* SPDX-License-Identifier: GPL-3.0-or-later
 * INI layout and defaults follow nanoDNS (c) Drakmor, GPLv3. */
#include "core/config.h"
#include "blocking/presets.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static const char DEFAULT_FILE[] =
    "# ps5-adblock configuration. Lines starting with # or ; are comments.\n"
    "\n"
    "[general]\n"
    "# debug=1 mirrors every query to the console log\n"
    "debug=0\n"
    "# quiet=1 hides the start notification (errors are always shown)\n"
    "quiet=0\n"
    "# autostart=1 keeps the payload in etaHEN/onionHEN payloads/ so it starts with the HEN\n"
    "autostart=1\n"
    "# address the DNS server listens on; 0.0.0.0 serves the whole LAN\n"
    "bind=127.0.0.1\n"
    "# IPv6 listener address, or off\n"
    "bind6=::1\n"
    "\n"
    "[upstream]\n"
    "# tried in order; timeout_ms is the total budget for one query\n"
    "server=1.1.1.1\n"
    "server=9.9.9.9\n"
    "timeout_ms=1500\n"
    "\n"
    "[overrides]\n"
    "# mask=IPv4. Masks accept *, ?, [abc], [a-z] and [!0-9]\n"
    "# *.ads.example.com=0.0.0.0\n"
    "# nas.home=192.168.1.10\n"
    "\n"
    "[exceptions]\n"
    "# masks that skip overrides and always go upstream\n"
    "# *.stun.playstation.net\n"
    "\n"
    "[blocking]\n"
    "# null answers 0.0.0.0 / ::, nxdomain answers \"no such domain\"\n"
    "block_mode=null\n"
    "# blocked domains are read from every .txt file in lists/; allowlist.txt wins over them\n"
    "max_domains=1500000\n"
    "# preset=update-block   blocks firmware updates and PSN (PSN features stop working)\n";

static const char *DEFAULT_UPSTREAMS[] = {"1.1.1.1", "9.9.9.9"};

enum section {
    SEC_NONE,
    SEC_GENERAL,
    SEC_UPSTREAM,
    SEC_OVERRIDES,
    SEC_EXCEPTIONS,
    SEC_BLOCKING,
    SEC_UNKNOWN
};

static void warn(config_t *c, unsigned line, const char *fmt, ...) {
    if (c->warning_count < CFG_MAX_WARNINGS) {
        char *w = c->warnings[c->warning_count];
        int n = line ? snprintf(w, CFG_WARNING_LEN, "config line %u: ", line) : 0;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(w + n, CFG_WARNING_LEN - (size_t)n, fmt, ap);
        va_end(ap);
    }
    c->warning_count++;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int add_upstream(config_t *c, const char *ip) {
    struct sockaddr_in *u = &c->upstreams[c->upstream_count];
    memset(u, 0, sizeof *u);
    if (inet_pton(AF_INET, ip, &u->sin_addr) != 1)
        return -1;
    u->sin_family = AF_INET;
    u->sin_port = htons(53);
    c->upstream_count++;
    return 0;
}

static void set_defaults(config_t *c) {
    memset(c, 0, sizeof *c);
    inet_pton(AF_INET, "127.0.0.1", &c->bind);
    inet_pton(AF_INET6, "::1", &c->bind6);
    c->bind6_enabled = 1;
    c->timeout_ms = 1500;
    c->max_domains = CFG_DEFAULT_MAX_DOMAINS;
    c->autostart = 1;
    for (size_t i = 0; i < sizeof DEFAULT_UPSTREAMS / sizeof *DEFAULT_UPSTREAMS; i++)
        add_upstream(c, DEFAULT_UPSTREAMS[i]);
}

/* Lowercases mask into out and drops a trailing dot. -1 if it is empty, too
 * long or has characters a mask cannot contain. */
static int normalize_mask(const char *mask, char out[DNS_NAME_MAX]) {
    size_t n = strlen(mask);
    if (n && mask[n - 1] == '.')
        n--;
    if (n == 0 || n > 253)
        return -1;
    for (size_t i = 0; i < n; i++) {
        char ch = (char)tolower((unsigned char)mask[i]);
        if (!isalnum((unsigned char)ch) && !strchr("-._*?[]!", ch))
            return -1;
        out[i] = ch;
    }
    out[n] = '\0';
    return 0;
}

static int parse_int(const char *s, int min, int max, int *out) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < min || v > max)
        return -1;
    *out = (int)v;
    return 0;
}

static void parse_general(config_t *c, unsigned line, const char *key, const char *val) {
    if (!strcmp(key, "debug")) {
        if (parse_int(val, 0, 1, &c->debug) != 0)
            warn(c, line, "debug must be 0 or 1");
    } else if (!strcmp(key, "quiet")) {
        if (parse_int(val, 0, 1, &c->quiet) != 0)
            warn(c, line, "quiet must be 0 or 1");
    } else if (!strcmp(key, "autostart")) {
        if (parse_int(val, 0, 1, &c->autostart) != 0)
            warn(c, line, "autostart must be 0 or 1");
    } else if (!strcmp(key, "bind")) {
        if (inet_pton(AF_INET, val, &c->bind) != 1) {
            inet_pton(AF_INET, "127.0.0.1", &c->bind);
            warn(c, line, "invalid IPv4 address '%s'", val);
        }
    } else if (!strcmp(key, "bind6")) {
        if (!strcmp(val, "off")) {
            c->bind6_enabled = 0;
        } else if (inet_pton(AF_INET6, val, &c->bind6) == 1) {
            c->bind6_enabled = 1;
        } else {
            inet_pton(AF_INET6, "::1", &c->bind6);
            warn(c, line, "invalid IPv6 address '%s'", val);
        }
    } else {
        warn(c, line, "unknown key '%s'", key);
    }
}

static void parse_upstream(config_t *c, unsigned line, const char *key, const char *val,
                           int *servers_seen) {
    if (!strcmp(key, "server")) {
        if (!*servers_seen) {
            c->upstream_count = 0; /* the file's list replaces the defaults */
            *servers_seen = 1;
        }
        if (c->upstream_count == CFG_MAX_UPSTREAMS)
            warn(c, line, "more than %d upstreams, ignored", CFG_MAX_UPSTREAMS);
        else if (add_upstream(c, val) != 0)
            warn(c, line, "invalid upstream '%s'", val);
    } else if (!strcmp(key, "timeout_ms")) {
        if (parse_int(val, 100, 30000, &c->timeout_ms) != 0)
            warn(c, line, "timeout_ms must be 100-30000");
    } else {
        warn(c, line, "unknown key '%s'", key);
    }
}

static void parse_override(config_t *c, unsigned line, const char *mask, const char *val) {
    rule_t *r = &c->rules[c->rule_count];
    if (c->rule_count == CFG_MAX_RULES)
        warn(c, line, "more than %d overrides, ignored", CFG_MAX_RULES);
    else if (normalize_mask(mask, r->mask) != 0)
        warn(c, line, "invalid mask '%s'", mask);
    else if (inet_pton(AF_INET, val, r->ip) != 1)
        warn(c, line, "invalid IPv4 address '%s'", val);
    else
        c->rule_count++;
}

static void parse_exception(config_t *c, unsigned line, const char *mask) {
    if (c->exception_count == CFG_MAX_EXCEPTIONS)
        warn(c, line, "more than %d exceptions, ignored", CFG_MAX_EXCEPTIONS);
    else if (normalize_mask(mask, c->exceptions[c->exception_count]) != 0)
        warn(c, line, "invalid mask '%s'", mask);
    else
        c->exception_count++;
}

static int has_mask(const char (*masks)[DNS_NAME_MAX], size_t n, const char *mask) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(masks[i], mask))
            return 1;
    return 0;
}

/* Appends mask to the list unless it is already there or the list is full. */
static void add_mask(config_t *c, unsigned line, char (*masks)[DNS_NAME_MAX], size_t *n,
                     size_t max, const char *mask) {
    if (has_mask((const char (*)[DNS_NAME_MAX])masks, *n, mask))
        return;
    if (*n == max)
        warn(c, line, "no room for preset mask '%s'", mask);
    else
        snprintf(masks[(*n)++], DNS_NAME_MAX, "%s", mask);
}

static void parse_blocking(config_t *c, unsigned line, const char *key, const char *val) {
    if (!strcmp(key, "block_mode")) {
        if (!strcmp(val, "null"))
            c->block_nxdomain = 0;
        else if (!strcmp(val, "nxdomain"))
            c->block_nxdomain = 1;
        else
            warn(c, line, "block_mode must be null or nxdomain");
    } else if (!strcmp(key, "max_domains")) {
        int v;
        if (parse_int(val, 1000, 10000000, &v) != 0)
            warn(c, line, "max_domains must be 1000-10000000");
        else
            c->max_domains = (size_t)v;
    } else if (!strcmp(key, "preset")) {
        const preset_t *p = preset_find(val);
        if (!p) {
            warn(c, line, "unknown preset '%s'", val);
            return;
        }
        for (const char *const *m = p->block; *m; m++)
            add_mask(c, line, c->block_masks, &c->block_mask_count, CFG_MAX_BLOCK_MASKS, *m);
        for (const char *const *m = p->allow; *m; m++)
            add_mask(c, line, c->exceptions, &c->exception_count, CFG_MAX_EXCEPTIONS, *m);
    } else {
        warn(c, line, "unknown key '%s'", key);
    }
}

static enum section section_of(const char *name) {
    if (!strcmp(name, "general"))
        return SEC_GENERAL;
    if (!strcmp(name, "upstream"))
        return SEC_UPSTREAM;
    if (!strcmp(name, "overrides"))
        return SEC_OVERRIDES;
    if (!strcmp(name, "exceptions"))
        return SEC_EXCEPTIONS;
    if (!strcmp(name, "blocking"))
        return SEC_BLOCKING;
    return SEC_UNKNOWN;
}

static void parse(FILE *f, config_t *c) {
    char buf[512];
    unsigned line = 0;
    enum section sec = SEC_NONE;
    int servers_seen = 0;

    while (fgets(buf, sizeof buf, f)) {
        line++;
        size_t n = strlen(buf); /* stops at an embedded NUL: the rest of that line is dropped */
        if (n == sizeof buf - 1 && buf[n - 1] != '\n' && !feof(f)) {
            warn(c, line, "line too long");
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') {
            }
            continue;
        }
        buf[strcspn(buf, "#;")] = '\0';
        char *s = trim(buf);
        if (!*s)
            continue;

        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end || end[1]) {
                warn(c, line, "malformed section header");
                sec = SEC_UNKNOWN;
                continue;
            }
            *end = '\0';
            sec = section_of(trim(s + 1));
            if (sec == SEC_UNKNOWN)
                warn(c, line, "unknown section '%s'", s + 1);
            continue;
        }
        if (sec == SEC_UNKNOWN)
            continue;
        if (sec == SEC_EXCEPTIONS) {
            parse_exception(c, line, s);
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            warn(c, line, "expected key=value");
            continue;
        }
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);
        switch (sec) {
        case SEC_GENERAL:
            parse_general(c, line, key, val);
            break;
        case SEC_UPSTREAM:
            parse_upstream(c, line, key, val, &servers_seen);
            break;
        case SEC_OVERRIDES:
            parse_override(c, line, key, val);
            break;
        case SEC_BLOCKING:
            parse_blocking(c, line, key, val);
            break;
        default:
            warn(c, line, "outside of a section");
        }
    }

    if (servers_seen && c->upstream_count == 0) {
        for (size_t i = 0; i < sizeof DEFAULT_UPSTREAMS / sizeof *DEFAULT_UPSTREAMS; i++)
            add_upstream(c, DEFAULT_UPSTREAMS[i]);
        warn(c, 0, "no valid upstream server, using 1.1.1.1 and 9.9.9.9");
    }
}

int config_load(const char *path, config_t *c) {
    set_defaults(c);

    int created = 0;
    FILE *f = fopen(path, "r");
    if (!f && errno == ENOENT) {
        FILE *w = fopen(path, "wx");
        if (w) {
            int ok = fputs(DEFAULT_FILE, w) >= 0;
            ok = (fclose(w) == 0) && ok;
            created = ok;
        }
        f = fopen(path, "r");
    }
    if (!f) {
        warn(c, 0, "cannot read %s: %s", path, strerror(errno));
        return -1;
    }
    parse(f, c);
    fclose(f);
    return created;
}
