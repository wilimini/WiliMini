#ifndef BILI_HTTP_BUFFER_H
#define BILI_HTTP_BUFFER_H
#include <stdlib.h>
#include <stddef.h>

/* Limit excludes the trailing NUL. Keep the old allocation on failure. */
static int bili_http_buffer_reserve(char **text, size_t *capacity,
                                    size_t needed, size_t limit) {
    if (needed > limit || limit == (size_t)-1) return -1;
    if (*capacity > needed) return 0;
    size_t next = *capacity ? *capacity : 4096;
    if (next > limit + 1) next = limit + 1;
    while (next <= needed) {
        if (next > (limit + 1) / 2) { next = limit + 1; break; }
        next *= 2;
    }
    char *grown = realloc(*text, next);
    if (!grown) return -1;
    *text = grown;
    *capacity = next;
    return 0;
}

/* Grow once to an expected body size (such as Content-Length) before the
 * first write, so a large response does not pass through every doubling.
 * A compressed body can still exceed the hint and then grows as above.
 * Hints that are unknown, already met or over the limit are ignored, as is
 * an allocation failure: the regular growth path reports that. */
static void bili_http_buffer_hint(char **text, size_t *capacity,
                                  long long expected, size_t limit) {
    if (expected <= 0 || (unsigned long long)expected > limit || limit == (size_t)-1) return;
    const size_t next = (size_t)expected + 1;
    if (*capacity >= next) return;
    char *grown = realloc(*text, next);
    if (!grown) return;
    *text = grown;
    *capacity = next;
}
#endif
