/* Runs the real swap against a temp directory through the POSIX binding. */
#define _POSIX_C_SOURCE 200809L
#include "wili_update_swap_posix.h"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

static void put(const char *root, const char *rel, const char *d) {
    char p[300]; snprintf(p, sizeof p, "%s/%s", root, rel);
    assert(wili_swap_px_mkdirs(NULL, p) == 0);
    assert(wili_swap_px_write(NULL, p, d, strlen(d)) == 0);
}
static void expect(const char *root, const char *rel, const char *d) {
    char p[300], b[64]; snprintf(p, sizeof p, "%s/%s", root, rel);
    int n = wili_swap_px_read(NULL, p, b, sizeof b - 1);
    assert(n >= 0); b[n] = 0; assert(strcmp(b, d) == 0);
}
int main(void) {
    char root[] = "/tmp/wili-swap-XXXXXX";
    if (!mkdtemp(root)) return 1;
    put(root, "EBOOT.PBP", "old"); put(root, "lib/a.prx", "old-a"); put(root, "settings.json", "user");
    put(root, "pending/EBOOT.PBP", "new"); put(root, "pending/lib/a.prx", "new-a"); put(root, "pending/lib/sub/n.bin", "new-n");
    put(root, "pending/MANIFEST", "EBOOT.PBP\nlib/a.prx\nlib/sub/n.bin\n");
    assert(wili_swap_run(&wili_swap_posix_fs, root) == WILI_SWAP_APPLIED);
    expect(root, "EBOOT.PBP", "new"); expect(root, "lib/a.prx", "new-a"); expect(root, "lib/sub/n.bin", "new-n");
    expect(root, "settings.json", "user"); expect(root, "lib/a.prx.bak", "old-a");
    assert(wili_swap_commit(&wili_swap_posix_fs, root) == 0);
    assert(wili_swap_run(&wili_swap_posix_fs, root) == WILI_SWAP_NONE);
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", root); assert(system(cmd) == 0);
    puts("update swap (posix binding): passed");
    return 0;
}
