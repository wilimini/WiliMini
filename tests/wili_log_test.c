#define _DEFAULT_SOURCE
#include "wili_log.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Capture stdout (the console sink) into a file so the line format is checkable. */
static char *capture(const char *path, void (*body)(void)) {
    static char text[8192];
    fflush(stdout);
    int saved = dup(1);
    FILE *f = freopen(path, "w", stdout);
    assert(f);
    body();
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    clearerr(stdout);
    FILE *in = fopen(path, "r");
    assert(in);
    size_t n = fread(text, 1, sizeof(text) - 1, in);
    text[n] = 0;
    fclose(in);
    return text;
}

static void default_levels(void) {
    WLOG_I(WLC_NET, "ssid=%s strength=%u", "home", 80u);
    WLOG_D(WLC_NET, "hidden by default");
    WLOG_V(WLC_NET, "compiled out");
    WLOG_E(WLC_MEM, "trailing newline is normalised\n");
}

static void configured(void) {
    WLOG_D(WLC_NET, "now visible");
    WLOG_I(WLC_PLAYER, "silenced");
    WLOG_W(WLC_PLAYER, "warn passes");
}

static void *worker(void *arg) {
    for (int i = 0; i < 500; ++i) WLOG_E(WLC_IO, "thread %d line %d", (int)(long)arg, i);
    return NULL;
}

static void threads(void) {
    pthread_t t[4];
    for (long i = 0; i < 4; ++i) pthread_create(&t[i], NULL, worker, (void *)i);
    for (int i = 0; i < 4; ++i) pthread_join(t[i], NULL);
}

int main(void) {
    char tmp[] = "/tmp/wili_log_test_XXXXXX";
    assert(mkdtemp(tmp));
    char path[256];
    snprintf(path, sizeof(path), "%s/out.txt", tmp);

    wili_log_init();
    assert(wili_log_ring_capacity() == 64);

    char *out = capture(path, default_levels);
    assert(strstr(out, " I NET ssid=home strength=80\n"));
    assert(strstr(out, " E MEM trailing newline is normalised\n"));
    assert(!strstr(out, "hidden by default"));
    assert(!strstr(out, "compiled out"));
    assert(out[0] == '[' && strstr(out, "] I NET"));
    assert(wili_log_ring_count() == 2);

    /* Levels, spec parsing and unknown items. */
    assert(wili_log_configure("warn,net=DEBUG,bogus=info,player=nope") == 2);
    assert(wili_log_levels[WLC_NET] == WILI_LOG_DEBUG);
    assert(wili_log_levels[WLC_PLAYER] == WILI_LOG_WARN);
    assert(wili_log_levels[WLC_MEM] == WILI_LOG_WARN);
    out = capture(path, configured);
    assert(strstr(out, " D NET now visible"));
    assert(!strstr(out, "silenced"));
    assert(strstr(out, " W PLAYER warn passes"));
    assert(wili_log_configure(NULL) == -1);
    WiliLogLevel level;
    assert(wili_log_parse_level("Verbose", &level) == 0 && level == WILI_LOG_VERBOSE);
    assert(wili_log_parse_level("loud", &level) != 0);

    /* Ring keeps only the newest lines, oldest first, and a dump matches. */
    wili_log_set_all(WILI_LOG_INFO);
    capture(path, threads);
    assert(wili_log_ring_count() == 64);
    snprintf(path, sizeof(path), "%s/dump.txt", tmp);
    FILE *dump = fopen(path, "w");
    assert(dump && wili_log_dump(dump) == 64);
    fclose(dump);
    dump = fopen(path, "r");
    char line[256];
    int lines = 0;
    while (fgets(line, sizeof(line), dump)) {
        assert(strstr(line, " E IO thread ") && line[strlen(line) - 1] == '\n');
        ++lines;
    }
    fclose(dump);
    assert(lines == 64);

    /* Long messages are truncated, not overflowed, and still end in one newline. */
    wili_log_set_all(WILI_LOG_INFO);
    char big[1000];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    snprintf(path, sizeof(path), "%s/long.txt", tmp);
    assert(freopen(path, "w", stdout));
    WLOG_I(WLC_CLIENT, "%s", big);
    fflush(stdout);
    FILE *lf = fopen(path, "r");
    size_t n = fread(line, 1, sizeof(line), lf);
    fclose(lf);
    assert(n == 192 - 1 && line[n - 1] == '\n');

    wili_log_shutdown();
    fprintf(stderr, "Leveled logger (format, filtering, spec, ring, threads, truncation): passed\n");
    return 0;
}
