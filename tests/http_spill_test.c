#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "../client/http_spill.h"
#include "../client/json_drop.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *read_entire_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    assert(fseek(f, 0, SEEK_END) == 0);
    long sz = ftell(f);
    assert(sz >= 0);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    assert(buf != NULL);
    if (sz > 0) assert(fread(buf, 1, (size_t)sz, f) == (size_t)sz);
    buf[sz] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int main(void) {
    char dir[] = "/tmp/wilimini_spill_test_XXXXXX";
    assert(mkdtemp(dir) != NULL);

    /* 1. Responses up to ram_limit stay purely in RAM (no spill file created). */
    {
        BiliHttpSpill s;
        assert(bili_http_spill_init(&s, 1024, 16384, dir) == 0);
        bili_http_spill_hint(&s, 1024);
        char block[1024];
        memset(block, 'A', sizeof(block));
        assert(bili_http_spill_write(&s, block, 512) == 0);
        assert(bili_http_spill_write(&s, block + 512, 512) == 0);
        assert(bili_http_spill_finish(&s) == 0);
        assert(!s.spilled);
        assert(s.path[0] == '\0');
        assert(s.used == 1024);
        assert(s.text != NULL);
        assert(memcmp(s.text, block, 1024) == 0);
        assert(s.text[1024] == '\0');
        free(s.text);
    }

    /* 2. Mid-stream transition: crossing ram_limit flushes RAM to storage and frees text. */
    {
        const size_t total = 20000;
        char *payload = malloc(total);
        assert(payload != NULL);
        for (size_t i = 0; i < total; ++i) payload[i] = (char)('a' + (i % 26));

        BiliHttpSpill s;
        assert(bili_http_spill_init(&s, 2048, 32768, dir) == 0);
        for (size_t off = 0; off < total; ) {
            size_t chunk = total - off < 777 ? total - off : 777;
            assert(bili_http_spill_write(&s, payload + off, chunk) == 0);
            off += chunk;
            if (off > 2048) {
                assert(s.spilled == 1);
                assert(s.text == NULL);
                assert(s.capacity == 0);
            }
        }
        assert(bili_http_spill_finish(&s) == 0);
        assert(s.spilled == 1);
        assert(s.used == total);
        assert(file_exists(s.path));

        size_t got_len = 0;
        char *got = read_entire_file(s.path, &got_len);
        assert(got_len == total);
        assert(memcmp(got, payload, total) == 0);
        free(got);
        free(payload);
        assert(remove(s.path) == 0);
    }

    /* 3. Content-Length hint above ram_limit streams straight to spill on first write. */
    {
        BiliHttpSpill s;
        assert(bili_http_spill_init(&s, 1024, 8192, dir) == 0);
        bili_http_spill_hint(&s, 4096);
        assert(s.prefer_spill == 1);
        assert(bili_http_spill_write(&s, "hello", 5) == 0);
        assert(s.spilled == 1);
        assert(s.text == NULL);
        assert(bili_http_spill_finish(&s) == 0);
        size_t got_len = 0;
        char *got = read_entire_file(s.path, &got_len);
        assert(got_len == 5 && strcmp(got, "hello") == 0);
        free(got);
        assert(remove(s.path) == 0);
    }

    /* 4. Exceeding spill_limit fails with BILI_SPILL_MEMORY and abort deletes the temp file. */
    {
        BiliHttpSpill s;
        assert(bili_http_spill_init(&s, 512, 2048, dir) == 0);
        char buf[1024];
        memset(buf, 'X', sizeof(buf));
        assert(bili_http_spill_write(&s, buf, 1024) == 0);
        assert(s.spilled == 1);
        char saved_path[256];
        strcpy(saved_path, s.path);
        assert(file_exists(saved_path));
        assert(bili_http_spill_write(&s, buf, 1024) == 0);
        assert(bili_http_spill_write(&s, "Z", 1) < 0);
        assert(s.failure == BILI_SPILL_MEMORY);
        bili_http_spill_abort(&s);
        assert(!file_exists(saved_path));
    }

    /* 5. Spilling disabled (spill_limit == 0) enforces hard ram_limit. */
    {
        BiliHttpSpill s;
        assert(bili_http_spill_init(&s, 512, 0, NULL) == 0);
        char buf[600];
        memset(buf, 'Y', sizeof(buf));
        assert(bili_http_spill_write(&s, buf, 512) == 0);
        assert(bili_http_spill_write(&s, buf, 1) < 0);
        assert(s.failure == BILI_SPILL_MEMORY);
        bili_http_spill_abort(&s);
    }

    /* 6. Unwritable spill directory reports BILI_SPILL_IO cleanly. */
    {
        BiliHttpSpill s;
        assert(bili_http_spill_init(&s, 16, 4096, "/nonexistent_wilimini_dir_xyz") == 0);
        assert(bili_http_spill_write(&s, "0123456789abcdef", 16) == 0);
        assert(bili_http_spill_write(&s, "!", 1) < 0);
        assert(s.failure == BILI_SPILL_IO);
        bili_http_spill_abort(&s);
    }

    /* 7. json_drop.h + http_spill.h combined: huge ugc_season stays below ram_limit. */
    {
        size_t big = 100000;
        char *in = malloc(big + 128);
        assert(in != NULL);
        strcpy(in, "{\"code\":0,\"data\":{\"ugc_season\":{\"s\":\"");
        size_t p = strlen(in);
        memset(in + p, 'q', big);
        strcpy(in + p + big, "\"},\"title\":\"ok\"}}");
        size_t total = strlen(in);

        BiliJsonDrop drop;
        memset(&drop, 0, sizeof(drop));
        BiliHttpSpill s;
        assert(bili_http_spill_init(&s, 4096, 65536, dir) == 0);
        char filtered[4096 + BILI_JSON_DROP_SLACK];
        for (size_t off = 0; off < total; ) {
            size_t slice = total - off < 4096 ? total - off : 4096;
            size_t w = bili_json_drop(&drop, in + off, slice, filtered);
            assert(bili_http_spill_write(&s, filtered, w) == 0);
            off += slice;
        }
        assert(bili_http_spill_finish(&s) == 0);
        assert(!s.spilled);
        assert(strcmp(s.text, "{\"code\":0,\"data\":{\"ugc_season_dropped\":0,\"title\":\"ok\"}}") == 0);
        free(s.text);
        free(in);
    }

    assert(rmdir(dir) == 0);
    puts("http_spill_test ok");
    return 0;
}
