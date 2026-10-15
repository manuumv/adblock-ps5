/* SPDX-License-Identifier: GPL-3.0-or-later
 * DNS wire format: parsing queries and building answers. Every function treats
 * the packet as untrusted and never reads outside (buf, len). */
#ifndef ADBLOCK_DNS_H
#define ADBLOCK_DNS_H

#include <stddef.h>
#include <stdint.h>

#define DNS_HEADER_LEN 12
#define DNS_MAX_PACKET 4096
#define DNS_NAME_MAX 256 /* buffer for a text name: 253 chars + NUL, rounded */

#define DNS_TYPE_A 1
#define DNS_TYPE_AAAA 28
#define DNS_TYPE_ANY 255
#define DNS_CLASS_IN 1

#define DNS_RCODE_NOERROR 0
#define DNS_RCODE_FORMERR 1
#define DNS_RCODE_SERVFAIL 2
#define DNS_RCODE_NXDOMAIN 3
#define DNS_RCODE_NOTIMP 4

/* dns_parse_query results. */
enum {
    DNS_OK = 0,
    DNS_DROP = -1,    /* not worth an answer: shorter than a header, or a response */
    DNS_FORMERR = -2, /* answer with dns_error(DNS_RCODE_FORMERR) */
    DNS_NOTIMP = -3,  /* opcode other than QUERY: dns_error(DNS_RCODE_NOTIMP) */
};

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qtype;
    uint16_t qclass;
    size_t question_end; /* offset just past the question */
    char qname[DNS_NAME_MAX]; /* lowercase, no trailing dot; "" for the root */
} dns_query_t;

/* Parses the header and the single question of a query. */
int dns_parse_query(const uint8_t *pkt, size_t len, dns_query_t *q);

/* Answer to a parsed query: header and question copied from req, rcode set,
 * and one record of type rtype (name pointing at the question) when rdata is
 * not NULL. Returns the length written to out, 0 if it does not fit. */
size_t dns_answer(const uint8_t *req, const dns_query_t *q, uint16_t rcode,
                  uint16_t rtype, const void *rdata, uint16_t rdlen, uint32_t ttl,
                  uint8_t *out, size_t cap);

/* A recursive query (RD set) for name and qtype, for resolving on our own
 * behalf. Returns its length, 0 if name is invalid or cap too small. */
size_t dns_build_query(uint16_t id, const char *name, uint16_t qtype, uint8_t *out, size_t cap);

/* Copies the first A record of a NOERROR response into ip. 0 on success, -1
 * if there is none or the packet is malformed. Compressed names are skipped,
 * never followed. */
int dns_first_a(const uint8_t *resp, size_t len, uint8_t ip[4]);

/* Header-only answer (no question) for a query that could not be parsed.
 * req must hold at least DNS_HEADER_LEN bytes. Returns the length, 0 if cap is
 * too small. */
size_t dns_error(const uint8_t *req, uint16_t rcode, uint8_t *out, size_t cap);

#endif
