#ifndef BILI_HTTP_SPILL_H
#define BILI_HTTP_SPILL_H
#include "http_buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Bounded HTTP response accumulator with threshold-based storage fallback.
 *
 * Responses up to `ram_limit` stay purely in RAM (using bili_http_buffer_*).
 * Once a response exceeds `ram_limit` (or a Content-Length hint already exceeds
 * it), buffered bytes are flushed to a temporary file under `spill_dir`, the
 * RAM buffer is freed immediately, and subsequent chunks stream straight to
 * storage up to `spill_limit`.
 *
 * When `spill_limit == 0` or `spill_dir == NULL`, spilling is disabled and
 * `ram_limit` acts as a hard in-RAM limit.
 */
#ifndef BILI_HTTP_RAM_THRESHOLD
#define BILI_HTTP_RAM_THRESHOLD (512u * 1024u)
#endif
#ifndef BILI_HTTP_SPILL_LIMIT
#define BILI_HTTP_SPILL_LIMIT (16u * 1024u * 1024u)
#endif
#ifndef BILI_HTTP_SPILL_PREFIX
#define BILI_HTTP_SPILL_PREFIX "http_spill_"
#endif

enum {
    BILI_SPILL_OK = 0,
    BILI_SPILL_MEMORY = 1,
    BILI_SPILL_IO = 2
};

typedef struct {
    char *text;
    size_t used;
    size_t capacity;
    size_t ram_limit;
    size_t spill_limit;
    const char *spill_dir;
    FILE *file;
    char path[256];
    int spilled;
    int prefer_spill;
    int failure;
} BiliHttpSpill;

static int bili_http_spill_can_spill(const BiliHttpSpill *s) {
    return s->spill_dir && s->spill_dir[0] && s->spill_limit > 0;
}

static size_t bili_http_spill_effective_limit(const BiliHttpSpill *s) {
    if (bili_http_spill_can_spill(s) && s->spill_limit > s->ram_limit)
        return s->spill_limit;
    return s->ram_limit;
}

static int bili_http_spill_init(BiliHttpSpill *s, size_t ram_limit,
                                size_t spill_limit, const char *spill_dir) {
    memset(s, 0, sizeof(*s));
    s->ram_limit = ram_limit;
    s->spill_limit = spill_limit;
    s->spill_dir = spill_dir;
    if (ram_limit == 0) {
        s->prefer_spill = 1;
        return 0;
    }
    const size_t initial = ram_limit + 1 < 4096 ? ram_limit + 1 : 4096;
    s->text = calloc(1, initial);
    if (!s->text) {
        s->failure = BILI_SPILL_MEMORY;
        return -1;
    }
    s->capacity = initial;
    return 0;
}

static void bili_http_spill_hint(BiliHttpSpill *s, long long expected) {
    if (expected <= 0 || s->spilled || s->used) return;
    if ((unsigned long long)expected > s->ram_limit) {
        if (bili_http_spill_can_spill(s) &&
            (unsigned long long)expected <= s->spill_limit) {
            s->prefer_spill = 1;
        }
        return;
    }
    bili_http_buffer_hint(&s->text, &s->capacity, expected, s->ram_limit);
}

static int bili_http_spill_open(BiliHttpSpill *s) {
    if (s->spilled) return 0;
    if (!bili_http_spill_can_spill(s)) {
        s->failure = BILI_SPILL_MEMORY;
        return -1;
    }
    static unsigned spill_seq;
    const unsigned id = __atomic_add_fetch(&spill_seq, 1u, __ATOMIC_RELAXED);
    const int n = snprintf(s->path, sizeof(s->path), "%s/%s%u.tmp",
                           s->spill_dir, BILI_HTTP_SPILL_PREFIX, id);
    if (n <= 0 || (size_t)n >= sizeof(s->path)) {
        s->path[0] = 0;
        s->failure = BILI_SPILL_IO;
        return -1;
    }
    s->file = fopen(s->path, "wb");
    if (!s->file) {
        s->path[0] = 0;
        s->failure = BILI_SPILL_IO;
        return -1;
    }
    setvbuf(s->file, NULL, _IOFBF, 8192);
    if (s->used > 0 && s->text) {
        if (fwrite(s->text, 1, s->used, s->file) != s->used) {
            fclose(s->file);
            s->file = NULL;
            remove(s->path);
            s->path[0] = 0;
            s->failure = BILI_SPILL_IO;
            return -1;
        }
    }
    free(s->text);
    s->text = NULL;
    s->capacity = 0;
    s->spilled = 1;
    return 0;
}

static int bili_http_spill_write(BiliHttpSpill *s, const char *data, size_t bytes) {
    if (!bytes) return 0;
    const size_t limit = bili_http_spill_effective_limit(s);
    if (s->used > limit || bytes > limit - s->used) {
        s->failure = BILI_SPILL_MEMORY;
        return -1;
    }
    if (s->spilled) {
        if (fwrite(data, 1, bytes, s->file) != bytes) {
            s->failure = BILI_SPILL_IO;
            return -1;
        }
        s->used += bytes;
        return 0;
    }
    if (!s->prefer_spill && s->used <= s->ram_limit &&
        bytes <= s->ram_limit - s->used &&
        bili_http_buffer_reserve(&s->text, &s->capacity, s->used + bytes, s->ram_limit) == 0) {
        memcpy(s->text + s->used, data, bytes);
        s->used += bytes;
        s->text[s->used] = 0;
        return 0;
    }
    if (!bili_http_spill_can_spill(s)) {
        s->failure = BILI_SPILL_MEMORY;
        return -1;
    }
    if (bili_http_spill_open(s) < 0) return -1;
    if (fwrite(data, 1, bytes, s->file) != bytes) {
        s->failure = BILI_SPILL_IO;
        return -1;
    }
    s->used += bytes;
    return 0;
}

static int bili_http_spill_finish(BiliHttpSpill *s) {
    if (s->file) {
        const int close_err = fclose(s->file);
        s->file = NULL;
        if (close_err != 0) {
            remove(s->path);
            s->path[0] = 0;
            s->spilled = 0;
            s->failure = BILI_SPILL_IO;
            return -1;
        }
    }
    return 0;
}

static void bili_http_spill_abort(BiliHttpSpill *s) {
    if (s->file) {
        fclose(s->file);
        s->file = NULL;
    }
    if (s->path[0]) {
        remove(s->path);
        s->path[0] = 0;
    }
    free(s->text);
    s->text = NULL;
    s->capacity = 0;
    s->used = 0;
    s->spilled = 0;
}

#endif
