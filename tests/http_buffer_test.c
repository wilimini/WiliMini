#include "../client/http_buffer.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const size_t limit = 512 * 1024;
    char *text = malloc(4096);
    size_t capacity = 4096;
    assert(text);

    /* Unknown, oversized and already-met hints keep the buffer. */
    bili_http_buffer_hint(&text, &capacity, -1, limit);
    assert(capacity == 4096);
    bili_http_buffer_hint(&text, &capacity, (long long)limit + 1, limit);
    assert(capacity == 4096);
    bili_http_buffer_hint(&text, &capacity, 100, limit);
    assert(capacity == 4096);

    /* A known length is reserved once, with room for the trailing NUL. */
    bili_http_buffer_hint(&text, &capacity, 200000, limit);
    assert(capacity == 200001);
    assert(bili_http_buffer_reserve(&text, &capacity, 200000, limit) == 0);
    assert(capacity == 200001);

    /* A compressed body past the hint still grows by doubling to the limit. */
    assert(bili_http_buffer_reserve(&text, &capacity, 200001, limit) == 0);
    assert(capacity == 400002);
    assert(bili_http_buffer_reserve(&text, &capacity, limit, limit) == 0);
    assert(capacity == limit + 1);
    assert(bili_http_buffer_reserve(&text, &capacity, limit + 1, limit) < 0);
    assert(capacity == limit + 1);

    /* A hint exactly at the limit is accepted. */
    free(text);
    text = malloc(4096);
    capacity = 4096;
    assert(text);
    bili_http_buffer_hint(&text, &capacity, (long long)limit, limit);
    assert(capacity == limit + 1);
    free(text);
    puts("HTTP buffer hint and growth passed");
    return 0;
}
