#include "wili_update_swap.h"
#include "wili_update_swap_priv.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define MAX_ENTRIES 256
#define MANIFEST_CAP 16384

typedef struct {
    const wili_swap_fs *fs;
    const char *root;
    char names[MAX_ENTRIES][MAX_REL + 1];
    int count;
} swap_ctx;

static char manifest_buf[MANIFEST_CAP + 1];

/* <root>/update.log: what the loader did, rewritten after every line so the
 * last line survives a crash. Best effort; never affects the swap. */
static char log_buf[3072];
static size_t log_len;
static int log_loaded;
static void log_load(const swap_ctx *c) {
    char p[MAX_PATH];
    int n;
    if (log_loaded) return;
    log_loaded = 1;
    snprintf(p, sizeof p, "%s/update.log", c->root);
    n = c->fs->exists(c->fs->ctx, p) ? c->fs->read(c->fs->ctx, p, log_buf, sizeof log_buf - 1) : 0;
    log_len = n > 0 ? (size_t)n : 0;
}
static void slog(const swap_ctx *c, const char *fmt, ...) {
    char line[160], p[MAX_PATH];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n > sizeof line - 2) n = (int)sizeof line - 2;
    log_load(c);
    if (log_len + (size_t)n + 1 >= sizeof log_buf) { /* drop the oldest half, at a line start */
        size_t cut = log_len / 2;
        while (cut < log_len && log_buf[cut - 1] != '\n') ++cut;
        memmove(log_buf, log_buf + cut, log_len - cut);
        log_len -= cut;
    }
    memcpy(log_buf + log_len, line, (size_t)n);
    log_len += (size_t)n;
    log_buf[log_len++] = '\n';
    snprintf(p, sizeof p, "%s/update.log", c->root);
    c->fs->write(c->fs->ctx, p, log_buf, log_len);
}
/* <root>/update.notice: first line "applied" or "failed", then the last log lines.
 * The client shows it once after start-up and deletes it when the user dismisses it. */
static void write_notice(const swap_ctx *c, const char *kind) {
    char out[400], p[MAX_PATH];
    size_t start = log_len, lines = 0, n;
    log_load(c);
    start = log_len;
    while (start > 0 && lines < 4) { /* up to the last 4 lines */
        --start;
        if (start > 0 && log_buf[start - 1] == '\n') ++lines;
        else if (start == 0) ++lines;
    }
    if (log_len - start > 300) start = log_len - 300;
    n = (size_t)snprintf(out, sizeof out, "%s\n", kind);
    memcpy(out + n, log_buf + start, log_len - start);
    n += log_len - start;
    snprintf(p, sizeof p, "%s/update.notice", c->root);
    c->fs->write(c->fs->ctx, p, out, n);
}


static void path(const swap_ctx *c, char *out, const char *a, const char *b, const char *suffix) {
    snprintf(out, MAX_PATH, "%s/%s%s%s", c->root, a, b ? b : "", suffix ? suffix : "");
}
static void live(const swap_ctx *c, char *o, const char *rel) { path(c, o, rel, NULL, NULL); }
static void bak(const swap_ctx *c, char *o, const char *rel) { path(c, o, rel, NULL, ".bak"); }
static void pend(const swap_ctx *c, char *o, const char *rel) { path(c, o, "pending/", rel, NULL); }

static int load_manifest(swap_ctx *c) {
    char p[MAX_PATH];
    int n, i, eboot = -1;
    char *line, *end;
    snprintf(p, sizeof p, "%s/pending/MANIFEST", c->root);
    n = c->fs->read(c->fs->ctx, p, manifest_buf, MANIFEST_CAP);
    if (n <= 0) return -1;
    manifest_buf[n] = 0;
    c->count = 0;
    for (line = manifest_buf; *line;) {
        end = strchr(line, '\n');
        if (end) *end = 0;
        n = (int)strlen(line);
        if (n && line[n - 1] == '\r') line[--n] = 0;
        if (n) {
            if (!wili_swap_path_ok(line) && strcmp(line, "EBOOT.PBP") != 0) return -1;
            if (c->count >= MAX_ENTRIES) return -1;
            for (i = 0; i < c->count; ++i) if (strcmp(c->names[i], line) == 0) return -1;
            strcpy(c->names[c->count++], line);
        }
        if (!end) break;
        line = end + 1;
    }
    if (c->count == 0) return -1;
    /* EBOOT.PBP last: the loader cannot replace itself before everything else is in place. */
    for (i = 0; i < c->count; ++i) if (strcmp(c->names[i], "EBOOT.PBP") == 0) eboot = i;
    if (eboot >= 0 && eboot != c->count - 1) {
        char t[MAX_REL + 1];
        strcpy(t, c->names[eboot]);
        strcpy(c->names[eboot], c->names[c->count - 1]);
        strcpy(c->names[c->count - 1], t);
    }
    return 0;
}

/* ---- state ---- */


/* ---- phases ---- */
static int do_backup(const swap_ctx *c) {
    int i;
    char l[MAX_PATH], b[MAX_PATH];
    for (i = 0; i < c->count; ++i) {
        live(c, l, c->names[i]); bak(c, b, c->names[i]);
        if (c->fs->exists(c->fs->ctx, l) && !c->fs->exists(c->fs->ctx, b))
            if (c->fs->rename(c->fs->ctx, l, b) < 0) { slog(c, "backup failed: %s", c->names[i]); return -1; }
    }
    return 0;
}
static int do_install(const swap_ctx *c) {
    int i;
    char l[MAX_PATH], p[MAX_PATH];
    for (i = 0; i < c->count; ++i) {
        pend(c, p, c->names[i]); live(c, l, c->names[i]);
        if (!c->fs->exists(c->fs->ctx, p)) {
            if (c->fs->exists(c->fs->ctx, l)) continue; /* already moved */
            slog(c, "staged file missing: %s", c->names[i]);
            return -1;
        }
        if (c->fs->mkdirs(c->fs->ctx, l) < 0) { slog(c, "mkdir failed: %s", c->names[i]); return -1; }
        /* A partial copy from an earlier power loss may be there: start from nothing. */
        if (c->fs->exists(c->fs->ctx, l) && c->fs->remove(c->fs->ctx, l) < 0) { slog(c, "remove failed: %s", c->names[i]); return -1; }
        if (c->fs->copy(c->fs->ctx, p, l) < 0) {
            slog(c, "install copy failed: %s", c->names[i]);
            c->fs->remove(c->fs->ctx, l);
            return -1;
        }
        if (c->fs->remove(c->fs->ctx, p) < 0) { slog(c, "remove staged failed: %s", c->names[i]); return -1; }
    }
    return 0;
}
/* Pass 1 (full rollback only): remove files that did not exist before. After a
 * complete backup phase "no .bak" means exactly that. This must finish before
 * pass 2 starts restoring, or a restored file would look like a new one. */
static void rollback_remove_new(const swap_ctx *c) {
    int i;
    char l[MAX_PATH], b[MAX_PATH];
    for (i = 0; i < c->count; ++i) {
        live(c, l, c->names[i]); bak(c, b, c->names[i]);
        if (!c->fs->exists(c->fs->ctx, b)) c->fs->remove(c->fs->ctx, l);
    }
}
/* Pass 2: put the old files back and drop leftover staged files. */
static void rollback_restore(const swap_ctx *c) {
    int i;
    char l[MAX_PATH], b[MAX_PATH], p[MAX_PATH];
    for (i = 0; i < c->count; ++i) {
        live(c, l, c->names[i]); bak(c, b, c->names[i]); pend(c, p, c->names[i]);
        if (c->fs->exists(c->fs->ctx, b)) {
            if (c->fs->exists(c->fs->ctx, l)) c->fs->remove(c->fs->ctx, l);
            c->fs->rename(c->fs->ctx, b, l);
        }
        c->fs->remove(c->fs->ctx, p);
    }
}
static wili_swap_status fail_rollback(swap_ctx *c, phase_t from) {
    char f[MAX_PATH];
    if (from == S_BACKUP || from == S_ROLLBACK_B) {
        if (from == S_BACKUP && swap_write_state(c->fs, c->root, S_ROLLBACK_B, 0) < 0) return WILI_SWAP_ERROR;
    } else {
        if (from != S_ROLLBACK2) {
            if (from != S_ROLLBACK && swap_write_state(c->fs, c->root, S_ROLLBACK, 0) < 0) return WILI_SWAP_ERROR;
            rollback_remove_new(c);
            if (swap_write_state(c->fs, c->root, S_ROLLBACK2, 0) < 0) return WILI_SWAP_ERROR;
        }
    }
    rollback_restore(c);
    snprintf(f, sizeof f, "%s/update.failed", c->root);
    slog(c, "rolled back");
    c->fs->write(c->fs->ctx, f, "rolled back\n", 12);
    write_notice(c, "failed");
    swap_finish_files(c->fs, c->root);
    return WILI_SWAP_ROLLED_BACK;
}

wili_swap_status wili_swap_run(const wili_swap_fs *fs, const char *root) {
    static swap_ctx ctx; /* loader is single-threaded; keeps the 128 KB heap free */
    swap_ctx *c = &ctx;
    wili_swap_status r = WILI_SWAP_ERROR;
    state_t s;
    int i;
    char b[MAX_PATH], m[MAX_PATH];
    c->fs = fs; c->root = root; c->count = 0;
    log_loaded = 0; log_len = 0;
    s = swap_read_state(c->fs, c->root);
    snprintf(m, sizeof m, "%s/pending/MANIFEST", root);
    if (s.phase != S_NONE || fs->exists(fs->ctx, m)) slog(c, "swap: state=%d trial=%d manifest=%d", (int)s.phase, s.trial, fs->exists(fs->ctx, m));
    if (s.phase == S_NONE) {
        if (!fs->exists(fs->ctx, m)) { r = WILI_SWAP_NONE; goto out; }
        if (load_manifest(c) < 0) { /* unusable staging: discard it */
            slog(c, "manifest rejected (bad entry, >%d entries or >%d bytes); discarded", MAX_ENTRIES, MANIFEST_CAP);
            fs->remove(fs->ctx, m);
            r = WILI_SWAP_NONE; goto out;
        }
        /* Drop backups of the previous update before this one begins. */
        for (i = 0; i < c->count; ++i) { bak(c, b, c->names[i]); fs->remove(fs->ctx, b); }
        { char f[MAX_PATH]; snprintf(f, sizeof f, "%s/update.failed", root); fs->remove(fs->ctx, f); }
        if (swap_write_state(c->fs, c->root, S_BACKUP, 0) < 0) goto out;
        s.phase = S_BACKUP; s.trial = 0;
    } else if (s.phase == S_DONE) {
        swap_finish_files(c->fs, c->root);
        r = WILI_SWAP_NONE; goto out;
    } else if (load_manifest(c) < 0) {
        if (s.phase == S_ROLLBACK2 || s.phase == S_ROLLBACK_B) { /* power loss inside finish_files() */
            swap_finish_files(c->fs, c->root);
            r = WILI_SWAP_ROLLED_BACK;
        }
        goto out; /* state without a manifest cannot be acted on safely */
    }
    if (s.phase == S_BACKUP) {
        if (do_backup(c) < 0) { r = fail_rollback(c, S_BACKUP); goto out; }
        if (swap_write_state(c->fs, c->root, S_INSTALL, 0) < 0) goto out;
        s.phase = S_INSTALL;
    }
    if (s.phase == S_INSTALL) {
        if (do_install(c) < 0) { r = fail_rollback(c, S_INSTALL); goto out; }
        slog(c, "installed %d files; trial boot", c->count);
        write_notice(c, "applied"); /* before the state, so it exists whenever the state says installed */
        if (swap_write_state(c->fs, c->root, S_TRIAL, 0) < 0) goto out;
        s.phase = S_TRIAL; s.trial = 0;
    }
    if (s.phase == S_ROLLBACK || s.phase == S_ROLLBACK2 || s.phase == S_ROLLBACK_B) {
        r = fail_rollback(c, s.phase);
        goto out;
    }
    if (s.phase == S_TRIAL) {
        if (s.trial >= WILI_SWAP_TRIAL_BOOTS) { slog(c, "new version never committed; rolling back"); r = fail_rollback(c, S_TRIAL); goto out; }
        if (swap_write_state(c->fs, c->root, S_TRIAL, s.trial + 1) < 0) goto out;
        r = WILI_SWAP_APPLIED;
    }
out:
    return r;
}
