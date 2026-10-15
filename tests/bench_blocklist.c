/* Loads 1M synthetic domains (plus the real fixtures when present) and reports
 * load time, peak memory and lookup speed. Run with `make bench`. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "blocklist.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double peak_mb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return (double)ru.ru_maxrss / (1024.0 * 1024.0); /* bytes */
#else
    return (double)ru.ru_maxrss / 1024.0; /* KiB */
#endif
}

int main(void) {
    char dir[] = "/tmp/adblock-bench-XXXXXX";
    char path[512];
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/lists", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/lists/synthetic.txt", dir);
    FILE *f = fopen(path, "w");
    for (int i = 0; i < 1000000; i++)
        fprintf(f, "0.0.0.0 d%07d.ads-%d.example.com\n", i, i % 977);
    fclose(f);

    double before = peak_mb();
    double t0 = now_s();
    blocklist_t *b = blocklist_load(dir, 1500000);
    double load = now_s() - t0;
    assert(b && b->blocked.count == 1000000);
    printf("1M domains: load %.3f s, peak RSS %.1f MB (%.1f MB before load), set %.1f MB\n", load,
           peak_mb(), before, (double)b->blocked.count * 8 / (1024 * 1024));

    t0 = now_s();
    int hits = 0;
    char name[64];
    for (int i = 0; i < 1000000; i++) {
        snprintf(name, sizeof name, "x.d%07d.ads-%d.example.com", i, i % 977);
        hits += set_contains_suffix(&b->blocked, name);
    }
    double lookups = now_s() - t0;
    assert(hits == 1000000);
    printf("1M suffix lookups: %.3f s (%.2f us each)\n", lookups, lookups);
    blocklist_free(b);
    unlink(path);
    snprintf(path, sizeof path, "%s/lists", dir);
    rmdir(path);
    rmdir(dir);
    return 0;
}
