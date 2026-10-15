#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dns.h"

/* Query for "Ads.Example.COM" type A, RD set, ID 0x1234. */
static const uint8_t QUERY_A[] = {
    0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    3, 'A', 'd', 's', 7, 'E', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'C', 'O', 'M', 0,
    0x00, 0x01, 0x00, 0x01,
};

static void test_simple_question(void) {
    dns_query_t q;
    assert(dns_parse_query(QUERY_A, sizeof QUERY_A, &q) == DNS_OK);
    assert(q.id == 0x1234);
    assert(q.qtype == DNS_TYPE_A);
    assert(q.qclass == DNS_CLASS_IN);
    assert(strcmp(q.qname, "ads.example.com") == 0);
    assert(q.question_end == sizeof QUERY_A);
}

static void test_compressed_name_rejected(void) {
    /* The question is the first name in a query, so a real client never
     * compresses it. A pointer there can only aim forward, at bytes an answer
     * would not copy: reject it. */
    static const uint8_t pkt[] = {
        0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        3, 'w', 'w', 'w', 0xc0, 22, /* -> offset 22 */
        0x00, 0x01, 0x00, 0x01,
        7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0,
    };
    dns_query_t q;
    assert(dns_parse_query(pkt, sizeof pkt, &q) == DNS_FORMERR);
}

static void test_truncated_name(void) {
    dns_query_t q;
    assert(dns_parse_query(QUERY_A, 20, &q) == DNS_FORMERR);
}

static void test_missing_type(void) {
    dns_query_t q;
    assert(dns_parse_query(QUERY_A, sizeof QUERY_A - 2, &q) == DNS_FORMERR);
}

static void test_compression_loop(void) {
    static const uint8_t pkt[] = {
        0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xc0, 12, /* points at itself */
        0x00, 0x01, 0x00, 0x01,
    };
    dns_query_t q;
    assert(dns_parse_query(pkt, sizeof pkt, &q) == DNS_FORMERR);
}

static void test_pointer_out_of_bounds(void) {
    static const uint8_t pkt[] = {
        0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xc0, 200, 0x00, 0x01, 0x00, 0x01,
    };
    dns_query_t q;
    assert(dns_parse_query(pkt, sizeof pkt, &q) == DNS_FORMERR);
}

static void test_name_too_long(void) {
    /* 5 labels of 63 bytes = 320 bytes of name, over the 255 limit. */
    uint8_t pkt[DNS_HEADER_LEN + 5 * 64 + 1 + 4] = {0x00, 0x01, 0x01, 0x00, 0x00, 0x01};
    size_t off = DNS_HEADER_LEN;
    for (int i = 0; i < 5; i++) {
        pkt[off++] = 63;
        memset(&pkt[off], 'a', 63);
        off += 63;
    }
    pkt[off++] = 0;
    pkt[off + 1] = 1;
    pkt[off + 3] = 1;
    dns_query_t q;
    assert(dns_parse_query(pkt, sizeof pkt, &q) == DNS_FORMERR);
}

static void test_bad_label_chars(void) {
    static const uint8_t pkt[] = {
        0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        3, 'a', '.', 'b', 3, 'c', 'o', 'm', 0, 0x00, 0x01, 0x00, 0x01,
    };
    dns_query_t q;
    assert(dns_parse_query(pkt, sizeof pkt, &q) == DNS_FORMERR);
}

static void test_short_packet(void) {
    dns_query_t q;
    assert(dns_parse_query(QUERY_A, 11, &q) == DNS_DROP);
    assert(dns_parse_query(QUERY_A, 0, &q) == DNS_DROP);
}

static void test_response_dropped(void) {
    uint8_t pkt[sizeof QUERY_A];
    memcpy(pkt, QUERY_A, sizeof pkt);
    pkt[2] |= 0x80; /* QR */
    dns_query_t q;
    assert(dns_parse_query(pkt, sizeof pkt, &q) == DNS_DROP);
}

static void test_qdcount(void) {
    uint8_t pkt[sizeof QUERY_A];
    dns_query_t q;
    memcpy(pkt, QUERY_A, sizeof pkt);
    pkt[5] = 0;
    assert(dns_parse_query(pkt, sizeof pkt, &q) == DNS_FORMERR);
    pkt[5] = 2;
    assert(dns_parse_query(pkt, sizeof pkt, &q) == DNS_FORMERR);
}

static void test_opcode(void) {
    uint8_t pkt[sizeof QUERY_A];
    memcpy(pkt, QUERY_A, sizeof pkt);
    pkt[2] = 0x28; /* opcode 5 (UPDATE) */
    dns_query_t q;
    assert(dns_parse_query(pkt, sizeof pkt, &q) == DNS_NOTIMP);
}

static void test_root_name(void) {
    static const uint8_t pkt[] = {
        0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0, 0x00, 0x02, 0x00, 0x01,
    };
    dns_query_t q;
    assert(dns_parse_query(pkt, sizeof pkt, &q) == DNS_OK);
    assert(q.qname[0] == '\0');
    assert(q.qtype == 2);
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void check_header(const uint8_t *out, uint16_t rcode, uint16_t ancount) {
    assert(u16(out) == 0x1234);
    assert(out[2] & 0x80);         /* QR */
    assert(out[2] & 0x01);         /* RD copied */
    assert(out[3] & 0x80);         /* RA */
    assert((out[3] & 0x0f) == rcode);
    assert(u16(out + 4) == 1);
    assert(u16(out + 6) == ancount);
    assert(u16(out + 8) == 0 && u16(out + 10) == 0);
    assert(memcmp(out + 12, QUERY_A + 12, sizeof QUERY_A - 12) == 0); /* question */
}

static void test_build_a(void) {
    dns_query_t q;
    uint8_t out[512];
    const uint8_t ip[4] = {0, 0, 0, 0};
    assert(dns_parse_query(QUERY_A, sizeof QUERY_A, &q) == DNS_OK);
    size_t n = dns_answer(QUERY_A, &q, DNS_RCODE_NOERROR, DNS_TYPE_A, ip, 4, 60, out, sizeof out);
    assert(n == sizeof QUERY_A + 16);
    check_header(out, DNS_RCODE_NOERROR, 1);
    const uint8_t *rr = out + sizeof QUERY_A;
    assert(u16(rr) == 0xc00c);
    assert(u16(rr + 2) == DNS_TYPE_A && u16(rr + 4) == DNS_CLASS_IN);
    assert(u16(rr + 6) == 0 && u16(rr + 8) == 60); /* TTL */
    assert(u16(rr + 10) == 4);
    assert(memcmp(rr + 12, ip, 4) == 0);
}

static void test_build_aaaa(void) {
    dns_query_t q;
    uint8_t out[512];
    const uint8_t ip6[16] = {0};
    assert(dns_parse_query(QUERY_A, sizeof QUERY_A, &q) == DNS_OK);
    size_t n = dns_answer(QUERY_A, &q, DNS_RCODE_NOERROR, DNS_TYPE_AAAA, ip6, 16, 60, out, sizeof out);
    assert(n == sizeof QUERY_A + 28);
    check_header(out, DNS_RCODE_NOERROR, 1);
    assert(u16(out + sizeof QUERY_A + 2) == DNS_TYPE_AAAA);
    assert(u16(out + sizeof QUERY_A + 10) == 16);
}

static void test_build_nodata_and_servfail(void) {
    dns_query_t q;
    uint8_t out[512];
    assert(dns_parse_query(QUERY_A, sizeof QUERY_A, &q) == DNS_OK);
    assert(dns_answer(QUERY_A, &q, DNS_RCODE_NOERROR, 0, NULL, 0, 0, out, sizeof out) == sizeof QUERY_A);
    check_header(out, DNS_RCODE_NOERROR, 0);
    assert(dns_answer(QUERY_A, &q, DNS_RCODE_SERVFAIL, 0, NULL, 0, 0, out, sizeof out) == sizeof QUERY_A);
    check_header(out, DNS_RCODE_SERVFAIL, 0);
}

static void test_build_too_small(void) {
    dns_query_t q;
    uint8_t out[sizeof QUERY_A + 15];
    const uint8_t ip[4] = {0};
    assert(dns_parse_query(QUERY_A, sizeof QUERY_A, &q) == DNS_OK);
    assert(dns_answer(QUERY_A, &q, 0, DNS_TYPE_A, ip, 4, 60, out, sizeof out) == 0);
    assert(dns_error(QUERY_A, DNS_RCODE_FORMERR, out, 11) == 0);
}

static void test_build_formerr(void) {
    uint8_t out[64];
    assert(dns_error(QUERY_A, DNS_RCODE_FORMERR, out, sizeof out) == DNS_HEADER_LEN);
    assert(u16(out) == 0x1234);
    assert(out[2] & 0x80 && out[2] & 0x01 && out[3] & 0x80);
    assert((out[3] & 0x0f) == DNS_RCODE_FORMERR);
    assert(u16(out + 4) == 0 && u16(out + 6) == 0);
}

int main(void) {
    test_simple_question();
    test_compressed_name_rejected();
    test_truncated_name();
    test_missing_type();
    test_compression_loop();
    test_pointer_out_of_bounds();
    test_name_too_long();
    test_bad_label_chars();
    test_short_packet();
    test_response_dropped();
    test_qdcount();
    test_opcode();
    test_root_name();
    test_build_a();
    test_build_aaaa();
    test_build_nodata_and_servfail();
    test_build_too_small();
    test_build_formerr();
    puts("test_dns: ok");
    return 0;
}
