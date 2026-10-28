/* Diagnostic only: no allocation tracing in production builds. */
#include <stddef.h>
#include <stdio.h>

void *__real_malloc(size_t size);
void *__real_realloc(void *ptr, size_t size);
void *__real_memalign(size_t alignment, size_t size);

void *__wrap_malloc(size_t size) {
    void *ptr = __real_malloc(size);
    if (size >= 512 * 1024)
        printf("ALLOC malloc bytes=%u caller=%p ok=%d\n", (unsigned)size,
               __builtin_return_address(0), ptr != NULL);
    return ptr;
}
void *__wrap_realloc(void *old, size_t size) {
    void *ptr = __real_realloc(old, size);
    if (size >= 512 * 1024)
        printf("ALLOC realloc bytes=%u caller=%p ok=%d\n", (unsigned)size,
               __builtin_return_address(0), ptr != NULL);
    return ptr;
}
void *__wrap_memalign(size_t alignment, size_t size) {
    void *ptr = __real_memalign(alignment, size);
    if (size >= 512 * 1024)
        printf("ALLOC memalign bytes=%u caller=%p ok=%d\n", (unsigned)size,
               __builtin_return_address(0), ptr != NULL);
    return ptr;
}
