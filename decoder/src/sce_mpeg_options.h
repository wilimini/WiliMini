#ifndef WILI_SCE_MPEG_OPTIONS_H
#define WILI_SCE_MPEG_OPTIONS_H
/* Experimental sceMpeg settings, read from "sce_mpeg.cfg" in the WiliMini folder (lines of key=value,
 * # starts a comment). They exist because the firmware's acceptance of our synthesized stream can only be
 * tested on a PSP: each difference from the setup that last produced pictures can be switched on there
 * without a rebuild. The defaults below are that setup; nothing here is a tuning recommendation.
 * No PSP headers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int psmf_version;    /* 12..15: the "00NN" in the header; the original build said 15 */
    int psmf_full;       /* 1: the full header a real file has; 0: only the fields PPSSPP reads */
    int ring_packets;    /* ring buffer size in 2048-byte packets; real players use 60, we used 256 */
    int put_packets;     /* at most this many packets per sceMpegRingbufferPut; 0 = all that are free */
    int register_atrac;  /* 1: also register the audio stream (no audio data is ever sent) */
    int stop_flush;      /* 1: sceMpegAvcDecodeStop + sceMpegFlushAllStream before teardown */
    int direct_feed;     /* 1: hand each frame to the decoder directly (no program stream, no ring buffer
                          * contents, no header); 0: wrap it in a program stream; -1: direct on a PSP,
                          * program stream under the emulator, which only understands that */
} SceMpegOptions;

static inline SceMpegOptions sce_mpeg_default_options(void) {
    SceMpegOptions o = {15, 0, 256, 0, 0, 0, -1};
    return o;
}

/* Sets one option; returns 1 when the key is known and the value is in range. */
static inline int sce_mpeg_set_option(SceMpegOptions *o, const char *key, size_t key_length, long value) {
    #define OPTION(name, field, lo, hi) \
        if (key_length == sizeof(name) - 1 && !memcmp(key, name, key_length)) { \
            if (value < (lo) || value > (hi)) return 0; \
            o->field = (int)value; return 1; }
    OPTION("psmf_version", psmf_version, 12, 15)
    OPTION("psmf_full", psmf_full, 0, 1)
    OPTION("ring_packets", ring_packets, 16, 1024)
    OPTION("put_packets", put_packets, 0, 1024)
    OPTION("register_atrac", register_atrac, 0, 1)
    OPTION("stop_flush", stop_flush, 0, 1)
    OPTION("direct_feed", direct_feed, 0, 1)
    #undef OPTION
    return 0;
}

/* Applies the options in text. Returns how many were applied; *rejected counts lines that were not. */
static inline int sce_mpeg_parse_options(const char *text, size_t length, SceMpegOptions *o, int *rejected) {
    int applied = 0, bad = 0;
    size_t i = 0;
    while (i < length) {
        size_t end = i;
        while (end < length && text[end] != '\n') ++end;
        size_t a = i, b = end;
        while (a < b && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r')) ++a;
        while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r')) --b;
        if (a < b && text[a] != '#') {
            const char *equals = memchr(text + a, '=', b - a);
            if (!equals) ++bad;
            else {
                size_t key_end = (size_t)(equals - text);
                while (key_end > a && (text[key_end - 1] == ' ' || text[key_end - 1] == '\t')) --key_end;
                char number[16];
                size_t v = (size_t)(equals - text) + 1;
                while (v < b && (text[v] == ' ' || text[v] == '\t')) ++v;
                size_t n = b - v;
                if (n == 0 || n >= sizeof(number)) { ++bad; }
                else {
                    memcpy(number, text + v, n);
                    number[n] = 0;
                    char *stop = NULL;
                    long value = strtol(number, &stop, 10);
                    if (*stop || !sce_mpeg_set_option(o, text + a, key_end - a, value)) ++bad;
                    else ++applied;
                }
            }
        }
        i = end + 1;
    }
    if (rejected) *rejected = bad;
    return applied;
}

/* Reads the file if there is one; returns the number of options applied, or -1 when it is missing. */
static inline int sce_mpeg_load_options(const char *path, SceMpegOptions *o, int *rejected) {
    FILE *file = fopen(path, "rb");
    if (!file) { if (rejected) *rejected = 0; return -1; }
    char text[1024];
    size_t length = fread(text, 1, sizeof(text), file);
    fclose(file);
    return sce_mpeg_parse_options(text, length, o, rejected);
}
#endif
