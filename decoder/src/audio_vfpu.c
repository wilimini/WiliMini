/* FFmpeg float DSP hooks. All vector inputs follow its 16-byte alignment
 * contract. VFPU state belongs to the calling PSP thread, never to the ME. */
#include <libavutil/float_dsp.h>

static void mul(float *d, const float *a, const float *b, int n)
{
    for (int i = 0; i < n; i += 4)
        __asm__ volatile("lv.q C000, 0(%1)\n\tlv.q C010, 0(%2)\n\t"
                         "vmul.q C020, C000, C010\n\tsv.q C020, 0(%0)"
                         :: "r"(d+i), "r"(a+i), "r"(b+i) : "memory");
}
static void scale(float *d, const float *a, float f, int n)
{
    for (int i = 0; i < n; i += 4)
        __asm__ volatile("lv.s S030, 0(%2)\n\tlv.q C000, 0(%1)\n\t"
                         "vscl.q C020, C000, S030\n\tsv.q C020, 0(%0)"
                         :: "r"(d+i), "r"(a+i), "r"(&f) : "memory");
}
static void add(float *d, const float *a, const float *b, const float *c, int n)
{
    for (int i = 0; i < n; i += 4)
        __asm__ volatile("lv.q C000, 0(%1)\n\tlv.q C010, 0(%2)\n\tlv.q C030, 0(%3)\n\t"
                         "vmul.q C020, C000, C010\n\tvadd.q C020, C020, C030\n\tsv.q C020, 0(%0)"
                         :: "r"(d+i), "r"(a+i), "r"(b+i), "r"(c+i) : "memory");
}
static void reverse(float *d, const float *a, const float *b, int n)
{
    for (int i = 0; i < n; i += 4)
        __asm__ volatile("lv.q C000, 0(%1)\n\tlv.q C010, 0(%2)\n\t"
                         "vmul.q C020, C000, C010[w,z,y,x]\n\tsv.q C020, 0(%0)"
                         :: "r"(d+i), "r"(a+i), "r"(b+n-i-4) : "memory");
}
static void window(float *d, const float *a, const float *b, const float *w, int n)
{
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
AVFloatDSPContext *__real_avpriv_float_dsp_alloc(int);
AVFloatDSPContext *__wrap_avpriv_float_dsp_alloc(int exact)
{
    AVFloatDSPContext *c = __real_avpriv_float_dsp_alloc(exact);
    if (!c || exact) return c;
    c->vector_fmul = mul;
    c->vector_fmul_scalar = scale;
    c->vector_fmul_add = add;
    c->vector_fmul_reverse = reverse;
    c->vector_fmul_window = window;
    return c;
}
