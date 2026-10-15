#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "blocking/listparse.h"

static char out[DNS_NAME_MAX];

static int parse(const char *line) { return list_parse_line(line, out); }

static void expect_domain(const char *line, const char *domain) {
    int rc = parse(line);
    if (rc != LP_DOMAIN || strcmp(out, domain) != 0) {
        fprintf(stderr, "line '%s': rc=%d out='%s', want '%s'\n", line, rc, out, domain);
        assert(0);
    }
}

static void expect(const char *line, int want) {
    int rc = parse(line);
    if (rc != want) {
        fprintf(stderr, "line '%s': rc=%d, want %d\n", line, rc, want);
        assert(0);
    }
}

static void test_hosts(void) {
    expect_domain("0.0.0.0 ads.example.com", "ads.example.com");
    expect_domain("127.0.0.1\tTracker.Example.NET.", "tracker.example.net");
    expect_domain("0.0.0.0 ads.example.com # comment", "ads.example.com");
    expect_domain(":: ads6.example.com", "ads6.example.com");
    expect_domain("0.0.0.0 ads.example.com\r\n", "ads.example.com");
}

static void test_plain(void) {
    expect_domain("ads.example.com", "ads.example.com");
    expect_domain("  ads.example.com  \n", "ads.example.com");
    expect_domain("*.ads.example.com", "ads.example.com"); /* wildcard = suffix anyway */
    expect_domain("under_score.example.com", "under_score.example.com");
}

static void test_adblock(void) {
    expect_domain("||tracker.example.net^", "tracker.example.net");
    expect_domain("||Tracker.Example.net^\r\n", "tracker.example.net");
    expect("||example.org^$third-party", LP_UNSUPPORTED);
    expect("||example.org/ads/*", LP_UNSUPPORTED);
    expect("@@||example.org^", LP_UNSUPPORTED);
    expect("example.org##.banner", LP_UNSUPPORTED);
    expect("##.ad-slot", LP_SKIP); /* leading '#' is a hosts comment ("## Section") first */
    expect("example.org#@#.banner", LP_UNSUPPORTED);
    expect("/banner[0-9]+/", LP_UNSUPPORTED);
    expect("||ads*.example.org^", LP_UNSUPPORTED);
}

static void test_comments_and_blank(void) {
    expect("", LP_SKIP);
    expect("   \r\n", LP_SKIP);
    expect("# hosts file", LP_SKIP);
    expect("! Title: HaGeZi", LP_SKIP);
    expect("[Adblock Plus 2.0]", LP_SKIP);
}

static void test_local_entries(void) {
    expect("127.0.0.1 localhost", LP_SKIP);
    expect("127.0.0.1 localhost.localdomain", LP_SKIP);
    expect("255.255.255.255 broadcasthost", LP_SKIP);
    expect("::1 ip6-localhost", LP_SKIP);
    expect("0.0.0.0 0.0.0.0", LP_SKIP);
    expect("0.0.0.0", LP_SKIP);
    expect("fe80::1%lo0 localhost", LP_SKIP);
}

static void test_invalid(void) {
    expect("bad domain!", LP_INVALID);
    expect("exa mple", LP_INVALID); /* two plain tokens */
    expect("example", LP_INVALID);  /* single label: would block a whole TLD */
    expect(".example.com", LP_INVALID);
    expect("example..com", LP_INVALID);
    expect("-", LP_INVALID);
    char label[80] = "";
    memset(label, 'a', 64);
    strcat(label, ".com");
    expect(label, LP_INVALID); /* label > 63 */
    char longname[300] = "";
    for (int i = 0; i < 26; i++)
        strcat(longname, "abcdefghi.");
    strcat(longname, "com"); /* 263 chars */
    expect(longname, LP_INVALID);
}

static void test_readme_examples(void) {
    /* The format examples in the README "Blocking" section, verbatim. */
    expect_domain("0.0.0.0 ads.example.com", "ads.example.com");
    expect_domain("tracker.example.net", "tracker.example.net");
    expect_domain("||popups.example.org^", "popups.example.org");
}

static char tmp[] = "/tmp/adblock-test-list-XXXXXX";

static void collect(void *ud, const char *domain) {
    size_t *n = ud;
    (*n)++;
    assert(strchr(domain, ' ') == NULL);
}

static void test_read_file(void) {
    int fd = mkstemp(tmp);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    fputs("# header\n0.0.0.0 a.example.com\n||b.example.com^\r\nc.example.com\n", f);
    fputs("||x.example.com^$popup\nnot valid!\n", f);
    for (int i = 0; i < 600; i++)
        fputc('z', f); /* over LIST_LINE_MAX */
    fputs("\nd.example.com", f); /* no trailing newline */
    fclose(f);

    list_stats_t st = {0};
    size_t n = 0;
    assert(list_read_file(tmp, collect, &n, &st) == 0);
    assert(n == 4 && st.domains == 4);
    assert(st.unsupported == 1);
    assert(st.invalid == 2);
    assert(st.lines == 8);
    unlink(tmp);
    assert(list_read_file("/nonexistent/list.txt", collect, &n, &st) == -1);
}

static void test_special_files_rejected(void) {
    /* A symlink, a FIFO or a device in lists/ must not hang or loop the loader. */
    char dir[] = "/tmp/adblock-test-special-XXXXXX", path[512], target[512];
    assert(mkdtemp(dir));
    list_stats_t st = {0};
    size_t n = 0;

    snprintf(target, sizeof target, "%s/real.txt", dir);
    FILE *f = fopen(target, "w");
    fputs("a.example.com\n", f);
    fclose(f);
    snprintf(path, sizeof path, "%s/link.txt", dir);
    assert(symlink(target, path) == 0);
    assert(list_read_file(path, collect, &n, &st) == -1);
    unlink(path);

    assert(symlink("/dev/zero", path) == 0);
    assert(list_read_file(path, collect, &n, &st) == -1);
    unlink(path);

    snprintf(path, sizeof path, "%s/fifo.txt", dir);
    assert(mkfifo(path, 0600) == 0);
    assert(list_read_file(path, collect, &n, &st) == -1); /* returns at once, no writer needed */
    unlink(path);

    assert(list_read_file(target, collect, &n, &st) == 0 && n == 1);
    unlink(target);
    rmdir(dir);
}

/* Real lists in tests/fixtures (git-ignored, fetched by `make fixtures`):
 * every non-comment line should load, within 1%. */
static void test_fixture(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("  skip %s (run make fixtures)\n", path);
        return;
    }
    char line[4096];
    size_t raw = 0;
    while (fgets(line, sizeof line, f))
        if (line[0] != '#' && line[0] != '!' && line[0] != '\n' && line[0] != '\r')
            raw++;
    fclose(f);

    list_stats_t st = {0};
    size_t n = 0;
    assert(list_read_file(path, collect, &n, &st) == 0);
    double diff = raw > n ? (double)(raw - n) : (double)(n - raw);
    printf("  %s: %zu lines, %zu domains, %zu unsupported, %zu invalid\n", path, raw, n,
           st.unsupported, st.invalid);
    assert(diff <= (double)raw * 0.01);
}

int main(void) {
    test_hosts();
    test_plain();
    test_adblock();
    test_comments_and_blank();
    test_local_entries();
    test_invalid();
    test_read_file();
    test_readme_examples();
    test_special_files_rejected();
    test_fixture("tests/fixtures/stevenblack.txt");
    test_fixture("tests/fixtures/hagezi-pro.txt");
    test_fixture("tests/fixtures/hagezi-popupads.txt");
    puts("test_listparse: ok");
    return 0;
}
