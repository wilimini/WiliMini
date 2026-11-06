/* The sceMpeg experiment switches (sce_mpeg.cfg). */
#include <assert.h>
#include <stdio.h>
#include "../decoder/src/sce_mpeg_options.h"

static int parse(const char *text, SceMpegOptions *o, int *rejected) {
    return sce_mpeg_parse_options(text, strlen(text), o, rejected);
}

int main(void) {
    /* The defaults are the setup that last produced pictures. */
    SceMpegOptions o = sce_mpeg_default_options();
    assert(o.psmf_version == 15 && !o.psmf_full && o.ring_packets == 256 && o.put_packets == 0 &&
           !o.register_atrac && !o.stop_flush && o.direct_feed == -1);
    int bad = -1;
    assert(parse("", &o, &bad) == 0 && bad == 0);
    assert(parse("psmf_version=12\nring_packets = 60\n  put_packets=32  \r\n# a comment\nregister_atrac=1\n"
                 "stop_flush=1\npsmf_full=1\ndirect_feed=0", &o, &bad) == 7 && bad == 0);
    assert(o.psmf_version == 12 && o.psmf_full == 1 && o.ring_packets == 60 && o.put_packets == 32 &&
           o.register_atrac == 1 && o.stop_flush == 1 && o.direct_feed == 0);
    assert(parse("direct_feed=1", &o, &bad) == 1 && o.direct_feed == 1);
    assert(parse("direct_feed=2\ndirect_feed=-1", &o, &bad) == 0 && bad == 2 && o.direct_feed == 1);
    /* Unknown keys, bad numbers and out-of-range values are skipped and counted; the rest still applies. */
    o = sce_mpeg_default_options();
    assert(parse("psmf_version=99\nring_packets=abc\nnonsense\nbogus=1\nput_packets=\nstop_flush=1\n"
                 "ring_packets=10\n", &o, &bad) == 1 && bad == 6);
    assert(o.stop_flush == 1 && o.psmf_version == 15 && o.ring_packets == 256);
    /* The last line needs no newline, and a key must match whole ("ring_packets2" is not a key). */
    o = sce_mpeg_default_options();
    assert(parse("ring_packets2=60\nput_packets=16", &o, &bad) == 1 && bad == 1 && o.put_packets == 16);
    assert(sce_mpeg_load_options("/nonexistent/sce_mpeg.cfg", &o, &bad) == -1 && bad == 0);
    puts("sce_mpeg options: passed");
    return 0;
}
