#include "wili_update_swap.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define MAX_ENTRIES 256
#define MAX_REL 100
#define MAX_PATH 224
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
static void slog(const swap_ctx *c, const char *fmt, ...) {
    char line[160], p[MAX_PATH];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n > sizeof line - 2) n = (int)sizeof line - 2;
    if (log_len + (size_t)n + 1 >= sizeof log_buf) log_len = 0; /* keep the newest lines */
    memcpy(log_buf + log_len, line, (size_t)n);
    log_len += (size_t)n;
    log_buf[log_len++] = '\n';
    snprintf(p, sizeof p, "%s/update.log", c->root);
    c->fs->write(c->fs->ctx, p, log_buf, log_len);
}

int wili_swap_path_ok(const char *rel) {
    size_t n = rel ? strlen(rel) : 0;
    const char *p;
    if (n == 0 || n > MAX_REL || rel[0] == '/' || rel[0] == '.') return 0;
    if (strchr(rel, '\\') || strchr(rel, ':')) return 0;
    for (p = rel; *p; ++p) if ((unsigned char)*p < 0x20) return 0;
    for (p = rel; *p;) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 0 || (len == 2 && p[0] == '.' && p[1] == '.')) return 0;
        if (!e) break;
        p = e + 1;
    }
    if (strcmp(rel, "settings.json") == 0) return 0; /* user data */
    if (strncmp(rel, "pending/", 8) == 0 || strncmp(rel, "update.", 7) == 0) return 0;
    if (n >= 4 && strcmp(rel + n - 4, ".bak") == 0) return 0;
    return 1;
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
typedef enum { S_NONE, S_BACKUP, S_INSTALL, S_TRIAL, S_ROLLBACK, S_ROLLBACK_B, S_DONE, S_ROLLBACK2 } phase_t;
typedef struct { phase_t phase; int trial; } state_t;

static int write_state(const swap_ctx *c, phase_t ph, int trial) {
    static const char *names[] = {"", "BACKUP", "INSTALL", "TRIAL", "ROLLBACK", "ROLLBACK_B", "DONE", "ROLLBACK2"};
    char text[40], st[MAX_PATH], tmp[MAX_PATH];
    int len = snprintf(text, sizeof text, "%s %d\n", names[ph], trial);
    snprintf(st, sizeof st, "%s/update.state", c->root);
    snprintf(tmp, sizeof tmp, "%s/update.state.tmp", c->root);
    /* Complete tmp first; a crash between remove and rename leaves a valid tmp. */
    if (c->fs->write(c->fs->ctx, tmp, text, (size_t)len) < 0) return -1;
    if (c->fs->remove(c->fs->ctx, st) < 0) return -1;
    return c->fs->rename(c->fs->ctx, tmp, st);
}
static int parse_state(const char *t, int n, state_t *s) {
    static const char *names[] = {"", "BACKUP", "INSTALL", "TRIAL", "ROLLBACK", "ROLLBACK_B", "DONE", "ROLLBACK2"};
    int ph, trial;
    char word[16];
    if (n < 3 || t[n - 1] != '\n') return 0;
    if (sscanf(t, "%15s %d", word, &trial) != 2 || trial < 0 || trial > 99) return 0;
    for (ph = 1; ph <= S_ROLLBACK2; ++ph) if (strcmp(word, names[ph]) == 0) { s->phase = (phase_t)ph; s->trial = trial; return 1; }
    return 0;
}
static state_t read_state(const swap_ctx *c) {
    state_t s = {S_NONE, 0};
    char st[MAX_PATH], tmp[MAX_PATH], buf[48];
    int n;
    snprintf(st, sizeof st, "%s/update.state", c->root);
    snprintf(tmp, sizeof tmp, "%s/update.state.tmp", c->root);
    if (c->fs->exists(c->fs->ctx, st)) {
        n = c->fs->read(c->fs->ctx, st, buf, sizeof buf - 1);
        if (n > 0 && parse_state(buf, n, &s)) return s;
        s.phase = S_NONE;
        return s;
    }
    if (c->fs->exists(c->fs->ctx, tmp)) {
        n = c->fs->read(c->fs->ctx, tmp, buf, sizeof buf - 1);
        if (n > 0 && parse_state(buf, n, &s)) return s;
        c->fs->remove(c->fs->ctx, tmp); /* torn first write: nothing has started */
        s.phase = S_NONE;
    }
    return s;
}

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
        if (c->fs->exists(c->fs->ctx, l) && c->fs->remove(c->fs->ctx, l) < 0) { slog(c, "remove failed: %s", c->names[i]); return -1; }
        if (c->fs->rename(c->fs->ctx, p, l) < 0) { slog(c, "install rename failed: %s", c->names[i]); return -1; }
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
static void finish_files(const swap_ctx *c) {
    char m[MAX_PATH], st[MAX_PATH], tmp[MAX_PATH];
    snprintf(m, sizeof m, "%s/pending/MANIFEST", c->root);
    snprintf(st, sizeof st, "%s/update.state", c->root);
    snprintf(tmp, sizeof tmp, "%s/update.state.tmp", c->root);
    c->fs->remove(c->fs->ctx, m);   /* manifest first: a leftover state alone is harmless */
    c->fs->remove(c->fs->ctx, tmp);
    c->fs->remove(c->fs->ctx, st);
}
static wili_swap_status fail_rollback(swap_ctx *c, phase_t from) {
    char f[MAX_PATH];
    if (from == S_BACKUP || from == S_ROLLBACK_B) {
        if (from == S_BACKUP && write_state(c, S_ROLLBACK_B, 0) < 0) return WILI_SWAP_ERROR;
    } else {
        if (from != S_ROLLBACK2) {
            if (from != S_ROLLBACK && write_state(c, S_ROLLBACK, 0) < 0) return WILI_SWAP_ERROR;
            rollback_remove_new(c);
            if (write_state(c, S_ROLLBACK2, 0) < 0) return WILI_SWAP_ERROR;
        }
    }
    rollback_restore(c);
    snprintf(f, sizeof f, "%s/update.failed", c->root);
    slog(c, "rolled back");
    c->fs->write(c->fs->ctx, f, "rolled back\n", 12);
    finish_files(c);
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
    s = read_state(c);
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
        if (write_state(c, S_BACKUP, 0) < 0) goto out;
        s.phase = S_BACKUP; s.trial = 0;
    } else if (s.phase == S_DONE) {
        finish_files(c);
        r = WILI_SWAP_NONE; goto out;
    } else if (load_manifest(c) < 0) {
        if (s.phase == S_ROLLBACK2 || s.phase == S_ROLLBACK_B) { /* power loss inside finish_files() */
            finish_files(c);
            r = WILI_SWAP_ROLLED_BACK;
        }
        goto out; /* state without a manifest cannot be acted on safely */
    }
    if (s.phase == S_BACKUP) {
        if (do_backup(c) < 0) { r = fail_rollback(c, S_BACKUP); goto out; }
        if (write_state(c, S_INSTALL, 0) < 0) goto out;
        s.phase = S_INSTALL;
    }
    if (s.phase == S_INSTALL) {
        if (do_install(c) < 0) { r = fail_rollback(c, S_INSTALL); goto out; }
        if (write_state(c, S_TRIAL, 0) < 0) goto out;
        slog(c, "installed %d files; trial boot", c->count);
        s.phase = S_TRIAL; s.trial = 0;
    }
    if (s.phase == S_ROLLBACK || s.phase == S_ROLLBACK2 || s.phase == S_ROLLBACK_B) {
        r = fail_rollback(c, s.phase);
        goto out;
    }
    if (s.phase == S_TRIAL) {
        if (s.trial >= WILI_SWAP_TRIAL_BOOTS) { slog(c, "new version never committed; rolling back"); r = fail_rollback(c, S_TRIAL); goto out; }
        if (write_state(c, S_TRIAL, s.trial + 1) < 0) goto out;
        r = WILI_SWAP_APPLIED;
    }
out:
    return r;
}

int wili_swap_commit(const wili_swap_fs *fs, const char *root) {
    swap_ctx c;
    state_t s;
    c.fs = fs; c.root = root; c.count = 0;
    s = read_state(&c);
    if (s.phase == S_NONE) return 0; /* nothing pending (e.g. dev build) */
    if (s.phase != S_TRIAL && s.phase != S_DONE) return -1;
    if (s.phase == S_TRIAL && write_state(&c, S_DONE, 0) < 0) return -1;
    finish_files(&c);
    return 0;
}
