#ifndef BILI_JSON_DROP_H
#define BILI_JSON_DROP_H
#include <stddef.h>
#include <string.h>

/*
 * Streaming filter that DELIBERATELY DROPS the `ugc_season` member of a JSON
 * response while the bytes arrive, so it is never buffered or parsed.
 *
 * Why: the video detail response embeds the uploader's whole "collection"
 * (合集). Every episode repeats a full `arc`, so one video's 388-episode
 * collection was 722 KB of a 733 KB response and a bigger one overflowed even
 * a 1 MiB buffer (curl error 23 once the size limit refused a chunk). WiliMini
 * has no collection UI and nothing reads the field, so we discard it up front;
 * memory use is constant whatever the collection size.
 *
 * What it does: a member keyed `ugc_season` that sits directly inside a
 * top-level member's object (i.e. `data.ugc_season`) has its key renamed to
 * `ugc_season_dropped` and its value replaced by 0. The output is always valid
 * JSON, and consumers that ask for `ugc_season` simply do not find it. The
 * value is skipped string-aware, across any chunk boundary.
 *
 * To support collections later, replace this with a filter that keeps a slim
 * subset (aid/bvid/cid/title per episode) instead of dropping the value.
 *
 * Usage: zero-initialise a BiliJsonDrop, then call bili_json_drop() per chunk
 * with an output buffer of at least in_len + BILI_JSON_DROP_SLACK bytes.
 */
#define BILI_JSON_DROP_KEY "\"ugc_season\""
#define BILI_JSON_DROP_KEY_LEN 12
#define BILI_JSON_DROP_RENAMED "\"ugc_season_dropped\":"
#define BILI_JSON_DROP_SLACK 64 /* held key bytes + renamed key + replacement */

typedef struct {
    int depth;            /* container nesting outside skipped values */
    int in_string, escape;
    int obj2;             /* container at depth 2 is an object */
    int expect_key;       /* at depth 2: next string is a member key */
    int hold_len;         /* bytes of a candidate key held back */
    char hold[BILI_JSON_DROP_KEY_LEN];
    int mode;             /* 0 pass, 1 holding key, 2 await ':', 3 await value, 4 skipping */
    int skip_depth;
} BiliJsonDrop;

static size_t bili_json_drop(BiliJsonDrop *f, const char *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const char c = in[i];
    again:
        switch (f->mode) {
        case 1: /* inside a depth-2 key string: hold up to the key length */
            if (f->hold_len == BILI_JSON_DROP_KEY_LEN - 1 && c == '"') {
                f->hold[f->hold_len++] = c;
                if (memcmp(f->hold, BILI_JSON_DROP_KEY, BILI_JSON_DROP_KEY_LEN) == 0) {
                    f->mode = 2;
                } else { /* cannot happen: only a matching prefix is held */
                    memcpy(out + o, f->hold, (size_t)f->hold_len); o += (size_t)f->hold_len;
                    f->mode = 0; f->in_string = 0;
                }
                continue;
            }
            if (c != '\\' && c != '"' && f->hold_len < BILI_JSON_DROP_KEY_LEN - 1 &&
                (unsigned char)c == (unsigned char)BILI_JSON_DROP_KEY[f->hold_len]) {
                f->hold[f->hold_len++] = c; /* still a prefix of the key */
                continue;
            }
            /* Not our key: release what was held and carry on inside the string. */
            memcpy(out + o, f->hold, (size_t)f->hold_len); o += (size_t)f->hold_len;
            f->mode = 0; f->in_string = 1; f->escape = 0;
            goto again;
        case 2: /* matched key; wait for ':' */
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
            if (c == ':') {
                memcpy(out + o, BILI_JSON_DROP_RENAMED, sizeof(BILI_JSON_DROP_RENAMED) - 1);
                o += sizeof(BILI_JSON_DROP_RENAMED) - 1;
                f->expect_key = 0; f->mode = 3;
                continue;
            }
            memcpy(out + o, f->hold, (size_t)f->hold_len); o += (size_t)f->hold_len;
            f->mode = 0; f->in_string = 0;
            goto again;
        case 3: /* wait for the first byte of the value */
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { out[o++] = c; continue; }
            if (c == '{' || c == '[') { f->mode = 4; f->skip_depth = 1; f->in_string = f->escape = 0; continue; }
            f->mode = 0; /* scalar value: nothing large to drop */
            goto again;
        case 4: /* skipping the value, string-aware */
            if (f->in_string) {
                if (f->escape) f->escape = 0;
                else if (c == '\\') f->escape = 1;
                else if (c == '"') f->in_string = 0;
            } else if (c == '"') f->in_string = 1;
            else if (c == '{' || c == '[') f->skip_depth++;
            else if ((c == '}' || c == ']') && --f->skip_depth == 0) { out[o++] = '0'; f->mode = 0; }
            continue;
        default: break;
        }
        /* pass-through, tracking just enough structure to find depth-2 keys */
        if (f->in_string) {
            if (f->escape) f->escape = 0;
            else if (c == '\\') f->escape = 1;
            else if (c == '"') f->in_string = 0;
        } else if (c == '"') {
            if (f->depth == 2 && f->obj2 && f->expect_key) {
                f->hold[0] = c; f->hold_len = 1; f->mode = 1; f->in_string = 1; /* hold, do not emit */
                continue;
            }
            f->in_string = 1;
        } else if (c == '{' || c == '[') {
            f->depth++;
            if (f->depth == 2) { f->obj2 = c == '{'; f->expect_key = f->obj2; }
        } else if (c == '}' || c == ']') {
            f->depth--;
        } else if (f->depth == 2 && f->obj2) {
            if (c == ',') f->expect_key = 1;
            else if (c == ':') f->expect_key = 0;
        }
        out[o++] = c;
    }
    return o;
}
#endif
