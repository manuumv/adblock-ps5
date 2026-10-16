#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dns/server.h"

static config_t cfg;

static domain_set_t make(const char *const *names, size_t n) {
    uint64_t *h = malloc(n * sizeof *h + 1);
    for (size_t i = 0; i < n; i++)
        h[i] = domain_hash(names[i], strlen(names[i]));
    domain_set_t s;
    set_build(&s, h, n);
    return s;
}

int main(void) {
    static const char *blocked[] = {"nas.home", "ads.example", "keep.example", "doubleclick.net"};
    static const char *allowed[] = {"cdn.ads.example"};
    blocklist_t b = {.blocked = make(blocked, 4), .allowed = make(allowed, 1)};

    snprintf(cfg.exceptions[0], DNS_NAME_MAX, "keep.example");
    cfg.exception_count = 1;
    snprintf(cfg.rules[0].mask, DNS_NAME_MAX, "nas.home");
    cfg.rules[0].ip[0] = 192;
    cfg.rule_count = 1;
    snprintf(cfg.block_masks[0], DNS_NAME_MAX, "*.masked.example");
    cfg.block_mask_count = 1;

    const rule_t *r = NULL;
    assert(server_decide(&cfg, &b, "keep.example", &r) == VERDICT_ALLOWLISTED);   /* exception > list */
    assert(server_decide(&cfg, &b, "cdn.ads.example", &r) == VERDICT_ALLOWLISTED); /* allowlist > list */
    assert(server_decide(&cfg, &b, "x.cdn.ads.example", &r) == VERDICT_ALLOWLISTED);
    assert(server_decide(&cfg, &b, "nas.home", &r) == VERDICT_OVERRIDE && r == &cfg.rules[0]);
    assert(server_decide(&cfg, &b, "ads.example", &r) == VERDICT_BLOCKED);
    assert(server_decide(&cfg, &b, "ad.g.doubleclick.net", &r) == VERDICT_BLOCKED);
    assert(server_decide(&cfg, &b, "a.masked.example", &r) == VERDICT_BLOCKED);  /* preset mask */
    assert(server_decide(&cfg, &b, "example.org", &r) == VERDICT_FORWARD);
    assert(server_decide(&cfg, NULL, "ads.example", &r) == VERDICT_FORWARD);    /* lists not loaded yet */
    assert(server_decide(&cfg, NULL, "a.masked.example", &r) == VERDICT_BLOCKED);

    set_free(&b.blocked);
    set_free(&b.allowed);

    /* Firmware updates are blocked by default, loaded through config.ini as a user would. */
    static config_t pc;
    char path[] = "/tmp/adblock-test-preset-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    fputs("[general]\ndebug=0\n", f);
    fclose(f);
    assert(config_load(path, &pc) == 0 && pc.warning_count == 0);
    assert(server_decide(&pc, NULL, "psndl.net", &r) == VERDICT_BLOCKED);
    assert(server_decide(&pc, NULL, "gs2.ww.prod.dl.playstation.net", &r) == VERDICT_BLOCKED);
    assert(server_decide(&pc, NULL, "x.playstation.com.edgekey.net", &r) == VERDICT_BLOCKED);
    assert(server_decide(&pc, NULL, "stun.us.playstation.net", &r) == VERDICT_ALLOWLISTED);
    assert(server_decide(&pc, NULL, "feature.api.playstation.com", &r) == VERDICT_ALLOWLISTED);
    assert(server_decide(&pc, NULL, "example.com", &r) == VERDICT_FORWARD);

    /* block_updates=0: PSN and updates reachable again. */
    f = fopen(path, "w");
    fputs("[blocking]\nblock_updates=0\n", f);
    fclose(f);
    assert(config_load(path, &pc) == 0 && pc.warning_count == 0);
    unlink(path);
    assert(server_decide(&pc, NULL, "psndl.net", &r) == VERDICT_FORWARD);
    assert(server_decide(&pc, NULL, "gs2.ww.prod.dl.playstation.net", &r) == VERDICT_FORWARD);

    puts("test_decide: ok");
    return 0;
}
