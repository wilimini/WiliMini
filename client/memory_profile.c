/* Diagnostic only: no allocation tracing in production builds. */
#include <stddef.h>
#include <malloc.h>
#include <wili_log.h>

static void allocation_log(const char *kind, size_t size, void *caller, void *ptr) {
    if (size >= 512 * 1024 || (!ptr && size)) {
        const struct mallinfo heap = mallinfo();
        WLOG_I(WLC_MEM, "ALLOC %s bytes=%u caller=%p ok=%d heap_used=%u heap_arena_free=%u",
               kind, (unsigned)size, caller, ptr != NULL,
               (unsigned)heap.uordblks, (unsigned)heap.fordblks);
    }
}

void *__real_malloc(size_t size);
void *__real_realloc(void *ptr, size_t size);
void *__real_memalign(size_t alignment, size_t size);

void *__wrap_malloc(size_t size) {
    void *ptr = __real_malloc(size);
    allocation_log("malloc", size, __builtin_return_address(0), ptr);
    return ptr;
}
void *__wrap_realloc(void *old, size_t size) {
    void *ptr = __real_realloc(old, size);
    allocation_log("realloc", size, __builtin_return_address(0), ptr);
    return ptr;
}
void *__wrap_memalign(size_t alignment, size_t size) {
    void *ptr = __real_memalign(alignment, size);
    allocation_log("memalign", size, __builtin_return_address(0), ptr);
    return ptr;
}
