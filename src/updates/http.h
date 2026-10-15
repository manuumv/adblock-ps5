/* SPDX-License-Identifier: GPL-3.0-or-later
 * HTTPS downloads for blocklists: a thin libcurl wrapper with verified TLS,
 * size and time limits, conditional requests, and every host it visits
 * (redirects included) resolved by the caller. */
#ifndef ADBLOCK_HTTP_H
#define ADBLOCK_HTTP_H

#include <stdatomic.h>
#include <stddef.h>

#define HTTP_MAX_REDIRECTS 5

typedef struct {
    const char *url;           /* https:// only */
    const char *dest;          /* written only while downloading; removed on failure */
    const char *etag;          /* If-None-Match, or NULL */
    const char *last_modified; /* If-Modified-Since, or NULL */
    /* Resolves each host to the IPv4 text to connect to, 0 on success; NULL
     * uses the system resolver. */
    int (*resolve)(void *ctx, const char *host, char *ip, size_t n);
    void *resolve_ctx;
    long max_bytes;
    long timeout_s;            /* for the whole download, redirects included */
    const char *ca_file;       /* CA bundle path; NULL uses the embedded bundle or the system's */
    atomic_int *cancel;        /* aborts the transfer when set, or NULL */
} http_get_t;

typedef struct {
    long status;           /* HTTP status, 0 on a transport error */
    int not_modified;      /* 304: dest was not written */
    int clock_error;       /* certificate not yet valid or expired: the console clock is likely wrong */
    long bytes;
    char etag[128];
    char last_modified[64];
    char error[192];
} http_result_t;

/* Call once from the main thread before any download (libcurl's global init
 * is not thread-safe). */
void http_init(void);

/* Downloads req->url to req->dest, following up to HTTP_MAX_REDIRECTS https
 * redirects. 0 on a 200 (dest written) or a 304 (not_modified set, dest
 * untouched); -1 otherwise, with result->error set and dest removed. */
int http_download(const http_get_t *req, http_result_t *result);

#endif
