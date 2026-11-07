/* Crash-injection test for the loader's update swap (issue #26). */
#include "wili_update_swap.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXF 96
typedef struct { char path[160]; char data[4096]; int used; } file_t;
static file_t fsx[MAXF];
static int ops, crash_at, crash_after; /* crash_at<0: never */
static jmp_buf boom;

static file_t *find(const char *p) { for (int i = 0; i < MAXF; ++i) if (fsx[i].used && !strcmp(fsx[i].path, p)) return &fsx[i]; return NULL; }
static file_t *make(const char *p) { file_t *f = find(p); if (f) return f; for (int i = 0; i < MAXF; ++i) if (!fsx[i].used) { fsx[i].used = 1; strcpy(fsx[i].path, p); fsx[i].data[0] = 0; return &fsx[i]; } abort(); }
static void put(const char *p, const char *d) { strcpy(make(p)->data, d); }
/* Power loss: before or after the mutation, then jump out. */
#define MUTATE(body) do { if (ops++ == crash_at && !crash_after) longjmp(boom, 1); body; if (ops - 1 == crash_at && crash_after) longjmp(boom, 1); } while (0)

static int t_exists(void *c, const char *p) { (void)c; return find(p) != NULL; }
static int same_dir(const char *a, const char *b) {
    const char *sa = strrchr(a, '/'), *sb = strrchr(b, '/');
    return sa && sb && (sa - a) == (sb - b) && !strncmp(a, b, (size_t)(sa - a));
}
static int t_rename(void *c, const char *a, const char *b) {
    /* PSP firmware and PPSSPP: no rename across directories, none onto an existing file. */
    (void)c; file_t *f = find(a); if (!f) return -1; if (find(b)) return -1; if (!same_dir(a, b)) return -1;
    MUTATE(strcpy(f->path, b)); return 0;
}
static int t_remove(void *c, const char *p) { (void)c; MUTATE({ file_t *f = find(p); if (f) f->used = 0; }); return 0; }
static int t_read(void *c, const char *p, char *buf, size_t cap) { (void)c; file_t *f = find(p); if (!f) return -1; size_t n = strlen(f->data); if (n > cap) n = cap; memcpy(buf, f->data, n); return (int)n; }
static int t_write(void *c, const char *p, const char *d, size_t n) {
    (void)c; MUTATE({ file_t *f = make(p); memcpy(f->data, d, n); f->data[n] = 0; }); return 0;
}
static int t_copy(void *c, const char *a, const char *b) {
    (void)c; file_t *f = find(a); if (!f) return -1;
    if (ops == crash_at && !crash_after) { /* power loss mid-copy leaves half a file */
        file_t *d = make(b); size_t n = strlen(f->data) / 2; memcpy(d->data, f->data, n); d->data[n] = 0; ++ops; longjmp(boom, 1);
    }
    MUTATE({ file_t *d = make(b); strcpy(d->data, f->data); }); return 0;
}
static int t_mkdirs(void *c, const char *p) { (void)c; (void)p; return 0; }
static const wili_swap_fs FS = {NULL, t_exists, t_rename, t_remove, t_read, t_write, t_mkdirs, t_copy};
#define R "ms0:/G"

static void setup(void) {
    memset(fsx, 0, sizeof fsx);
    put(R "/EBOOT.PBP", "old-eboot"); put(R "/a.prx", "old-a"); put(R "/data/x", "old-x");
    put(R "/settings.json", "user"); put(R "/a.prx.bak", "stale-bak");
    put(R "/pending/EBOOT.PBP", "new-eboot"); put(R "/pending/a.prx", "new-a"); put(R "/pending/data/y", "new-y");
    put(R "/pending/MANIFEST", "EBOOT.PBP\na.prx\ndata/y\n"); /* x is not in the new version's manifest */
}
static const char *get(const char *p) { file_t *f = find(p); return f ? f->data : NULL; }
static int same(const char *p, const char *v) { const char *g = get(p); return g && !strcmp(g, v); }

static void assert_old(void) {
    assert(same(R "/EBOOT.PBP", "old-eboot") && same(R "/a.prx", "old-a") && same(R "/data/x", "old-x"));
    assert(same(R "/settings.json", "user") && !find(R "/data/y"));
}
static void assert_new(void) {
    assert(same(R "/EBOOT.PBP", "new-eboot") && same(R "/a.prx", "new-a") && same(R "/data/y", "new-y"));
    assert(same(R "/data/x", "old-x") && same(R "/settings.json", "user"));
}
/* Runs fn until it finishes without a simulated power loss. */
static int guarded(int (*fn)(void)) {
    int crashed = 0;
    if (setjmp(boom)) { crashed = 1; crash_at = -1; return -100; }
    int r = fn(); (void)crashed; return r;
}
static int do_run(void) { return wili_swap_run(&FS, R); }
static int do_commit(void) { return wili_swap_commit(&FS, R); }

/* Scenario 1: update succeeds and the new client commits. Returns 1 if a crash was injected. */
static int scenario_success(int at, int after) {
    setup(); ops = 0; crash_at = at; crash_after = after;
    int crashed = 0, r;
    for (int guard = 0; guard < 8; ++guard) {
        r = guarded(do_run);
        if (r == -100) { crashed = 1; continue; }
        assert(r == WILI_SWAP_APPLIED);
        assert_new();
        break;
    }
    assert(r == WILI_SWAP_APPLIED);
    for (int guard = 0; guard < 8; ++guard) {
        r = guarded(do_commit);
        if (r == -100) { crashed = 1; continue; }
        assert(r == 0);
        break;
    }
    crash_at = -1;
    assert(do_run() == WILI_SWAP_NONE && !find(R "/pending/MANIFEST") && !find(R "/update.state"));
    assert_new();
    assert(same(R "/a.prx.bak", "old-a")); /* manual rollback copy kept; stale bak replaced */
    return crashed;
}
/* Scenario 2: client never commits; after the trial boots the old version returns. */
static int scenario_rollback(int at, int after) {
    setup(); ops = 0; crash_at = at; crash_after = after;
    int crashed = 0, applied = 0, r = 0;
    for (int guard = 0; guard < 20; ++guard) {
        r = guarded(do_run);
        if (r == -100) { crashed = 1; continue; }
        if (r == WILI_SWAP_APPLIED) { ++applied; assert_new(); continue; }
        break;
    }
    /* Power lost right after the final cleanup op: the next launch sees a finished rollback. */
    assert(r == WILI_SWAP_ROLLED_BACK || (crashed && r == WILI_SWAP_NONE));
    /* A power loss while counting a boot can swallow that boot, never add one. */
    assert(applied == WILI_SWAP_TRIAL_BOOTS || (crashed && applied == WILI_SWAP_TRIAL_BOOTS - 1));
    assert_old();
    assert(find(R "/update.failed") && !find(R "/pending/MANIFEST") && !find(R "/update.state"));
    crash_at = -1;
    assert(do_run() == WILI_SWAP_NONE); /* does not retry the same staging */
    assert_old();
    return crashed;
}

int main(void) {
    int points = 0;
    assert(wili_swap_path_ok("a/b.prx") && !wili_swap_path_ok("../x") && !wili_swap_path_ok("a/../b"));
    assert(!wili_swap_path_ok("/x") && !wili_swap_path_ok("a\\b") && !wili_swap_path_ok("c:x") && !wili_swap_path_ok(""));
    assert(!wili_swap_path_ok("settings.json") && !wili_swap_path_ok("a.bak") && !wili_swap_path_ok("update.state"));
    assert(!wili_swap_path_ok("pending/x") && !wili_swap_path_ok("a//b") && !wili_swap_path_ok(".hidden"));

    setup(); crash_at = -1; assert(wili_swap_run(&FS, R) == WILI_SWAP_APPLIED); assert_new();
    assert(wili_swap_commit(&FS, R) == 0);

    for (int after = 0; after <= 1; ++after)
        for (int at = 0;; ++at) {
            int c = scenario_success(at, after);
            if (!c) break;
            ++points;
        }
    for (int after = 0; after <= 1; ++after)
        for (int at = 0;; ++at) {
            int c = scenario_rollback(at, after);
            if (!c) break;
            ++points;
        }
    /* Staged file missing: rollback to old without ever reaching the trial. */
    setup(); crash_at = -1; ops = 0; find(R "/pending/a.prx")->used = 0;
    assert(wili_swap_run(&FS, R) == WILI_SWAP_ROLLED_BACK); assert_old();
    /* Hostile manifest is discarded without touching live files. */
    setup(); put(R "/pending/MANIFEST", "../evil\n");
    assert(wili_swap_run(&FS, R) == WILI_SWAP_NONE); assert_old(); assert(!find(R "/pending/MANIFEST"));
    /* Commit outside an update is a no-op. */
    setup(); find(R "/pending/MANIFEST")->used = 0; assert(wili_swap_commit(&FS, R) == 0);
    printf("update swap: %d crash points passed\n", points);
    return 0;
}
