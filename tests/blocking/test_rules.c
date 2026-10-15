#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "blocking/rules.h"

static void test_literals_and_star(void) {
    assert(mask_match("example.com", "example.com"));
    assert(!mask_match("example.com", "example.org"));
    assert(mask_match("*.example.com", "ads.example.com"));
    assert(mask_match("*.example.com", "a.b.example.com"));
    assert(!mask_match("*.example.com", "example.com"));
    assert(mask_match("*", ""));
    assert(mask_match("***", "anything"));
    assert(mask_match("*.playstation.com.*", "x.playstation.com.edgekey.net"));
    assert(!mask_match("", "a"));
    assert(mask_match("", ""));
}

static void test_question_mark(void) {
    assert(mask_match("api??.test.local", "api01.test.local"));
    assert(!mask_match("api??.test.local", "api1.test.local"));
    assert(!mask_match("?", ""));
}

static void test_classes(void) {
    assert(mask_match("[abc]x", "bx"));
    assert(!mask_match("[abc]x", "dx"));
    assert(mask_match("host[0-9].lan", "host7.lan"));
    assert(!mask_match("host[0-9].lan", "hosta.lan"));
    assert(mask_match("host[!0-9].lan", "hosta.lan"));
    assert(!mask_match("host[!0-9].lan", "host5.lan"));
    assert(mask_match("[a-c-]", "-"));
    assert(mask_match("a[", "a["));      /* unterminated: literal */
    assert(!mask_match("a[", "ab"));
}

static void test_case_comes_lowercase(void) {
    /* Names and masks are normalized before matching: lowercase only. */
    assert(mask_match("*.example.com", "ads.example.com"));
}

static void test_pathological(void) {
    char name[201];
    memset(name, 'a', 200);
    name[200] = '\0';
    clock_t start = clock();
    assert(!mask_match("*a*a*a*a*a*a*a*a*b", name));
    double ms = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
    assert(ms < 1.0);
}

static void test_rules_find(void) {
    static const rule_t rules[] = {
        {"*.ads.example", {0, 0, 0, 0}},
        {"nas.home", {192, 168, 1, 10}},
    };
    const rule_t *r = rules_find(rules, 2, "nas.home");
    assert(r && r->ip[0] == 192 && r->ip[3] == 10);
    assert(rules_find(rules, 2, "x.ads.example") == &rules[0]);
    assert(rules_find(rules, 2, "other.home") == NULL);
    assert(rules_find(rules, 0, "nas.home") == NULL);
}

static void test_masks_any(void) {
    static const char masks[][DNS_NAME_MAX] = {"*.stun.playstation.net", "feature.api.playstation.com"};
    assert(masks_match_any(masks, 2, "a.stun.playstation.net"));
    assert(masks_match_any(masks, 2, "feature.api.playstation.com"));
    assert(!masks_match_any(masks, 2, "store.playstation.com"));
}

int main(void) {
    test_literals_and_star();
    test_question_mark();
    test_classes();
    test_case_comes_lowercase();
    test_pathological();
    test_rules_find();
    test_masks_any();
    puts("test_rules: ok");
    return 0;
}
