#include "wili_update_swap.h"
#include "wili_update_swap_priv.h"
#include <stdio.h>
#include <string.h>

/* Parts shared by the loader (wili_update_swap_run.c) and the client, which only needs
 * wili_swap_path_ok() and wili_swap_commit(): no big buffers live here. */

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

int swap_write_state(const wili_swap_fs *fs, const char *root, phase_t ph, int trial) {
    static const char *names[] = {"", "BACKUP", "INSTALL", "TRIAL", "ROLLBACK", "ROLLBACK_B", "DONE", "ROLLBACK2"};
    char text[40], st[MAX_PATH], tmp[MAX_PATH];
    int len = snprintf(text, sizeof text, "%s %d\n", names[ph], trial);
    snprintf(st, sizeof st, "%s/update.state", root);
    snprintf(tmp, sizeof tmp, "%s/update.state.tmp", root);
    /* Complete tmp first; a crash between remove and rename leaves a valid tmp. */
    if (fs->write(fs->ctx, tmp, text, (size_t)len) < 0) return -1;
    if (fs->remove(fs->ctx, st) < 0) return -1;
    return fs->rename(fs->ctx, tmp, st);
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

state_t swap_read_state(const wili_swap_fs *fs, const char *root) {
    state_t s = {S_NONE, 0};
    char st[MAX_PATH], tmp[MAX_PATH], buf[48];
    int n;
    snprintf(st, sizeof st, "%s/update.state", root);
    snprintf(tmp, sizeof tmp, "%s/update.state.tmp", root);
    if (fs->exists(fs->ctx, st)) {
        n = fs->read(fs->ctx, st, buf, sizeof buf - 1);
        if (n > 0 && parse_state(buf, n, &s)) return s;
        s.phase = S_NONE;
        return s;
    }
    if (fs->exists(fs->ctx, tmp)) {
        n = fs->read(fs->ctx, tmp, buf, sizeof buf - 1);
        if (n > 0 && parse_state(buf, n, &s)) return s;
        fs->remove(fs->ctx, tmp); /* torn first write: nothing has started */
        s.phase = S_NONE;
    }
    return s;
}

void swap_finish_files(const wili_swap_fs *fs, const char *root) {
    char m[MAX_PATH], st[MAX_PATH], tmp[MAX_PATH];
    snprintf(m, sizeof m, "%s/pending/MANIFEST", root);
    snprintf(st, sizeof st, "%s/update.state", root);
    snprintf(tmp, sizeof tmp, "%s/update.state.tmp", root);
    fs->remove(fs->ctx, m);   /* manifest first: a leftover state alone is harmless */
    fs->remove(fs->ctx, tmp);
    fs->remove(fs->ctx, st);
}

int wili_swap_commit(const wili_swap_fs *fs, const char *root) {
    state_t s = swap_read_state(fs, root);
    if (s.phase == S_NONE) return 0; /* nothing pending (e.g. dev build) */
    if (s.phase != S_TRIAL && s.phase != S_DONE) return -1;
    if (s.phase == S_TRIAL && swap_write_state(fs, root, S_DONE, 0) < 0) return -1;
    swap_finish_files(fs, root);
    return 0;
}
