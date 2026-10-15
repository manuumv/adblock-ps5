/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "blocking/listparse.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIST_FILE_MAX (256L * 1024 * 1024)

/* ASCII-only character classes: lists are ASCII and setlocale must not matter. */
static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
static int is_alnum(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
static char to_lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static const char *const LOCAL_NAMES[] = {
    "localhost", "localhost.localdomain", "local", "broadcasthost", "ip6-localhost",
    "ip6-loopback", "ip6-localnet", "ip6-mcastprefix", "ip6-allnodes", "ip6-allrouters",
    "ip6-allhosts", "0.0.0.0",
};

typedef struct {
    const char *p;
    size_t n;
} span_t;

static int span_is(span_t s, const char *lit) { return strlen(lit) == s.n && !memcmp(s.p, lit, s.n); }

static int span_has(span_t s, const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= s.n; i++)
        if (!memcmp(s.p + i, needle, k))
            return 1;
    return 0;
}

/* Next whitespace-separated token from *s, or n == 0 at the end. */
static span_t next_token(span_t *s) {
    while (s->n && is_space(*s->p)) {
        s->p++;
        s->n--;
    }
    span_t t = {s->p, 0};
    while (t.n < s->n && !is_space(s->p[t.n]))
        t.n++;
    s->p += t.n;
    s->n -= t.n;
    return t;
}

static int is_ip(span_t t) {
    char buf[64];
    if (t.n == 0 || t.n >= sizeof buf)
        return 0;
    memcpy(buf, t.p, t.n);
    buf[t.n] = '\0';
    char *zone = strchr(buf, '%');
    if (zone)
        *zone = '\0';
    unsigned char addr[16];
    return inet_pton(AF_INET, buf, addr) == 1 || inet_pton(AF_INET6, buf, addr) == 1;
}

static int is_local(span_t t) {
    for (size_t i = 0; i < sizeof LOCAL_NAMES / sizeof *LOCAL_NAMES; i++)
        if (span_is(t, LOCAL_NAMES[i]))
            return 1;
    return 0;
}

/* Lowercase, no trailing dot, letters/digits/-/_ in 1-63 byte labels, at
 * least two labels, at most 253 bytes. */
static int normalize(span_t d, char out[DNS_NAME_MAX]) {
    if (d.n && d.p[d.n - 1] == '.')
        d.n--;
    if (d.n == 0 || d.n > 253)
        return LP_INVALID;
    size_t label = 0;
    int dots = 0;
    for (size_t i = 0; i < d.n; i++) {
        char c = to_lower(d.p[i]);
        if (c == '.') {
            if (label == 0)
                return LP_INVALID;
            label = 0;
            dots++;
        } else if (is_alnum(c) || c == '-' || c == '_') {
            if (++label > 63)
                return LP_INVALID;
        } else {
            return LP_INVALID;
        }
        out[i] = c;
    }
    out[d.n] = '\0';
    /* ponytail: a single label would block a whole TLD; lists that want that
     * are rare, so it is treated as a typo. */
    return label && dots ? LP_DOMAIN : LP_INVALID;
}

static int parse_adblock(span_t s, char out[DNS_NAME_MAX]) {
    span_t d = {s.p + 2, 0};
    while (2 + d.n < s.n && s.p[2 + d.n] != '^')
        d.n++;
    if (2 + d.n == s.n)
        return LP_UNSUPPORTED; /* no '^': a path or pattern rule */
    size_t rest = s.n - 2 - d.n - 1;
    if (rest > 1 || (rest == 1 && s.p[s.n - 1] != '|'))
        return LP_UNSUPPORTED; /* modifiers such as $third-party */
    if (span_has(d, "*") || span_has(d, "/"))
        return LP_UNSUPPORTED;
    return normalize(d, out);
}

int list_parse_line(const char *line, char out[DNS_NAME_MAX]) {
    span_t s = {line, strlen(line)};
    while (s.n && is_space(*s.p)) {
        s.p++;
        s.n--;
    }
    while (s.n && is_space(s.p[s.n - 1]))
        s.n--;

    if (s.n == 0 || *s.p == '#' || *s.p == '!' || *s.p == '[')
        return LP_SKIP;
    if (span_has(s, "##") || span_has(s, "#@#") || span_has(s, "#?#") || span_has(s, "#$#") ||
        (s.n >= 2 && !memcmp(s.p, "@@", 2)) || *s.p == '/')
        return LP_UNSUPPORTED;
    if (s.n >= 2 && !memcmp(s.p, "||", 2))
        return parse_adblock(s, out);

    span_t first = next_token(&s);
    if (is_ip(first)) { /* hosts format: address then name */
        span_t name = next_token(&s);
        if (name.n == 0 || *name.p == '#' || is_local(name) || is_ip(name))
            return LP_SKIP;
        return normalize(name, out);
    }

    span_t extra = next_token(&s);
    if (extra.n && *extra.p != '#')
        return LP_INVALID;
    if (first.n > 2 && !memcmp(first.p, "*.", 2)) {
        first.p += 2;
        first.n -= 2;
    }
    return normalize(first, out);
}

int list_read_file(const char *path, void (*fn)(void *ud, const char *domain), void *ud,
                   list_stats_t *st) {
    /* Regular files only, never through a symlink: a FIFO or /dev/zero in
     * lists/ would hang or spin the loader. O_NONBLOCK makes a FIFO fail fast. */
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat sb;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size > LIST_FILE_MAX) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return -1;
    }

    char line[LIST_LINE_MAX + 2];
    char domain[DNS_NAME_MAX];
    while (fgets(line, sizeof line, f)) {
        st->lines++;
        size_t n = strlen(line);
        if (n == sizeof line - 1 && line[n - 1] != '\n' && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {
            }
            st->invalid++;
            continue;
        }
        switch (list_parse_line(line, domain)) {
        case LP_DOMAIN:
            st->domains++;
            fn(ud, domain);
            break;
        case LP_UNSUPPORTED:
            st->unsupported++;
            break;
        case LP_INVALID:
            st->invalid++;
            break;
        }
    }
    fclose(f);
    return 0;
}
