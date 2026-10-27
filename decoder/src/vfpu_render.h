#ifndef PSP_LAB_VFPU_RENDER_H
#define PSP_LAB_VFPU_RENDER_H
#include <stdint.h>
#include <string.h>
#if PSP_VFPU_RENDER
/* Private leaf routines: no intervening function calls. Entry thread needs
 * VFPU permission. Integer-valued FP32 color arithmetic stays below 2^24. */
static inline void vfpu_chroma_pair(const uint8_t *u, const uint8_t *v)
{
    uint16_t up, vp;
    memcpy(&up, u, 2);
    memcpy(&vp, v, 2);
    __asm__ volatile(
        "mtv %0, S010\n\tmtv %1, S020\n\t"
        "vc2i.s C010, S010\n\tvc2i.s C020, S020\n\t"
        "vi2f.q C010, C010[x,x,y,y], 24\n\t"
        "vi2f.q C020, C020[x,x,y,y], 24\n\t"
        "vscl.q C300, C020, S701\n\t"
        "vscl.q C310, C010, S702\n\t"
        "vscl.q C320, C020, S703\n\t"
        "vscl.q C330, C010, S710\n\t"
        "vadd.q C310, C310, C320\n\t"
        :: "r"((uint32_t)up ^ 0x8080u), "r"((uint32_t)vp ^ 0x8080u) : "memory");
}

static inline void vfpu_luma3(const uint8_t *src, uint32_t *dst)
{
    uint32_t yp;
    memcpy(&yp, src, 4);
    __asm__ volatile(
        "mtv %0, S000\n\tvc2i.s C000, S000\n\tvi2f.q C000, C000, 24\n\t"
        "vscl.q C000, C000, S700\n\tvadd.q C000, C000, C710[y,y,y,y]\n\t"
        "vadd.q C100, C300, C000\n\t"
        "vadd.q C110, C310, C000\n\t"
        "vadd.q C120, C330, C000\n\t"
        "vf2iz.q C100, C100, 15\n\t"
        "vf2iz.q C110, C110, 15\n\t"
        "vf2iz.q C120, C120, 15\n\t"
        "vi2uc.q S200, R100\n\tvi2uc.q S210, R101\n\tvi2uc.q S220, R102\n\t"
        "sv.s S200, 0(%1)\n\tsv.s S210, 4(%1)\n\tsv.s S220, 8(%1)\n\t"
        :: "r"(yp ^ 0x80808080u), "r"(dst) : "memory");
}

static void vfpu_draw_full(uint8_t *const planes[], const int strides[], uint32_t *dst)
{
    static const float constants[8] __attribute__((aligned(16))) = {
        298, 459, -55, -136, 541, 33504, 0, 0
    };
    __asm__ volatile(
        "lv.q C700, 0(%0)\n\tlv.q C710, 16(%0)\n\t"
        "vf2iz.q C130, C710[y,y,y,y], 23\n\t"
        :: "r"(constants) : "memory");
    for (int y = 0; y < 270; y += 3) {
        int sy = y * 4 / 3;
        const uint8_t *yl = planes[0] + sy * strides[0];
        const uint8_t *ul = planes[1] + (sy / 2) * strides[1];
        const uint8_t *vl = planes[2] + (sy / 2) * strides[2];
        uint32_t *line = dst + (y + 1) * 512;
        for (int g = 0; g < 160; ++g) {
            vfpu_chroma_pair(ul + g * 2, vl + g * 2);
            vfpu_luma3(yl + g * 4, line + g * 3);
            vfpu_luma3(yl + strides[0] + g * 4, line + 512 + g * 3);
        }
        ul += strides[1];
        vl += strides[2];
        for (int g = 0; g < 160; ++g) {
            vfpu_chroma_pair(ul + g * 2, vl + g * 2);
            vfpu_luma3(yl + strides[0] * 2 + g * 4, line + 1024 + g * 3);
        }
    }
}
#endif
#endif
