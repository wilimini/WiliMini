#ifndef WILI_UPDATE_SWAP_H
#define WILI_UPDATE_SWAP_H
/* Crash-safe swap of a staged update into the live package folder (issue #26).
 *
 * The client extracts a verified update into <root>/pending/ and writes
 * <root>/pending/MANIFEST (one relative path per line) last. On the next
 * launch the loader calls wili_swap_run() before loading any client module.
 *
 * Every step is idempotent and the current phase is stored in
 * <root>/update.state, so a power loss at any point is resumed or rolled
 * back on the next launch. Old files are kept as "<name>.bak" until the next
 * update. Files not listed in the manifest (settings, themes) are never
 * touched. EBOOT.PBP is always swapped last.
 *
 * After the swap the new version has WILI_SWAP_TRIAL_BOOTS launches to call
 * wili_swap_commit(); if it never does, the next launch rolls back.
 *
 * Plain C with an injected file system so it can be host-tested with crash
 * injection. The PSP binding lives in the loader. */
#include <stddef.h>

#define WILI_SWAP_TRIAL_BOOTS 3

typedef struct wili_swap_fs {
    void *ctx;
    int (*exists)(void *ctx, const char *path);                      /* regular file: 1/0 */
    int (*rename)(void *ctx, const char *from, const char *to);      /* 0 on success */
    int (*remove)(void *ctx, const char *path);                      /* 0 on success or if missing */
    int (*read)(void *ctx, const char *path, char *buf, size_t cap); /* bytes read, <0 on error */
    int (*write)(void *ctx, const char *path, const char *data, size_t len); /* create/truncate, 0 ok */
    int (*mkdirs)(void *ctx, const char *file_path);                 /* create parent dirs, 0 ok */
} wili_swap_fs;

typedef enum {
    WILI_SWAP_NONE = 0,      /* nothing to do, launch normally */
    WILI_SWAP_APPLIED,       /* new version in place (trial boot); launch it */
    WILI_SWAP_ROLLED_BACK,   /* update failed; old version restored; update.failed written */
    WILI_SWAP_ERROR          /* could not make progress; launch what is there */
} wili_swap_status;

wili_swap_status wili_swap_run(const wili_swap_fs *fs, const char *root);
/* Called by the client once the new version has started successfully. */
int wili_swap_commit(const wili_swap_fs *fs, const char *root);
/* 1 if safe to extract/write a manifest entry. */
int wili_swap_path_ok(const char *rel);

#endif
