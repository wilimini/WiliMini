#ifndef PSP_LAB_VFPU_DSP_TEST_H
#define PSP_LAB_VFPU_DSP_TEST_H
#if PSP_DSP_SELFTEST
#include "h264qpel.h"
#include <stdio.h>
#include <string.h>
void __real_ff_h264qpel_init(H264QpelContext *, int);
static unsigned test_rng = 0x243f6a88u;
static unsigned test_random(void)
{
    test_rng ^= test_rng << 13;
    test_rng ^= test_rng >> 17;
    test_rng ^= test_rng << 5;
    return test_rng;
}
static unsigned vfpu_dsp_selftest(void)
{
    static uint8_t src[2048] __attribute__((aligned(16)));
    static uint8_t a[2048] __attribute__((aligned(16)));
    static uint8_t b[2048] __attribute__((aligned(16)));
    H264QpelContext scalar, candidate;
    __real_ff_h264qpel_init(&scalar, 8);
    ff_h264qpel_init(&candidate, 8);
    unsigned tests = 0, failures = 0;
    /* Both functions, every source byte alignment, four destination word
     * alignments, two strides, boundary bytes and random bytes. Guards outside
     * the 16x16 block must also remain identical. */
    for (int trial = 0; trial < 32; ++trial)
        for (int stride = 64; stride <= 80; stride += 16)
            for (int so = 0; so < 16; ++so)
                for (int offset = 0; offset < 16; offset += 4)
                    for (int average = 0; average < 2; ++average) {
                        for (unsigned i = 0; i < sizeof(src); ++i) {
                            src[i] = trial < 2 ? (trial ? 255 : 0) : test_random();
                            a[i] = trial < 2 ? (trial ? 0 : 255) : test_random();
                        }
                        memcpy(b, a, sizeof(a));
                        qpel_mc_func original = average ? scalar.avg_h264_qpel_pixels_tab[0][0] : scalar.put_h264_qpel_pixels_tab[0][0];
                        qpel_mc_func changed = average ? candidate.avg_h264_qpel_pixels_tab[0][0] : candidate.put_h264_qpel_pixels_tab[0][0];
                        original(a + 128 + offset, src + 128 + so, stride);
                        changed(b + 128 + offset, src + 128 + so, stride);
                        ++tests;
                        if (memcmp(a, b, sizeof(a))) ++failures;
                    }
    char path[192];
    snprintf(path, sizeof(path), "host0:/results/psp-vfpu-full/%s.json", PSP_BUILD_ID);
    FILE *out = fopen(path, "w");
    if (out) {
        fprintf(out, "{\"build\":\"%s\",\"tests\":%u,\"failures\":%u}\n", PSP_BUILD_ID, tests, failures);
        fclose(out);
    }
    printf("QPEL integer oracle tests=%u failures=%u\n", tests, failures);
    return failures;
}
#endif
#endif
