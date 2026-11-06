/* The synthesized PSMF header against the fields of a real one (decoded from Sony's UMD Stream
 * Composer template; see decoder/src/psmf_header.h). */
#include <assert.h>
#include <stdio.h>
#include "../decoder/src/psmf_header.h"

static unsigned be(const uint8_t *p, int n) {
    unsigned v = 0;
    for (int i = 0; i < n; ++i) v = v << 8 | p[i];
    return v;
}

int main(void) {
    uint8_t h[PSMF_HEADER_SIZE];
    psmf_build_header(h, 480, 272, 12, 1);
    assert(!memcmp(h, "PSMF0012", 8));
    /* What PPSSPP checks: a nonzero offset that is a multiple of 2048. */
    assert(be(h + 0x08, 4) == 2048 && (be(h + 0x08, 4) & 2047) == 0);
    assert(be(h + 0x0c, 4) % 2048 == 0);
    /* Timestamps: 48-bit at 0x54 and 0x5A, repeated at 0x6E and 0x74. First is one second. */
    assert(be(h + 0x54, 2) == 0 && be(h + 0x56, 4) == 90000);
    assert(be(h + 0x5a, 2) == 0 && be(h + 0x5c, 4) == 0x7fffffffu);
    assert(!memcmp(h + 0x54, h + 0x6e, 6) && !memcmp(h + 0x5a, h + 0x74, 6));
    /* The fields a zeroed header would leave empty. */
    assert(be(h + 0x50, 4) == 0x4e && be(h + 0x60, 4) == 0x61a8 && be(h + 0x64, 4) == 90000);
    assert(h[0x68] == 2 && h[0x69] == 1 && be(h + 0x6a, 4) == 0x34);
    assert(be(h + 0x7a, 2) == 1 && be(h + 0x7c, 4) == 0x22);
    /* Two streams, 16 bytes each from 0x82: video then audio. Table length = 2 + 2 * 16 = 0x22. */
    assert(be(h + 0x80, 2) == 2 && be(h + 0x7c, 4) == 2 + 2 * 16);
    assert(h[0x82] == 0xe0 && h[0x83] == 0 && be(h + 0x84, 2) == 0x21ef);
    assert(h[0x8e] == 30 && h[0x8f] == 17); /* 480x272 in macroblocks, where PPSSPP reads it */
    assert(h[0x92] == 0xbd && be(h + 0x94, 2) == 0x2004 && h[0xa0] == 2 && h[0xa1] == 2);
    /* Nothing after the stream table. */
    for (int i = 0xa2; i < PSMF_HEADER_SIZE; ++i) assert(h[i] == 0);
    /* Sizes round up to whole macroblocks. */
    psmf_build_header(h, 480, 270, 12, 1);
    assert(h[0x8e] == 30 && h[0x8f] == 17);
    psmf_build_header(h, 384, 216, 12, 1);
    assert(h[0x8e] == 24 && h[0x8f] == 14);
    /* The minimal layout is exactly what the first working build wrote: version 0015, one video
     * stream, the two timestamps and the picture size, and nothing else. */
    psmf_build_header(h, 480, 272, 15, 0);
    assert(!memcmp(h, "PSMF0015", 8) && be(h + 8, 4) == 2048 && be(h + 0x56, 4) == 90000 &&
           be(h + 0x5c, 4) == 0x7fffffffu && be(h + 0x80, 2) == 1 && h[0x82] == 0xe0 &&
           h[0x8e] == 30 && h[0x8f] == 17);
    for (int i = 0; i < PSMF_HEADER_SIZE; ++i) {
        const int known = i < 16 || (i >= 0x56 && i < 0x5a) || (i >= 0x5c && i < 0x60) || i == 0x81 ||
                          i == 0x82 || i == 0x8e || i == 0x8f;
        if (!known) assert(h[i] == 0);
    }
    psmf_build_header(h, 480, 272, 13, 0);
    assert(!memcmp(h, "PSMF0013", 8));
    puts("psmf header: passed");
    return 0;
}
