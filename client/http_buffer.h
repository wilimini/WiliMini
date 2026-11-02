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
#endif
