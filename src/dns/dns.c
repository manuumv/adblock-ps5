/* SPDX-License-Identifier: GPL-3.0-or-later
 * Name reading adapted from nanoDNS (c) Drakmor, GPLv3. */
#include "dns/dns.h"

#include <string.h>

#define MAX_WIRE_NAME 255

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v) {
    wr16(p, (uint16_t)(v >> 16));
    wr16(p + 2, (uint16_t)v);
}

static int label_char_ok(uint8_t c) { return c != '.' && c != '\0'; }

/* Reads the question name at *off into out (lowercase, dot separated, no
 * trailing dot) and moves *off past it. The question is the first name in a
 * query, so it is never compressed: any pointer is rejected, which also rules
 * out pointer loops. -1 on any malformed or oversized name. */
static int read_name(const uint8_t *pkt, size_t len, size_t *off, char *out) {
    size_t cur = *off;
    size_t text = 0;
    size_t wire = 1; /* the terminating zero */

    while (cur < len) {
        uint8_t l = pkt[cur];
        if (l & 0xc0)
            return -1; /* compression pointer or reserved label type */

        cur++;
        if (l == 0) {
            out[text] = '\0';
            *off = cur;
            return 0;
        }
        wire += (size_t)l + 1;
        if (wire > MAX_WIRE_NAME || cur + l > len)
            return -1;
        if (text)
            out[text++] = '.';
        for (uint8_t i = 0; i < l; i++) {
            uint8_t c = pkt[cur + i];
            if (!label_char_ok(c))
                return -1;
            out[text++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        cur += l;
    }
    return -1;
}

int dns_parse_query(const uint8_t *pkt, size_t len, dns_query_t *q) {
    if (len < DNS_HEADER_LEN)
        return DNS_DROP;

    memset(q, 0, sizeof *q);
    q->id = rd16(pkt);
    q->flags = rd16(pkt + 2);
    if (q->flags & 0x8000)
        return DNS_DROP; /* a response: never answer it, avoids loops */
    if ((q->flags >> 11 & 0x0f) != 0)
        return DNS_NOTIMP;
    if (rd16(pkt + 4) != 1)
        return DNS_FORMERR;

    size_t off = DNS_HEADER_LEN;
    if (read_name(pkt, len, &off, q->qname) != 0 || off + 4 > len)
        return DNS_FORMERR;
    q->qtype = rd16(pkt + off);
    q->qclass = rd16(pkt + off + 2);
    q->question_end = off + 4;
    return DNS_OK;
}

/* Response flags: QR, the request's opcode and RD, RA, and rcode. */
static uint16_t response_flags(uint16_t req_flags, uint16_t rcode) {
    return (uint16_t)(0x8000 | (req_flags & 0x7900) | 0x0080 | (rcode & 0x0f));
}

size_t dns_answer(const uint8_t *req, const dns_query_t *q, uint16_t rcode,
                  uint16_t rtype, const void *rdata, uint16_t rdlen, uint32_t ttl,
                  uint8_t *out, size_t cap) {
    size_t n = q->question_end + (rdata ? 12u + rdlen : 0u);
    if (n > cap)
        return 0;

    memcpy(out, req, q->question_end);
    wr16(out + 2, response_flags(q->flags, rcode));
    wr16(out + 4, 1);
    wr16(out + 6, rdata ? 1 : 0);
    wr16(out + 8, 0);
    wr16(out + 10, 0);
    if (rdata) {
        uint8_t *rr = out + q->question_end;
        wr16(rr, 0xc00c); /* name: the question's, at offset 12 */
        wr16(rr + 2, rtype);
        wr16(rr + 4, DNS_CLASS_IN);
        wr32(rr + 6, ttl);
        wr16(rr + 10, rdlen);
        memcpy(rr + 12, rdata, rdlen);
    }
    return n;
}

size_t dns_error(const uint8_t *req, uint16_t rcode, uint8_t *out, size_t cap) {
    if (cap < DNS_HEADER_LEN)
        return 0;
    memset(out, 0, DNS_HEADER_LEN);
    memcpy(out, req, 2);
    wr16(out + 2, response_flags(rd16(req + 2), rcode));
    return DNS_HEADER_LEN;
}
