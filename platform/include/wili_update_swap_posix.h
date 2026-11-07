#ifndef WILI_UPDATE_SWAP_POSIX_H
#define WILI_UPDATE_SWAP_POSIX_H
/* newlib/POSIX binding for wili_update_swap.h. Used by the loader, the client
 * (commit) and host tests, so the same code path runs everywhere. */
#include "wili_update_swap.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int wili_swap_px_exists(void *ctx, const char *p) {
    struct stat st; (void)ctx;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
static int wili_swap_px_rename(void *ctx, const char *a, const char *b) { (void)ctx; return rename(a, b); }
static int wili_swap_px_remove(void *ctx, const char *p) {
    (void)ctx;
    struct stat st;
    if (stat(p, &st) != 0) return 0; /* missing is fine */
    return remove(p);
}
static int wili_swap_px_read(void *ctx, const char *p, char *buf, size_t cap) {
    (void)ctx;
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap, f);
    int bad = ferror(f);
    fclose(f);
    return bad ? -1 : (int)n;
}
static int wili_swap_px_write(void *ctx, const char *p, const char *d, size_t n) {
    (void)ctx;
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    int ok = fwrite(d, 1, n, f) == n;
    ok = fflush(f) == 0 && ok;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}
static int wili_swap_px_copy(void *ctx, const char *from, const char *to) {
    static char buf[16384];
    (void)ctx;
    FILE *in = fopen(from, "rb");
    if (!in) return -1;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return -1; }
    int ok = 1;
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (ferror(in)) ok = 0;
    fclose(in);
    ok = fflush(out) == 0 && ok;
    return (fclose(out) == 0 && ok) ? 0 : -1;
}
static int wili_swap_px_mkdirs(void *ctx, const char *file_path) {
    (void)ctx;
    char tmp[256];
    size_t n = strlen(file_path);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, file_path, n + 1);
    for (size_t i = 1; i < n; ++i) {
        if (tmp[i] != '/') continue;
        if (i > 0 && tmp[i - 1] == ':') continue; /* "ms0:/" */
        tmp[i] = 0;
        struct stat st;
        if (stat(tmp, &st) != 0 && mkdir(tmp, 0777) != 0) return -1;
        tmp[i] = '/';
    }
    return 0;
}
static const wili_swap_fs wili_swap_posix_fs = {
    NULL, wili_swap_px_exists, wili_swap_px_rename, wili_swap_px_remove,
    wili_swap_px_read, wili_swap_px_write, wili_swap_px_mkdirs, wili_swap_px_copy
};
#endif
