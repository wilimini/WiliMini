/* FFmpeg float DSP hooks. All vector inputs follow its 16-byte alignment
 * contract. VFPU state belongs to the calling PSP thread, never to the ME. */
#include <math.h>
#include <string.h>
#include <libavutil/float_dsp.h>
unsigned audio_vfpu_failures, audio_vfpu_calls;

static void mul(float *d, const float *a, const float *b, int n)
{
    ++audio_vfpu_calls;
    for (int i = 0; i < n; i += 4)
        __asm__ volatile("lv.q C000, 0(%1)\n\tlv.q C010, 0(%2)\n\t"
                         "vmul.q C020, C000, C010\n\tsv.q C020, 0(%0)"
                         :: "r"(d+i), "r"(a+i), "r"(b+i) : "memory");
}
static void scale(float *d, const float *a, float f, int n)
{
    ++audio_vfpu_calls;
    for (int i = 0; i < n; i += 4)
        __asm__ volatile("lv.s S030, 0(%2)\n\tlv.q C000, 0(%1)\n\t"
                         "vscl.q C020, C000, S030\n\tsv.q C020, 0(%0)"
                         :: "r"(d+i), "r"(a+i), "r"(&f) : "memory");
}
static void add(float *d, const float *a, const float *b, const float *c, int n)
{
    ++audio_vfpu_calls;
    for (int i = 0; i < n; i += 4)
        __asm__ volatile("lv.q C000, 0(%1)\n\tlv.q C010, 0(%2)\n\tlv.q C030, 0(%3)\n\t"
                         "vmul.q C020, C000, C010\n\tvadd.q C020, C020, C030\n\tsv.q C020, 0(%0)"
                         :: "r"(d+i), "r"(a+i), "r"(b+i), "r"(c+i) : "memory");
}
static void reverse(float *d, const float *a, const float *b, int n)
{
    ++audio_vfpu_calls;
    for (int i = 0; i < n; i += 4)
        __asm__ volatile("lv.q C000, 0(%1)\n\tlv.q C010, 0(%2)\n\t"
                         "vmul.q C020, C000, C010[w,z,y,x]\n\tsv.q C020, 0(%0)"
                         :: "r"(d+i), "r"(a+i), "r"(b+n-i-4) : "memory");
}
static void window(float *d, const float *a, const float *b, const float *w, int n)
{
    ++audio_vfpu_calls;
    for (int i = 0; i < n; i += 4) {
        __asm__ volatile(
            "lv.q C000, 0(%2)\n\tlv.q C010, 0(%3)\n\tvmov.q C010, C010[w,z,y,x]\n\t"
            "lv.q C020, 0(%4)\n\tlv.q C030, 0(%5)\n\tvmov.q C030, C030[w,z,y,x]\n\t"
            "vmul.q C100, C000, C030\n\tvmul.q C110, C010, C020\n\t"
            "vsub.q C120, C100, C110\n\tsv.q C120, 0(%0)\n\t"
            "vmul.q C100, C000, C020\n\tvmul.q C110, C010, C030\n\t"
            "vadd.q C120, C100, C110\n\tvmov.q C120, C120[w,z,y,x]\n\tsv.q C120, 0(%1)"
            :: "r"(d+i), "r"(d+2*n-i-4), "r"(a+i), "r"(b+n-i-4),
               "r"(w+i), "r"(w+2*n-i-4) : "memory");
    }
}
static unsigned compare(const float *a, const float *b, int n)
{
    unsigned failures = 0;
    for (int i = 0; i < n; ++i)
        if (!isfinite(a[i]) || fabsf(a[i] - b[i]) > 0.000002f * (1.0f + fabsf(b[i])))
            ++failures;
    return failures;
}
static void selftest(const AVFloatDSPContext *c)
{
    float a[1024] __attribute__((aligned(64)));
    float b[1024] __attribute__((aligned(64)));
    float w[2048] __attribute__((aligned(64)));
    float d[2048] __attribute__((aligned(64)));
    float ref[2048] __attribute__((aligned(64)));
    for (int i = 0; i < 1024; ++i) {
        a[i] = (i % 61 - 30) * 0.03125f;
        b[i] = (i % 47 - 23) * 0.0625f;
    }
    for (int i = 0; i < 2048; ++i) w[i] = (i % 97 - 48) * 0.015625f;
    const int sizes[] = {4, 16, 60, 64, 120, 128, 480, 512, 1024};
    for (unsigned k = 0; k < sizeof(sizes)/sizeof(sizes[0]); ++k) {
        int n = sizes[k];
        mul(d, a, b, n); c->vector_fmul(ref, a, b, n);
        audio_vfpu_failures += compare(d, ref, n);
        scale(d, a, -0.3125f, n); c->vector_fmul_scalar(ref, a, -0.3125f, n);
        audio_vfpu_failures += compare(d, ref, n);
        reverse(d, a, b, n); c->vector_fmul_reverse(ref, a, b, n);
        audio_vfpu_failures += compare(d, ref, n);
        add(d, a, b, w, n); c->vector_fmul_add(ref, a, b, w, n);
        audio_vfpu_failures += compare(d, ref, n);
        window(d, a, b, w, n); c->vector_fmul_window(ref, a, b, w, n);
        audio_vfpu_failures += compare(d, ref, 2*n);
        memcpy(d, a, n*sizeof(float)); memcpy(ref, a, n*sizeof(float));
        mul(d, d, b, n); c->vector_fmul(ref, ref, b, n);
        audio_vfpu_failures += compare(d, ref, n);
        memcpy(d, a, n*sizeof(float)); memcpy(ref, a, n*sizeof(float));
        scale(d, d, 0.3125f, n); c->vector_fmul_scalar(ref, ref, 0.3125f, n);
        audio_vfpu_failures += compare(d, ref, n);
    }
}
AVFloatDSPContext *__real_avpriv_float_dsp_alloc(int);
AVFloatDSPContext *__wrap_avpriv_float_dsp_alloc(int exact)
{
    AVFloatDSPContext *c = __real_avpriv_float_dsp_alloc(exact);
    static int tested;
    if (!c || exact) return c;
    if (!tested) { selftest(c); tested = 1; audio_vfpu_calls = 0; }
    if (audio_vfpu_failures) return c;
    c->vector_fmul = mul;
    c->vector_fmul_scalar = scale;
    c->vector_fmul_add = add;
    c->vector_fmul_reverse = reverse;
    c->vector_fmul_window = window;
    return c;
}
