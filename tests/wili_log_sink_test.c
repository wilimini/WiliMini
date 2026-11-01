/* Built with -DWILI_LOG_SINK_FLUSH_MS=300 -DWILI_LOG_SINK_IDLE_MS=200 (see tools/test-logging.sh). */
#define _DEFAULT_SOURCE
#include "wili_log.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char dir[256];

static void msleep(int ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

static char *slurp(const char *name, char *buf, size_t size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = 0; return NULL; }
    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static int count_lines(const char *text, const char *needle) {
    int n = 0;
    for (const char *p = text; (p = strstr(p, needle)); p += strlen(needle)) ++n;
    return n;
}

static void path_of(const char *name, char *out, size_t size) { snprintf(out, size, "%s/%s", dir, name); }

struct burst { int id, lines; };
static void *burst_thread(void *arg) {
    struct burst *b = arg;
    for (int i = 0; i < b->lines; ++i) WLOG_I(WLC_IO, "burst t%d n%d", b->id, i);
    return NULL;
}

int main(void) {
    snprintf(dir, sizeof(dir), "/tmp/wili_sink_test_XXXXXX");
    assert(mkdtemp(dir));
    assert(freopen("/dev/null", "w", stdout)); /* console output is not under test here */
    static char text[1 << 20];
    char path[512];
    wili_log_init();
    wili_log_set_all(WILI_LOG_INFO);

    /* Off by default: no thread, no file, a clear return code. */
    path_of("off.log", path, sizeof(path));
    assert(wili_log_sink_start(WILI_LOG_SINK_SESSION, path, 0, 4096) == -2);
    assert(access(path, F_OK) != 0 && !wili_log_sink_active(WILI_LOG_SINK_SESSION));
    wili_log_sink_stop(WILI_LOG_SINK_SESSION); /* harmless when never started */
    wili_log_sink_shutdown();

    wili_log_set_files(1);
    path_of("bad.log", path, sizeof(path));
    char bad[512];
    snprintf(bad, sizeof(bad), "%s/missing-dir/x.log", dir);
    assert(wili_log_sink_start(WILI_LOG_SINK_SESSION, bad, 0, 0) == -1);
    assert(wili_log_sink_start((WiliLogSink)9, path, 0, 0) == -1);
    assert(wili_log_sink_start(WILI_LOG_SINK_RUN, NULL, 0, 0) == -1);

    /* Lines arrive in order, and stop() writes everything pending. */
    path_of("run.log", path, sizeof(path));
    const unsigned mark = wili_log_mark();
    WLOG_I(WLC_CLIENT, "before start");
    assert(wili_log_sink_start(WILI_LOG_SINK_RUN, path, mark, 0) == 0);
    assert(wili_log_sink_active(WILI_LOG_SINK_RUN));
    for (int i = 0; i < 10; ++i) WLOG_I(WLC_PLAYER, "ordered %d", i);
    wili_log_sink_stop(WILI_LOG_SINK_RUN);
    assert(!wili_log_sink_active(WILI_LOG_SINK_RUN));
    assert(slurp("run.log", text, sizeof(text)));
    assert(strstr(text, " I CLIENT before start\n"));            /* from_mark includes earlier lines */
    char *last = text;
    for (int i = 0; i < 10; ++i) {
        char want[40];
        snprintf(want, sizeof(want), " I PLAYER ordered %d\n", i);
        char *at = strstr(text, want);
        assert(at && at >= last);
        last = at;
    }
    assert(!strstr(text, "log_lines_dropped"));

    /* Restarting a run sink replaces the file and only carries lines after the new mark. */
    const unsigned mark2 = wili_log_mark();
    assert(wili_log_sink_start(WILI_LOG_SINK_RUN, path, mark2, 0) == 0);
    WLOG_I(WLC_PLAYER, "second run");
    wili_log_sink_stop(WILI_LOG_SINK_RUN);
    slurp("run.log", text, sizeof(text));
    assert(count_lines(text, "\n") == 1 && strstr(text, "second run") && !strstr(text, "ordered"));

    /* Writes are batched, but an ERROR goes out at once. */
    assert(wili_log_sink_start(WILI_LOG_SINK_RUN, path, wili_log_mark(), 0) == 0);
    WLOG_I(WLC_PLAYER, "batched info");
    WLOG_E(WLC_PLAYER, "urgent error");
    msleep(150); /* well under the 300 ms flush interval */
    slurp("run.log", text, sizeof(text));
    assert(strstr(text, "urgent error") && strstr(text, "batched info")); /* the error flushed everything before it */
    WLOG_I(WLC_PLAYER, "late info");
    msleep(900);
    slurp("run.log", text, sizeof(text));
    assert(strstr(text, "late info")); /* arrives without stop() after the interval */
    wili_log_sink_stop(WILI_LOG_SINK_RUN);

    /* Concurrent bursts: every line is either in the file or counted as dropped. */
    path_of("burst.log", path, sizeof(path));
    const unsigned burst_mark = wili_log_mark();
    assert(wili_log_sink_start(WILI_LOG_SINK_RUN, path, burst_mark, 0) == 0);
    pthread_t threads[4];
    struct burst bursts[4];
    for (int i = 0; i < 4; ++i) {
        bursts[i] = (struct burst){i, 3000};
        pthread_create(&threads[i], NULL, burst_thread, &bursts[i]);
    }
    for (int i = 0; i < 4; ++i) pthread_join(threads[i], NULL);
    wili_log_sink_stop(WILI_LOG_SINK_RUN);
    slurp("burst.log", text, sizeof(text));
    const int in_file = count_lines(text, " I IO burst t");
    int dropped = 0;
    for (char *p = text; (p = strstr(p, "log_lines_dropped=")); p += 18) dropped += atoi(p + 18);
    assert(in_file > 0 && in_file + dropped == 12000);
    for (char *p = text; *p; ) { /* no torn or merged lines */
        char *nl = strchr(p, '\n');
        assert(nl);
        if (strncmp(p, "log_lines_dropped=", 18) != 0) assert(p[0] == '[' && strstr(p, " I IO burst t") && strstr(p, " I IO burst t") < nl);
        p = nl + 1;
    }

    /* Rotation keeps path and path.1 and stays near the size limit. */
    path_of("session.log", path, sizeof(path));
    assert(wili_log_sink_start(WILI_LOG_SINK_SESSION, path, wili_log_mark(), 1500) == 0);
    for (int round = 0; round < 12; ++round) {
        for (int i = 0; i < 20; ++i) WLOG_I(WLC_NET, "rotating line %d %d", round, i);
        msleep(350);
    }
    wili_log_sink_stop(WILI_LOG_SINK_SESSION);
    struct stat st, st1;
    char old[560];
    snprintf(old, sizeof(old), "%s.1", path);
    assert(stat(path, &st) == 0 && stat(old, &st1) == 0);
    assert(st.st_size < 1500 + 400 && st1.st_size >= 1500 - 400);
    snprintf(old, sizeof(old), "%s.2", path);
    assert(access(old, F_OK) != 0);
    /* Reopening appends to the existing file instead of truncating it. */
    const long before = st.st_size;
    assert(wili_log_sink_start(WILI_LOG_SINK_SESSION, path, wili_log_mark(), 100000) == 0);
    WLOG_I(WLC_NET, "after reopen");
    wili_log_sink_stop(WILI_LOG_SINK_SESSION);
    assert(stat(path, &st) == 0 && st.st_size > before);

    /* Switching log files off closes the sink without an explicit stop. */
    path_of("toggle.log", path, sizeof(path));
    assert(wili_log_sink_start(WILI_LOG_SINK_SESSION, path, wili_log_mark(), 0) == 0);
    WLOG_I(WLC_NET, "before toggle");
    wili_log_set_files(0);
    for (int i = 0; i < 40 && wili_log_sink_active(WILI_LOG_SINK_SESSION); ++i) msleep(100);
    assert(!wili_log_sink_active(WILI_LOG_SINK_SESSION));
    assert(wili_log_sink_start(WILI_LOG_SINK_SESSION, path, 0, 0) == -2);

    /* The writer thread can be shut down and started again. */
    wili_log_sink_shutdown();
    wili_log_set_files(1);
    path_of("again.log", path, sizeof(path));
    assert(wili_log_sink_start(WILI_LOG_SINK_RUN, path, wili_log_mark(), 0) == 0);
    WLOG_I(WLC_NET, "after restart");
    wili_log_sink_shutdown(); /* writes pending lines and joins */
    slurp("again.log", text, sizeof(text));
    assert(strstr(text, "after restart"));
    assert(!wili_log_sink_active(WILI_LOG_SINK_RUN));

    wili_log_shutdown();
    fprintf(stderr, "Streaming log sink (order, marks, urgent, bursts, rotation, toggle, restart): passed\n");
    return 0;
}
