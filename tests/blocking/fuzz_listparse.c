/* libFuzzer harness for blocklist lines: lists will be downloaded from the internet. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "blocking/listparse.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char line[LIST_LINE_MAX + 2];
    char out[DNS_NAME_MAX];
    size_t n = size < sizeof line - 1 ? size : sizeof line - 1;
    memcpy(line, data, n);
    line[n] = '\0';
    if (list_parse_line(line, out) == LP_DOMAIN) {
        size_t len = strlen(out);
        if (len == 0 || len > 253 || strchr(out, ' ') || out[len - 1] == '.')
            __builtin_trap();
    }
    return 0;
}
