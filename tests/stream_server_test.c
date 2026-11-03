/* Host test for client/stream_server.c. argv[1] is tests/data. */
#include "../client/stream_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define CHECK_STR(actual, expected) do { const char *a_ = (actual), *e_ = (expected); \
    if (strcmp(a_, e_)) { printf("FAIL %s:%d:\n  got  %s\n  want %s\n", __FILE__, __LINE__, a_, e_); ++failures; } } while (0)

static WiliServer server(void) {
    WiliServer s = {1, "192.168.1.20", 8787, "k7m2q9xa"};
    return s;
}

static void test_address(void) {
    char host[WILI_SERVER_HOST_MAX]; int port = 0;
    CHECK(wili_server_parse_address("192.168.1.20", host, sizeof(host), &port) == 0);
    CHECK_STR(host, "192.168.1.20"); CHECK(port == 8787);
    CHECK(wili_server_parse_address("  desk-pc.local:9000 ", host, sizeof(host), &port) == 0);
    CHECK_STR(host, "desk-pc.local"); CHECK(port == 9000);
    const char *bad[] = {"", "   ", ":80", "host:", "host:0", "host:65536", "host:80a", "ho st", "-host", "host.",
        "ho/st", "http://host", "host:1:2", "a@b", "[::1]:80", "host:999999"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        if (wili_server_parse_address(bad[i], host, sizeof(host), &port) == 0) {
            printf("FAIL accepted address \"%s\"\n", bad[i]); ++failures;
        }
    char tiny[4];
    CHECK(wili_server_parse_address("192.168.1.20", tiny, sizeof(tiny), &port) == -1);
}

static void test_token(void) {
    CHECK(wili_server_valid_token("k7m2q9xa"));
    CHECK(wili_server_valid_token("ab-_9"));
    CHECK(!wili_server_valid_token("abc"));
    CHECK(!wili_server_valid_token("has space"));
    CHECK(!wili_server_valid_token("a&b=cdef"));
    CHECK(!wili_server_valid_token(""));
    CHECK(!wili_server_valid_token("0123456789012345678901234567890123"));
    CHECK(!wili_server_valid_token(NULL));
}

static void test_rewrite(void) {
    WiliServer s = server();
    char out[512];
    CHECK(wili_server_rewrite(&s, "https://api.bilibili.com/x/web-interface/view?bvid=BV1xx411c7mD", out, sizeof(out)) == 1);
    CHECK_STR(out, "http://192.168.1.20:8787/p/api.bilibili.com/x/web-interface/view?bvid=BV1xx411c7mD&k=k7m2q9xa");
    CHECK(wili_server_rewrite(&s, "https://passport.bilibili.com/x/passport-login/web/qrcode/generate", out, sizeof(out)) == 1);
    CHECK_STR(out, "http://192.168.1.20:8787/p/passport.bilibili.com/x/passport-login/web/qrcode/generate?k=k7m2q9xa");
    CHECK(wili_server_rewrite(&s, "//api.bilibili.com/x/web-interface/nav", out, sizeof(out)) == 1);
    CHECK_STR(out, "http://192.168.1.20:8787/p/api.bilibili.com/x/web-interface/nav?k=k7m2q9xa");
    CHECK(wili_server_rewrite(&s, "https://i0.hdslb.com/bfs/archive/a.jpg@320w_200h.jpg", out, sizeof(out)) == 1);
    CHECK_STR(out, "http://192.168.1.20:8787/p/i0.hdslb.com/bfs/archive/a.jpg@320w_200h.jpg?k=k7m2q9xa");
    CHECK(wili_server_rewrite(&s, "https://api.bilibili.com", out, sizeof(out)) == 1);
    CHECK_STR(out, "http://192.168.1.20:8787/p/api.bilibili.com/?k=k7m2q9xa");
    CHECK(wili_server_rewrite(&s, "https://api.bilibili.com/x/y?a=1#frag", out, sizeof(out)) == 1);
    CHECK_STR(out, "http://192.168.1.20:8787/p/api.bilibili.com/x/y?a=1&k=k7m2q9xa");

    /* Not for the server: other schemes and hosts stay untouched. */
    strcpy(out, "untouched");
    const char *skip[] = {"http://api.bilibili.com/x", "https://evil.test/x", "https://api.bilibili.com.evil.test/x",
        "https://api.bilibili.com@evil.test/x", "https://api.bilibili.com:444/x", "https://hdslb.com/x",
        "https://x.bilibili.com/x", "https://a..hdslb.com/x", "https://I0.hdslb.com/x", "ftp://api.bilibili.com/x", "", "api.bilibili.com/x"};
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); ++i) {
        if (wili_server_rewrite(&s, skip[i], out, sizeof(out)) != 0) { printf("FAIL rewrote \"%s\"\n", skip[i]); ++failures; }
        CHECK_STR(out, "untouched");
    }
    /* Off, or incomplete settings, never rewrite. */
    WiliServer off = s; off.enabled = 0;
    CHECK(wili_server_rewrite(&off, "https://api.bilibili.com/x", out, sizeof(out)) == 0);
    WiliServer notoken = s; notoken.token[0] = 0;
    CHECK(wili_server_rewrite(&notoken, "https://api.bilibili.com/x", out, sizeof(out)) == 0);
    /* Overflow is reported, not truncated into a different URL. */
    for (size_t cap = 1; cap < 92; ++cap) {
        char small[100]; memset(small, 'z', sizeof(small));
        int r = wili_server_rewrite(&s, "https://api.bilibili.com/x/web-interface/view?bvid=BV1xx411c7mD", small, cap);
        if (r != -1) { printf("FAIL cap %zu returned %d\n", cap, r); ++failures; break; }
        CHECK(small[0] == 0 && small[cap] == 'z');
    }
}

static void test_stream_url(void) {
    WiliServer s = server();
    WiliStreamRequest r = {"BV1xx411c7mD", 12345, 0, 0, WILI_PRESET_BALANCED, WILI_PROFILE_AUTO};
    char out[512];
    CHECK(wili_server_stream_url(&s, &r, out, sizeof(out)) == 0);
    CHECK_STR(out, "http://192.168.1.20:8787/wili/v1/stream?bvid=BV1xx411c7mD&cid=12345&profile=auto&w=480&h=272&fps=30&vb=1000&ab=96&k=k7m2q9xa");
    r.start_ms = 61500; r.profile = WILI_PROFILE_CABAC; r.preset = WILI_PRESET_PERFORMANCE;
    CHECK(wili_server_stream_url(&s, &r, out, sizeof(out)) == 0);
    CHECK_STR(out, "http://192.168.1.20:8787/wili/v1/stream?bvid=BV1xx411c7mD&cid=12345&t=61.500&profile=cabac&w=384&h=216&fps=24&vb=700&ab=64&k=k7m2q9xa");
    r.start_ms = 3005; r.pgc = 1; r.bvid = NULL; r.profile = WILI_PROFILE_CAVLC; r.preset = WILI_PRESET_QUALITY;
    CHECK(wili_server_stream_url(&s, &r, out, sizeof(out)) == 0);
    CHECK_STR(out, "http://192.168.1.20:8787/wili/v1/stream?pgc=1&cid=12345&t=3.005&profile=cavlc&w=480&h=272&fps=30&vb=1500&ab=128&k=k7m2q9xa");
    /* A hostile bvid cannot add parameters. */
    r.pgc = 0; r.bvid = "BV1&profile=x#";
    CHECK(wili_server_stream_url(&s, &r, out, sizeof(out)) == 0);
    CHECK(strstr(out, "bvid=BV1%26profile%3Dx%23&cid="));
    r.bvid = ""; CHECK(wili_server_stream_url(&s, &r, out, sizeof(out)) == -1);
    r.bvid = "BV1xx411c7mD"; r.cid = 0; CHECK(wili_server_stream_url(&s, &r, out, sizeof(out)) == -1);
    r.cid = 1; CHECK(wili_server_stream_url(&s, &r, out, 20) == -1);

    CHECK(wili_server_hello_url(&s, out, sizeof(out)) == 0);
    CHECK_STR(out, "http://192.168.1.20:8787/wili/v1/hello?k=k7m2q9xa");
    CHECK(wili_server_logout_url(&s, out, sizeof(out)) == 0);
    CHECK_STR(out, "http://192.168.1.20:8787/wili/v1/logout?k=k7m2q9xa");
    WiliServer off = s; off.enabled = 0;
    CHECK(wili_server_hello_url(&off, out, sizeof(out)) == -1);
    CHECK(wili_server_logout_url(&off, out, sizeof(out)) == -1);
}

static size_t read_hex(const char *dir, const char *name, unsigned char *out, size_t cap) {
    char path[512]; snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) { printf("FAIL cannot open %s\n", path); ++failures; return 0; }
    size_t n = 0; int hi = -1, c;
    while ((c = fgetc(f)) != EOF) {
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) continue;
        if (hi < 0) hi = v; else { if (n < cap) out[n++] = (unsigned char)(hi << 4 | v); hi = -1; }
    }
    fclose(f);
    return n;
}

static void test_discovery(const char *dir) {
    int port = 0; char name[WILI_SERVER_NAME_MAX];
    CHECK(wili_beacon_parse((const unsigned char *)"WILIMINI-SERVER 1 8787 Desk PC\n", 30, &port, name, sizeof(name)) == 0);
    CHECK(port == 8787); CHECK_STR(name, "Desk PC");
    const char *bad[] = {"", "HELLO 1 2 n\n", "WILIMINI-SERVER 1 0 n\n", "WILIMINI-SERVER 1 99999 n\n",
        "WILIMINI-SERVER x 80 n\n", "WILIMINI-SERVER 1 80", "WILIMINI-SERVER 0 80 n\n"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        if (wili_beacon_parse((const unsigned char *)bad[i], strlen(bad[i]), &port, name, sizeof(name)) == 0) {
            printf("FAIL accepted beacon \"%s\"\n", bad[i]); ++failures;
        }
    unsigned char probe[64];
    int n = wili_beacon_probe(probe, sizeof(probe));
    CHECK(n == 20 && !memcmp(probe, "WILIMINI-DISCOVER 1\n", 20));
    CHECK(wili_beacon_probe(probe, 5) == -1);

    unsigned char query[128], want_query[128], response[1500];
    n = wili_mdns_query(query, sizeof(query));
    size_t want = read_hex(dir, "wili_mdns_query.hex", want_query, sizeof(want_query));
    CHECK(n > 0 && (size_t)n == want && !memcmp(query, want_query, want));
    size_t length = read_hex(dir, "wili_mdns_response.hex", response, sizeof(response));
    CHECK(length > 60);
    CHECK(wili_mdns_parse(response, length, &port, name, sizeof(name)) == 0);
    CHECK(port == 8787); CHECK_STR(name, "Desk PC");
    /* A small name buffer truncates but never overflows. */
    char tiny[4];
    CHECK(wili_mdns_parse(response, length, &port, tiny, sizeof(tiny)) == 0 && strlen(tiny) == 3);
    /* A query is not a response. */
    CHECK(wili_mdns_parse(query, (size_t)n, &port, name, sizeof(name)) == -1);
    /* Every truncation and a few thousand corruptions must be rejected or parsed safely. */
    for (size_t cut = 0; cut < length; ++cut) (void)wili_mdns_parse(response, cut, &port, name, sizeof(name));
    unsigned seed = 12345;
    for (int i = 0; i < 20000; ++i) {
        unsigned char copy[1500]; memcpy(copy, response, length);
        for (int k = 0; k < 1 + i % 4; ++k) { seed = seed * 1103515245u + 12345u; copy[(seed >> 8) % length] = (unsigned char)(seed >> 16); }
        (void)wili_mdns_parse(copy, length, &port, name, sizeof(name));
    }
    /* A compression loop must not hang. */
    unsigned char loop[] = {0,0,0x84,0,0,1,0,0,0,0,0,0, 0xC0, 12, 0,12,0,1};
    CHECK(wili_mdns_parse(loop, sizeof(loop), &port, name, sizeof(name)) == -1);
}

int main(int argc, char **argv) {
    if (argc < 2) { puts("usage: stream_server_test tests/data"); return 2; }
    test_address(); test_token(); test_rewrite(); test_stream_url(); test_discovery(argv[1]);
    WiliStreamParams p = wili_stream_params(WILI_PRESET_BALANCED);
    CHECK(p.width == 480 && p.height == 272 && p.fps == 30);
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    puts("stream_server_test: ok");
    return 0;
}
