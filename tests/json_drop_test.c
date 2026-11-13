#include "../client/json_drop.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Runs the filter over `in`, feeding it `chunk` bytes at a time. */
static char *run(const char *in, size_t chunk) {
    const size_t n = strlen(in);
    char *out = calloc(1, n + 256), *tmp = malloc(chunk + BILI_JSON_DROP_SLACK);
    BiliJsonDrop f;
    memset(&f, 0, sizeof f);
    size_t used = 0;
    for (size_t i = 0; i < n; i += chunk) {
        size_t len = n - i < chunk ? n - i : chunk;
        size_t w = bili_json_drop(&f, in + i, len, tmp);
        assert(w <= len + BILI_JSON_DROP_SLACK);
        memcpy(out + used, tmp, w);
        used += w;
    }
    free(tmp);
    return out;
}

static void check(const char *in, const char *want) {
    static const size_t chunks[] = {1, 2, 3, 5, 7, 64, 100000};
    for (size_t i = 0; i < sizeof chunks / sizeof *chunks; i++) {
        char *got = run(in, chunks[i]);
        if (strcmp(got, want) != 0) {
            fprintf(stderr, "chunk=%zu\n in:   %s\n got:  %s\n want: %s\n", chunks[i], in, got, want);
            abort();
        }
        free(got);
    }
}

int main(void) {
    /* Middle, first and last member; nested containers and braces inside strings. */
    check("{\"code\":0,\"data\":{\"a\":1,\"ugc_season\":{\"x\":[1,{\"y\":\"}]\\\"{\"}],\"z\":{}},\"b\":2}}",
          "{\"code\":0,\"data\":{\"a\":1,\"ugc_season_dropped\":0,\"b\":2}}");
    check("{\"data\":{\"ugc_season\":{\"k\":1},\"b\":2}}", "{\"data\":{\"ugc_season_dropped\":0,\"b\":2}}");
    check("{\"data\":{\"b\":2,\"ugc_season\":[{\"k\":1}]}}", "{\"data\":{\"b\":2,\"ugc_season_dropped\":0}}");
    check("{\"data\":{ \"ugc_season\" : { \"k\" : 1 } }}", "{\"data\":{ \"ugc_season_dropped\": 0 }}");

    /* A scalar value is kept; the key is still renamed so consumers never see it. */
    check("{\"data\":{\"ugc_season\":null,\"b\":1}}", "{\"data\":{\"ugc_season_dropped\":null,\"b\":1}}");

    /* Other keys, values, depths and prefixes pass through untouched. */
    const char *same[] = {
        "{\"data\":{\"ugc_seasons\":{\"k\":1},\"ugc\":1,\"ugc_season_x\":2,\"u\":3}}",
        "{\"ugc_season\":{\"k\":1}}",                      /* depth 1 is not data.* */
        "{\"data\":{\"a\":{\"ugc_season\":{\"k\":1}}}}",   /* depth 3 is not data.* */
        "{\"data\":{\"s\":\"ugc_season\",\"t\":[\"ugc_season\"]}}", /* string values, array */
        "{\"data\":{\"a\\\"ugc_season\":1}}",              /* escaped quote in another key */
        "{\"data\":[\"ugc_season\",{\"ugc_season\":1}]}",  /* array at depth 2 */
    };
    for (size_t i = 0; i < sizeof same / sizeof *same; i++) check(same[i], same[i]);

    /* A large value is dropped with constant output. */
    size_t big = 3u << 20;
    char *in = malloc(big + 128);
    strcpy(in, "{\"data\":{\"ugc_season\":{\"s\":\"");
    size_t prefix = strlen(in);
    memset(in + prefix, 'a', big);
    strcpy(in + prefix + big, "\"}}}");
    char *got = run(in, 16384);
    assert(strcmp(got, "{\"data\":{\"ugc_season_dropped\":0}}") == 0);
    free(got);
    free(in);
    puts("json_drop_test ok");
    return 0;
}
