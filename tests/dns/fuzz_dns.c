/* libFuzzer harness for the DNS parser and answer builders. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dns/dns.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    dns_query_t q;
    uint8_t out[DNS_MAX_PACKET];
    static const uint8_t ip6[16];

    int rc = dns_parse_query(data, size, &q);
    if (rc == DNS_OK) {
        size_t n = strlen(q.qname);
        if (n > 253 || q.question_end > size)
            __builtin_trap();
        (void)dns_answer(data, &q, DNS_RCODE_NOERROR, DNS_TYPE_AAAA, ip6, 16, 60, out, sizeof out);
        (void)dns_answer(data, &q, DNS_RCODE_SERVFAIL, 0, NULL, 0, 0, out, sizeof out);
    } else if (rc != DNS_DROP) {
        (void)dns_error(data, DNS_RCODE_FORMERR, out, sizeof out);
    }
    return 0;
}
