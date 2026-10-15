#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blocklist.h"

static domain_set_t make(const char *const *names, size_t n) {
    uint64_t *h = malloc(n * sizeof *h + 1);
    for (size_t i = 0; i < n; i++)
        h[i] = domain_hash(names[i], strlen(names[i]));
    domain_set_t s;
    set_build(&s, h, n);
    return s;
}

static void test_exact_and_missing(void) {
    static const char *names[] = {"ads.example.com", "tracker.net"};
    domain_set_t s = make(names, 2);
    assert(set_contains_suffix(&s, "ads.example.com"));
    assert(set_contains_suffix(&s, "tracker.net"));
    assert(!set_contains_suffix(&s, "example.com"));
    assert(!set_contains_suffix(&s, "other.org"));
    assert(!set_contains_suffix(&s, ""));
    set_free(&s);
}

static void test_suffix(void) {
    static const char *names[] = {"doubleclick.net", "ad.g.example.org"};
    domain_set_t s = make(names, 2);
    assert(set_contains_suffix(&s, "doubleclick.net"));
    assert(set_contains_suffix(&s, "ad.g.doubleclick.net"));
    assert(!set_contains_suffix(&s, "notdoubleclick.net")); /* label boundary */
    assert(set_contains_suffix(&s, "x.ad.g.example.org"));
    assert(!set_contains_suffix(&s, "g.example.org"));      /* parent not blocked */
    assert(!set_contains_suffix(&s, "example.org"));
    set_free(&s);
}

static void test_duplicates(void) {
    static const char *names[] = {"a.com", "b.com", "a.com", "a.com", "c.com", "b.com"};
    domain_set_t s = make(names, 6);
    assert(s.count == 3);
    for (size_t i = 1; i < s.count; i++)
        assert(s.hashes[i - 1] < s.hashes[i]);
    set_free(&s);
}

static void test_empty(void) {
    domain_set_t s = make(NULL, 0);
    assert(s.count == 0);
    assert(!set_contains_suffix(&s, "a.com"));
    set_free(&s);
    domain_set_t z = {0};
    assert(!set_contains_suffix(&z, "a.com"));
}

static void test_hash_is_keyed_siphash(void) {
    /* SipHash-2-4 reference vector: key 00..0f, message 00..0e. */
    uint8_t key[16], msg[15];
    for (int i = 0; i < 16; i++)
        key[i] = (uint8_t)i;
    for (int i = 0; i < 15; i++)
        msg[i] = (uint8_t)i;
    domain_hash_set_key(key);
    assert(domain_hash((const char *)msg, 15) == 0xa129ca6149be45e5ULL);

    /* A different key gives a different hash: precomputed collisions fail. */
    uint64_t h1 = domain_hash("example.com", 11);
    key[0] ^= 1;
    domain_hash_set_key(key);
    assert(domain_hash("example.com", 11) != h1);
}

int main(void) {
    test_exact_and_missing();
    test_suffix();
    test_duplicates();
    test_empty();
    test_hash_is_keyed_siphash();
    puts("test_blocklist: ok");
    return 0;
}
