/* SPDX-License-Identifier: GPL-3.0-or-later
 * Verified HTTPS downloads over libcurl. */
#include "updates/http.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#ifdef ADBLOCK_CA_BUNDLE
#include "ca_bundle.h" /* generated: static const char CA_BUNDLE[] */
#endif

#define USER_AGENT "ps5-adblock/" ADBLOCK_VERSION
#define URL_MAX 2048
#define CONNECT_TIMEOUT_MS 15000L
#define X509_V_ERR_CERT_NOT_YET_VALID 9
#define X509_V_ERR_CERT_HAS_EXPIRED 10

void http_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Splits https://host[:port]/... into host and port. The host is a plain DNS
 * name: no userinfo, no IP literals, nothing that could change the meaning of
 * the CURLOPT_RESOLVE entry built from it. */
static int parse_url(const char *url, char *host, size_t n, long *port) {
    if (strncmp(url, "https://", 8) != 0)
        return -1;
    const char *a = url + 8;
    size_t len = strcspn(a, "/?#");
    const char *colon = memchr(a, ':', len);
    size_t hlen = colon ? (size_t)(colon - a) : len;
    if (hlen == 0 || hlen >= n || a[0] == '-' || a[0] == '.' ||
        strspn(a, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") < hlen)
        return -1;
    memcpy(host, a, hlen);
    host[hlen] = '\0';
    *port = 443;
    if (colon) {
        char *end;
        *port = strtol(colon + 1, &end, 10);
        if (end != a + len || *port <= 0 || *port > 65535)
            return -1;
    }
    return 0;
}

typedef struct {
    FILE *out;
    long bytes, max_bytes;
    int too_big;
    atomic_int *cancel;
    http_result_t *result;
} transfer_t;

static size_t on_body(char *data, size_t size, size_t nmemb, void *ud) {
    transfer_t *t = ud;
    size_t n = size * nmemb;
    if (t->bytes + (long)n > t->max_bytes) {
        t->too_big = 1;
        return 0; /* aborts the transfer */
    }
    t->bytes += (long)n;
    return fwrite(data, 1, n, t->out);
}

/* Copies a header's value, without the line ending, when its name matches.
 * Values with control characters are dropped: they are stored in the meta
 * file and sent back in later requests. */
static void header_value(const char *line, size_t n, const char *name, char *out, size_t cap) {
    size_t k = strlen(name);
    if (n <= k + 1 || strncasecmp(line, name, k) != 0 || line[k] != ':')
        return;
    const char *v = line + k + 1;
    size_t len = n - k - 1;
    while (len && (*v == ' ' || *v == '\t')) {
        v++;
        len--;
    }
    while (len && (v[len - 1] == '\r' || v[len - 1] == '\n' || v[len - 1] == ' '))
        len--;
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)v[i] < 0x20 || v[i] == 0x7f)
            return;
    if (len < cap) {
        memcpy(out, v, len);
        out[len] = '\0';
    }
}

static size_t on_header(char *line, size_t size, size_t nmemb, void *ud) {
    transfer_t *t = ud;
    size_t n = size * nmemb;
    if (n > 5 && !strncmp(line, "HTTP/", 5)) { /* a new response: forget the previous one */
        t->result->etag[0] = '\0';
        t->result->last_modified[0] = '\0';
    }
    header_value(line, n, "ETag", t->result->etag, sizeof t->result->etag);
    header_value(line, n, "Last-Modified", t->result->last_modified, sizeof t->result->last_modified);
    return n;
}

static int on_progress(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)dt, (void)dn, (void)ut, (void)un;
    transfer_t *t = ud;
    return t->cancel && atomic_load(t->cancel);
}

/* Sets every option for one request. Nonzero if libcurl rejected one that
 * security depends on (an older runtime libcurl, a bad CA bundle). */
static int configure(CURL *curl, const http_get_t *req, const char *url, transfer_t *t, long timeout_ms,
                     struct curl_slist *pin, struct curl_slist *headers, char *errbuf) {
    int bad = 0;
    bad |= curl_easy_setopt(curl, CURLOPT_URL, url);
    bad |= curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    bad |= curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    bad |= curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (req->ca_file) {
        bad |= curl_easy_setopt(curl, CURLOPT_CAINFO, req->ca_file);
    } else {
#ifdef ADBLOCK_CA_BUNDLE
        struct curl_blob blob = {(void *)CA_BUNDLE, sizeof CA_BUNDLE - 1, CURL_BLOB_NOCOPY};
        bad |= curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
#endif
    }
    if (pin)
        bad |= curl_easy_setopt(curl, CURLOPT_RESOLVE, pin);
    /* Redirects are followed by http_download, so every hop gets pinned. */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); /* runs on a worker thread */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)req->max_bytes);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); /* gzip if offered; limits apply to decoded bytes */
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, t);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, t);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    return bad;
}

static struct curl_slist *conditional_headers(const http_get_t *req) {
    struct curl_slist *headers = NULL;
    char line[256];
    if (req->etag && *req->etag) {
        snprintf(line, sizeof line, "If-None-Match: %s", req->etag);
        headers = curl_slist_append(headers, line);
    }
    if (req->last_modified && *req->last_modified) {
        snprintf(line, sizeof line, "If-Modified-Since: %s", req->last_modified);
        headers = curl_slist_append(headers, line);
    }
    return headers;
}

static void transport_error(http_result_t *r, CURL *curl, CURLcode rc, const transfer_t *t, const char *errbuf) {
    long verify = 0;
    curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &verify);
    if (rc == CURLE_PEER_FAILED_VERIFICATION &&
        (verify == X509_V_ERR_CERT_NOT_YET_VALID || verify == X509_V_ERR_CERT_HAS_EXPIRED ||
         strstr(errbuf, "expired") || strstr(errbuf, "not yet valid")))
        r->clock_error = 1;
    if (t->too_big || rc == CURLE_FILESIZE_EXCEEDED)
        snprintf(r->error, sizeof r->error, "list exceeds the size limit");
    else
        snprintf(r->error, sizeof r->error, "%s", errbuf[0] ? errbuf : curl_easy_strerror(rc));
}

/* One request to url, redirects not followed. 0 with r->status set and, for a
 * redirect, its absolute target in next; -1 with r->error set. */
static int fetch(const http_get_t *req, const char *url, transfer_t *t, long timeout_ms, char *next) {
    http_result_t *r = t->result;
    char host[256], ip[64] = "", line[512];
    long port;
    if (parse_url(url, host, sizeof host, &port) != 0) {
        snprintf(r->error, sizeof r->error, "only https:// URLs with a plain host name are accepted");
        return -1;
    }
    if (req->resolve && req->resolve(req->resolve_ctx, host, ip, sizeof ip) != 0) {
        snprintf(r->error, sizeof r->error, "cannot resolve %s", host);
        return -1;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(r->error, sizeof r->error, "curl_easy_init failed");
        return -1;
    }
    struct curl_slist *pin = NULL, *headers = conditional_headers(req);
    if (ip[0]) {
        snprintf(line, sizeof line, "%s:%ld:%s", host, port, ip);
        pin = curl_slist_append(NULL, line);
    }
    char errbuf[CURL_ERROR_SIZE] = "";
    int rc = -1;
    if (configure(curl, req, url, t, timeout_ms, pin, headers, errbuf) != 0) {
        snprintf(r->error, sizeof r->error, "libcurl rejected the TLS settings");
    } else {
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            transport_error(r, curl, res, t, errbuf);
        } else {
            char *target = NULL;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r->status);
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &target);
            next[0] = '\0';
            if (r->status / 100 == 3 && target)
                snprintf(next, URL_MAX, "%s", target);
            rc = 0;
        }
    }
    curl_slist_free_all(headers);
    curl_slist_free_all(pin);
    curl_easy_cleanup(curl);
    return rc;
}

/* Never follows a symlink planted at dest. */
static FILE *create(const char *path) {
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, "wb");
    if (!f)
        close(fd);
    return f;
}

static int fail(const http_get_t *req, FILE *out) {
    if (out)
        fclose(out);
    unlink(req->dest);
    return -1;
}

/* Follows redirects itself, with the remaining time budget, so each hop is
 * resolved through req->resolve. A redirect's own body is discarded. */
static int follow(const http_get_t *req, transfer_t *t) {
    http_result_t *r = t->result;
    char url[URL_MAX], next[URL_MAX];
    int64_t deadline = mono_ms() + req->timeout_s * 1000;
    snprintf(url, sizeof url, "%s", req->url);
    for (int hop = 0;; hop++) {
        int64_t left = deadline - mono_ms();
        if (left <= 0) {
            snprintf(r->error, sizeof r->error, "timed out");
            return -1;
        }
        if (fetch(req, url, t, (long)left, next) != 0)
            return -1;
        if (!next[0])
            return 0;
        if (hop == HTTP_MAX_REDIRECTS) {
            snprintf(r->error, sizeof r->error, "too many redirects");
            return -1;
        }
        memcpy(url, next, sizeof url);
        if (fflush(t->out) != 0 || ftruncate(fileno(t->out), 0) != 0) {
            snprintf(r->error, sizeof r->error, "cannot write %s", req->dest);
            return -1;
        }
        rewind(t->out);
        t->bytes = 0;
    }
}

int http_download(const http_get_t *req, http_result_t *r) {
    memset(r, 0, sizeof *r);
    if (strlen(req->url) >= URL_MAX || strncmp(req->url, "https://", 8) != 0) {
        snprintf(r->error, sizeof r->error, "only https:// URLs with a plain host name are accepted");
        return -1;
    }
    FILE *out = create(req->dest);
    if (!out) {
        snprintf(r->error, sizeof r->error, "cannot create %s", req->dest);
        return -1;
    }
    transfer_t t = {.out = out, .max_bytes = req->max_bytes, .cancel = req->cancel, .result = r};
    int rc = follow(req, &t);
    r->bytes = t.bytes;
    if (rc != 0)
        return fail(req, out);
    if (r->status == 304) {
        r->not_modified = 1;
        fail(req, out);
        return 0;
    }
    if (r->status != 200) {
        snprintf(r->error, sizeof r->error, "HTTP %ld", r->status);
        return fail(req, out);
    }
    int bad = fflush(out) != 0;
    bad |= fsync(fileno(out)) != 0;
    bad |= fclose(out) != 0; /* always runs: no descriptor leak on a full disk */
    if (bad) {
        snprintf(r->error, sizeof r->error, "cannot write %s", req->dest);
        return fail(req, NULL);
    }
    return 0;
}
