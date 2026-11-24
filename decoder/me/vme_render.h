/* Experimental VME2 renderer (480x270 RGB565) and exact H.264 half-pel
 * precomputer (640xH P20/P02/P22). VME API/opcode definitions are by
 * mcidclan, MIT licensed; see third_party/psp-media-engine-custom-core/LICENSE.
 * Called exclusively from the ME worker under Custom Core. */
#ifndef WILI_VME_RENDER_H
#define WILI_VME_RENDER_H

#include <stdint.h>
#include <string.h>
#include <me-core-mapper/me-core-mapper.h>
#include <me-core-mapper/vme-lib.h>
#include <me-core-mapper/vme-fu-opcodes.h>

#define VME_HP_W           640
#define VME_HP_MAX_H       368
#define VME_HP_Y16_STRIDE  656 /* >= 645, multiple of 16 */

/* ME eDRAM scratch region (offset +64 KiB above 0x40000000 to stay clear of
 * low eDRAM vectors; ~1.05 MiB total, well below ME stack at top of eDRAM) */
#define VME_EDRAM_Y16      0x40010000u /* 656 * 373 * 2 = 489,376 B */
#define VME_EDRAM_HRAW16   0x40090000u /* 640 * 373 * 2 = 477,440 B */
#define VME_EDRAM_OUT16    0x40110000u /* 2-row ring: 2 * 640 * 2 = 2,560 B */

typedef struct __attribute__((aligned(64))) {
    uint8_t *p20;
    uint8_t *p02;
    uint8_t *p22;
} VmeHalfpelJob;

static volatile int me_vme_wiped = 0;

static uint32_t vme_ctx_csc480[2][3][VME_CONTEXT_WORD_COUNT] __attribute__((aligned(64)));
static uint32_t vme_ctx_h1[VME_CONTEXT_WORD_COUNT] __attribute__((aligned(64)));
static uint32_t vme_ctx_h2[VME_CONTEXT_WORD_COUNT] __attribute__((aligned(64)));
static uint32_t vme_ctx_p02[6][VME_CONTEXT_WORD_COUNT] __attribute__((aligned(64)));
static uint32_t vme_ctx_p22[6][VME_CONTEXT_WORD_COUNT] __attribute__((aligned(64)));

static const int vme_slot_woff[6] = {
    VME_TOP_BUFF0_WOFF, VME_TOP_BUFF1_WOFF, VME_TOP_BUFF2_WOFF,
    VME_TOP_BUFF3_WOFF, VME_BASE_BUFF0_WOFF, VME_BASE_BUFF1_WOFF
};

static void vme_build_csc_ctxs_half(uint32_t ctxs[3][VME_CONTEXT_WORD_COUNT], int n, uint32_t top_off)
{
    /* Pass 0: Y/U/V -> r_raw, b_raw, g_part */
    {
        memset(ctxs[0], 0, VME_CONTEXT_WORD_COUNT * 4);
        volatile u32 VME_BASE_ADDR = (u32)ctxs[0];
        vme_pe0(agu_top(MODE), VME_DEF_MODE, top_off);
        vme_pe0(agu_top(COUNT), VME_DEF_STEP, (uint32_t)(n - 1));
        vme_icn(AGU_TOP, 0x40004000u);
        vme_icn(AGU_BASE, 0x66666666u);
        vme_pe2(vme_fu(PRIMARY), vme_mux(NONE, TOP_0), VME_FU_OPCODE_MUL_CONST_RSHIFT_0);
        vme_pe2(fu_reg(PRIMARY, B), 298);
        vme_pe0(vme_fu(PRIMARY), vme_mux(NONE, TOP_2), VME_FU_OPCODE_MUL_CONST_RSHIFT_0);
        vme_pe0(fu_reg(PRIMARY, B), 459);
        vme_pe0(vme_fu(SECONDARY), vme_mux(STAGING_2, STAGING_0), fu_code(OP_ADD_OFA));
        vme_pe0(fu_reg(SECONDARY, A), (uint32_t)(-63392) & 0xffffffu);
        vme_pe1(vme_fu(PRIMARY), vme_mux(NONE, TOP_1), VME_FU_OPCODE_MUL_CONST_RSHIFT_0);
        vme_pe1(fu_reg(PRIMARY, B), 541);
        vme_pe1(vme_fu(SECONDARY), vme_mux(STAGING_2, STAGING_1), fu_code(OP_ADD_OFA));
        vme_pe1(fu_reg(SECONDARY, A), (uint32_t)(-73888) & 0xffffffu);
        vme_pe3(vme_fu(PRIMARY), vme_mux(NONE, TOP_1), VME_FU_OPCODE_MUL_CONST_RSHIFT_0);
        vme_pe3(fu_reg(PRIMARY, B), 55);
        vme_pe3(vme_fu(SECONDARY), vme_mux(STAGING_2, STAGING_3), fu_code(OP_SUB_INV_OFA));
        vme_pe3(fu_reg(SECONDARY, A), 19808);
        vme_pe0(agu_write(MODE), VME_DEF_MODE, vme_cyc(9));
        vme_pe0(agu_write(COUNT), VME_DEF_STEP, (uint32_t)(n - 1));
        vme_pe1(agu_write(MODE), VME_DEF_MODE, vme_cyc(9));
        vme_pe1(agu_write(COUNT), VME_DEF_STEP, (uint32_t)(n - 1));
        vme_pe3(agu_write(MODE), VME_DEF_MODE, vme_cyc(9));
        vme_pe3(agu_write(COUNT), VME_DEF_STEP, (uint32_t)(n - 1));
        vme_icn(AGU_WRITE, 0x34103410u);
        vme_icn(SWEN, 0xD0000000u);
    }
    /* Pass 1: shift + clamp -> r5, b5, g6 at BASE_0/1/3[+0x400] */
    {
        memset(ctxs[1], 0, VME_CONTEXT_WORD_COUNT * 4);
        volatile u32 VME_BASE_ADDR = (u32)ctxs[1];
        vme_pe0(agu_top(MODE), VME_DEF_MODE, top_off);
        vme_pe0(agu_top(COUNT), VME_DEF_STEP, (uint32_t)(n - 1));
        vme_icn(AGU_TOP, 0x40444044u);
        vme_pe0(agu_base(MODE), VME_DEF_MODE);
        vme_pe0(agu_base(COUNT), VME_DEF_STEP, (uint32_t)(n - 1));
        vme_pe1(agu_base(MODE), VME_DEF_MODE, vme_cyc(3));
        vme_pe1(agu_base(COUNT), VME_DEF_STEP, (uint32_t)(n - 1));
        vme_icn(AGU_BASE, 0x16001600u);
        vme_pe0(vme_fu(PRIMARY), vme_mux(NONE, BASE_0), (0x014u << 12), 11);
        vme_pe0(vme_fu(SECONDARY), vme_mux(NONE, STAGING_0), fu_code(OP_CLP));
        vme_pe0(fu_reg(SECONDARY, A), 31); vme_pe0(fu_reg(SECONDARY, B), 0);
        vme_pe1(vme_fu(PRIMARY), vme_mux(NONE, BASE_1), (0x014u << 12), 11);
        vme_pe1(vme_fu(SECONDARY), vme_mux(NONE, STAGING_1), fu_code(OP_CLP));
        vme_pe1(fu_reg(SECONDARY, A), 31); vme_pe1(fu_reg(SECONDARY, B), 0);
        vme_pe2(vme_fu(PRIMARY), vme_mux(NONE, TOP_2), VME_FU_OPCODE_MUL_CONST_RSHIFT_0);
        vme_pe2(fu_reg(PRIMARY, B), 136);
        vme_pe2(vme_fu(SECONDARY), vme_mux(BASE_3, STAGING_2), fu_code(OP_SUB), 10);
        vme_pe3(vme_fu(PRIMARY), vme_mux(NONE, STAGING_6), fu_code(OP_CLP));
        vme_pe3(fu_reg(PRIMARY, A), 63); vme_pe3(fu_reg(PRIMARY, B), 0);
        vme_pe0(agu_write(MODE), VME_DEF_MODE, vme_cyc(9), 0x400);
        vme_pe0(agu_write(COUNT), VME_DEF_STEP, (uint32_t)(n - 1));
        vme_pe1(agu_write(MODE), VME_DEF_MODE, vme_cyc(9), 0x400);
        vme_pe1(agu_write(COUNT), VME_DEF_STEP, (uint32_t)(n - 1));
        vme_pe3(agu_write(MODE), VME_DEF_MODE, vme_cyc(12), 0x400);
        vme_pe3(agu_write(COUNT), VME_DEF_STEP, (uint32_t)(n - 1));
        vme_icn(AGU_WRITE, 0x34103410u);
        vme_icn(SWEN, 0xC0000000u);
    }
    /* Pass 2: pack RGB565 = (b5 << 11) + (g6 << 5) + r5 -> BASE_2[0..n-1] */
    {
        memset(ctxs[2], 0, VME_CONTEXT_WORD_COUNT * 4);
        volatile u32 VME_BASE_ADDR = (u32)ctxs[2];
        vme_pe0(agu_base(MODE), VME_DEF_MODE, 0x400);
        vme_pe0(agu_base(COUNT), VME_DEF_STEP, (uint32_t)(n - 1));
        vme_pe1(agu_base(MODE), VME_DEF_MODE, vme_cyc(6), 0x400);
        vme_pe1(agu_base(COUNT), VME_DEF_STEP, (uint32_t)(n - 1));
        vme_icn(AGU_TOP, 0x44444444u);
        vme_icn(AGU_BASE, 0x06010601u);
        vme_pe0(vme_fu(PRIMARY), vme_mux(NONE, BASE_1), VME_FU_OPCODE_MUL_CONST_RSHIFT_0);
        vme_pe0(fu_reg(PRIMARY, B), 2048);
        vme_pe1(vme_fu(PRIMARY), vme_mux(NONE, BASE_3), VME_FU_OPCODE_MUL_CONST_RSHIFT_0);
        vme_pe1(fu_reg(PRIMARY, B), 32);
        vme_pe2(vme_fu(PRIMARY), vme_mux(STAGING_0, STAGING_1), fu_code(OP_ADD));
        vme_pe2(vme_fu(SECONDARY), vme_mux(STAGING_2, BASE_0), fu_code(OP_ADD));
        vme_pe2(agu_write(MODE), VME_DEF_MODE, vme_cyc(12));
        vme_pe2(agu_write(COUNT), VME_DEF_STEP, (uint32_t)(n - 1));
        vme_icn(AGU_WRITE, 0x42444244u);
        vme_icn(SWEN, 0x20000000u);
    }
}

static void vme_build_horiz_ctxs(uint32_t *c1, uint32_t *c2, int w)
{
    /* Pass 1: ALL MULs on PRIMARY! PE1_SEC = S0 + TOP_0 -> S5; PE2_PRI = 5 * S5 -> BASE_2 */
    {
        memset(c1, 0, VME_CONTEXT_WORD_COUNT * 4);
        volatile u32 VME_BASE_ADDR = (u32)c1;
        vme_pe0(agu_top(MODE), VME_DEF_MODE);
        vme_pe0(agu_top(COUNT), VME_DEF_STEP, (uint32_t)(w + 4));
        vme_icn(AGU_TOP, 0x44404440u);
        vme_icn(AGU_BASE, 0x66666666u);
        vme_pe0(vme_fu(PRIMARY), vme_mux(NONE, TOP_0), fu_code(OP_BAK));
        vme_pe1(vme_fu(PRIMARY), vme_mux(NONE, TOP_0), fu_code(OP_BAK));
        vme_pe1(vme_fu(SECONDARY), vme_mux(STAGING_0, TOP_0), fu_code(OP_ADD));
        vme_pe2(vme_fu(PRIMARY), vme_mux(NONE, STAGING_5), VME_FU_OPCODE_MUL_CONST_RSHIFT_0);
        vme_pe2(fu_reg(PRIMARY, B), 5);
        vme_pe3(vme_fu(PRIMARY), vme_mux(NONE, TOP_0), fu_code(OP_BAK));
        vme_pe0(agu_write(MODE), VME_DEF_MODE, vme_cyc(6));
        vme_pe0(agu_write(COUNT), VME_DEF_STEP, (uint32_t)(w + 4));
        vme_pe1(agu_write(MODE), VME_DEF_MODE, vme_cyc(6));
        vme_pe1(agu_write(COUNT), VME_DEF_STEP, (uint32_t)(w + 4));
        vme_pe2(agu_write(MODE), VME_DEF_MODE, vme_cyc(13));
        vme_pe2(agu_write(COUNT), VME_DEF_STEP, (uint32_t)(w - 1));
        vme_pe3(agu_write(MODE), VME_DEF_MODE, vme_cyc(6));
        vme_pe3(agu_write(COUNT), VME_DEF_STEP, (uint32_t)(w + 4));
        vme_icn(AGU_WRITE, 0x32103210u);
        vme_icn(SWEN, 0x00000000u);
    }
    /* Pass 2: ALL MULs on PRIMARY! Outputs BOTH H_raw+4096 (BASE_2[+0x400]) AND P20 (BASE_3[+0x400]) */
    {
        memset(c2, 0, VME_CONTEXT_WORD_COUNT * 4);
        volatile u32 VME_BASE_ADDR = (u32)c2;
        vme_pe0(agu_top(MODE), VME_DEF_MODE, vme_cyc(3));
        vme_pe0(agu_top(COUNT), VME_DEF_STEP, (uint32_t)(w - 1));
        vme_pe0(agu_base(MODE), VME_DEF_MODE, vme_cyc(3), 5);
        vme_pe0(agu_base(COUNT), VME_DEF_STEP, (uint32_t)(w - 1));
        vme_pe1(agu_base(MODE), VME_DEF_MODE, 2);
        vme_pe1(agu_base(COUNT), VME_DEF_STEP, (uint32_t)(w - 1));
        vme_pe2(agu_base(MODE), VME_DEF_MODE, 3);
        vme_pe2(agu_base(COUNT), VME_DEF_STEP, (uint32_t)(w - 1));
        vme_pe3(agu_base(MODE), VME_DEF_MODE, vme_cyc(6));
        vme_pe3(agu_base(COUNT), VME_DEF_STEP, (uint32_t)(w - 1));
        vme_icn(AGU_TOP, 0x44404440u);
        vme_icn(AGU_BASE, 0x23102310u);
        vme_pe0(vme_fu(PRIMARY), vme_mux(BASE_1, BASE_3), fu_code(OP_ADD));
        vme_pe0(vme_fu(SECONDARY), vme_mux(TOP_0, BASE_0), fu_code(OP_ADD));
        vme_pe1(vme_fu(PRIMARY), vme_mux(NONE, STAGING_0), VME_FU_OPCODE_MUL_CONST_RSHIFT_0);
        vme_pe1(fu_reg(PRIMARY, B), 20);
        vme_pe1(vme_fu(SECONDARY), vme_mux(NONE, STAGING_4), fu_code(OP_BAK));
        vme_pe2(vme_fu(PRIMARY), vme_mux(STAGING_1, BASE_2), fu_code(OP_SUB));
        vme_pe2(vme_fu(SECONDARY), vme_mux(STAGING_2, STAGING_5), fu_code(OP_ADD_OFA));
        vme_pe2(fu_reg(SECONDARY, A), 4096);
        vme_pe3(vme_fu(PRIMARY), vme_mux(STAGING_2, STAGING_5), fu_code(OP_ADD), 5, (1u << 6));
        vme_pe3(vme_fu(SECONDARY), vme_mux(NONE, STAGING_3), fu_code(OP_CLP));
        vme_pe3(fu_reg(SECONDARY, A), 255);
        vme_pe3(fu_reg(SECONDARY, B), 0);
        vme_pe2(agu_write(MODE), VME_DEF_MODE, vme_cyc(15), 0x400);
        vme_pe2(agu_write(COUNT), VME_DEF_STEP, (uint32_t)(w - 1));
        vme_pe3(agu_write(MODE), VME_DEF_MODE, vme_cyc(18), 0x400);
        vme_pe3(agu_write(COUNT), VME_DEF_STEP, (uint32_t)(w - 1));
        vme_icn(AGU_WRITE, 0x32443244u);
        vme_icn(SWEN, 0x30000000u);
    }
}

static void vme_build_fir6_ctx(uint32_t *ctx, int rot, int count, int shift, int post_sub)
{
    static const uint32_t FRONT_MUX[6] = {
        VME_MUX_FRONT_TOP_0, VME_MUX_FRONT_TOP_1, VME_MUX_FRONT_TOP_2,
        VME_MUX_FRONT_TOP_3, VME_MUX_FRONT_BASE_0, VME_MUX_FRONT_BASE_1
    };
    static const uint32_t BACK_MUX[6] = {
        VME_MUX_BACK_TOP_0, VME_MUX_BACK_TOP_1, VME_MUX_BACK_TOP_2,
        VME_MUX_BACK_TOP_3, VME_MUX_BACK_BASE_0, VME_MUX_BACK_BASE_1
    };
    memset(ctx, 0, VME_CONTEXT_WORD_COUNT * 4);
    volatile u32 VME_BASE_ADDR = (u32)ctx;
    int sa = (rot + 0) % 6, sb = (rot + 1) % 6, sc = (rot + 2) % 6;
    int sd = (rot + 3) % 6, se = (rot + 4) % 6, sf = (rot + 5) % 6;
    uint32_t agu_top = 0, agu_base = 0;
    for (int k = 0; k < 4; ++k) {
        uint32_t drv = (k == sa || k == sf) ? 1u : 0u;
        agu_top |= (drv << (4 * k)) | (drv << (4 * (k + 4)));
    }
    for (int k = 0; k < 4; ++k) {
        uint32_t drv = (k < 2) ? (((4 + k) == sa || (4 + k) == sf) ? 1u : 0u) : 6u;
        agu_base |= (drv << (4 * k)) | (drv << (4 * (k + 4)));
    }
    vme_pe0(agu_top(MODE), VME_DEF_MODE);
    vme_pe0(agu_top(COUNT), VME_DEF_STEP, (uint32_t)(count - 1));
    vme_pe1(agu_top(MODE), VME_DEF_MODE, vme_cyc(6));
    vme_pe1(agu_top(COUNT), VME_DEF_STEP, (uint32_t)(count - 1));
    vme_pe0(agu_base(MODE), VME_DEF_MODE);
    vme_pe0(agu_base(COUNT), VME_DEF_STEP, (uint32_t)(count - 1));
    vme_pe1(agu_base(MODE), VME_DEF_MODE, vme_cyc(6));
    vme_pe1(agu_base(COUNT), VME_DEF_STEP, (uint32_t)(count - 1));
    vme_pe0(vme_fu(PRIMARY), FRONT_MUX[sc] | BACK_MUX[sd], fu_code(OP_ADD));
    vme_pe0(vme_fu(SECONDARY), FRONT_MUX[sb] | BACK_MUX[se], fu_code(OP_ADD));
    vme_pe1(vme_fu(PRIMARY), vme_mux(NONE, STAGING_0), VME_FU_OPCODE_MUL_CONST_RSHIFT_0);
    vme_pe1(fu_reg(PRIMARY, B), 20);
    vme_pe1(vme_fu(SECONDARY), FRONT_MUX[sa] | BACK_MUX[sf], fu_code(OP_ADD));
    vme_pe2(vme_fu(PRIMARY), vme_mux(NONE, STAGING_4), VME_FU_OPCODE_MUL_CONST_RSHIFT_0);
    vme_pe2(fu_reg(PRIMARY, B), 5);
    vme_pe2(vme_fu(SECONDARY), vme_mux(STAGING_1, STAGING_2), fu_code(OP_SUB));
    vme_pe3(agu_write(MODE), VME_DEF_MODE, vme_cyc(18));
    vme_pe3(agu_write(COUNT), VME_DEF_STEP, (uint32_t)(count - 1));
    vme_pe3(vme_fu(PRIMARY), vme_mux(STAGING_6, STAGING_5), fu_code(OP_ADD), (uint32_t)shift, (1u << 6));
    vme_pe3(vme_fu(SECONDARY), vme_mux(NONE, STAGING_3), fu_code(OP_CLP));
    vme_pe3(fu_reg(SECONDARY, A), (uint32_t)(255 + post_sub));
    vme_pe3(fu_reg(SECONDARY, B), (uint32_t)post_sub);
    vme_icn(AGU_TOP, agu_top);
    vme_icn(AGU_BASE, agu_base);
    vme_icn(AGU_WRITE, 0x34443444u);
    vme_icn(SWEN, fu_on(0x1u));
}

/* Called ONCE on SC before submitting ME VME jobs. */
static void vme_contexts_init(void)
{
    vme_build_csc_ctxs_half(vme_ctx_csc480[0], 480, 0);
    vme_build_csc_ctxs_half(vme_ctx_csc480[1], 480, 0x400);
    vme_build_horiz_ctxs(vme_ctx_h1, vme_ctx_h2, VME_HP_W);
    for (int rot = 0; rot < 6; ++rot) {
        vme_build_fir6_ctx(vme_ctx_p02[rot], rot, VME_HP_W, 5, 0);
        vme_build_fir6_ctx(vme_ctx_p22[rot], rot, VME_HP_W, 10, 128);
    }
    me_vme_wiped = 0;
    sceKernelDcacheWritebackRange(vme_ctx_csc480, sizeof(vme_ctx_csc480));
    sceKernelDcacheWritebackRange(vme_ctx_h1, sizeof(vme_ctx_h1));
    sceKernelDcacheWritebackRange(vme_ctx_h2, sizeof(vme_ctx_h2));
    sceKernelDcacheWritebackRange(vme_ctx_p02, sizeof(vme_ctx_p02));
    sceKernelDcacheWritebackRange(vme_ctx_p22, sizeof(vme_ctx_p22));
    sceKernelDcacheWritebackInvalidateRange((void *)&me_vme_wiped, 64);
}

static inline void vme_run_ctx(void *ctx)
{
    _vmeLibStart();
    vmeLibLoadCustomContext(ctx);
    _vmeLibFinish();
}

/* Called ONCE on ME at playback start (operation == 0). */
static void vme_me_wipe(void)
{
    vmeLibEnable();
    vmeLibWipe();
    vmeLibConfigTransfer(0);
    vmeLibDisable();
    *(volatile int *)((uintptr_t)&me_vme_wiped | 0xA0000000u) = 1;
    __asm__ volatile("sync" ::: "memory");
}

/* Bit-exact 640x360 -> 480x270 YUV420 -> RGB565 on VME2 (4:2:0 chroma row reuse
 * + 480-element 0x5a DMAC16 write straight to Main RAM; silicon-verified 0 mismatch,
 * ~12.5 ms/frame vs 17.4 ms ME MIPS). */
static void vme_render_565(const uint8_t *const data[3], const int stride[3], void *dst_phys)
{
    vmeLibEnable();
    vmeLibConfigTransfer(0);
    volatile uint32_t *sp_y = (volatile uint32_t *)VME_TOP_BUFFER_0;
    volatile uint32_t *sp_u = (volatile uint32_t *)VME_TOP_BUFFER_1;
    volatile uint32_t *sp_v = (volatile uint32_t *)VME_TOP_BUFFER_2;
    uint16_t *rgb_dst = (uint16_t *)dst_phys;

    int last_cy = -1;
    for (int dy = 0; dy < 270; ++dy) {
        int sy = (dy / 3) * 4 + (dy % 3);
        int cy = sy >> 1;
        const uint8_t *yl = data[0] + sy * stride[0];
        for (int g = 0; g < 160; ++g) {
            int sx = g * 4, dx = g * 3;
            sp_y[dx]     = yl[sx];
            sp_y[dx + 1] = yl[sx + 1];
            sp_y[dx + 2] = yl[sx + 2];
        }
        if (cy != last_cy) {
            last_cy = cy;
            const uint8_t *ul = data[1] + cy * stride[1];
            const uint8_t *vl = data[2] + cy * stride[2];
            for (int g = 0; g < 160; ++g) {
                int dx = g * 3, cx = g * 2;
                uint32_t u0 = ul[cx], v0 = vl[cx];
                sp_u[dx] = u0; sp_u[dx + 1] = u0; sp_u[dx + 2] = ul[cx + 1];
                sp_v[dx] = v0; sp_v[dx + 1] = v0; sp_v[dx + 2] = vl[cx + 1];
            }
        }
        vme_run_ctx(vme_ctx_csc480[0][0]);
        vme_run_ctx(vme_ctx_csc480[0][1]);
        vme_run_ctx(vme_ctx_csc480[0][2]);
        vmeLibMemFrom16((u32)(rgb_dst + (dy + 1) * 512), VME_BASE_BUFF2_WOFF, 480, 1);
    }
    vmeLibDisable();
}

static inline void vme_pack_row_640(const uint16_t *src, uint8_t *dst, uint32_t sub)
{
    const uint32_t *s = (const uint32_t *)src;
    uint32_t *d = (uint32_t *)dst;
    for (int x = 0; x < VME_HP_W / 4; ++x) {
        uint32_t w0 = s[2 * x], w1 = s[2 * x + 1];
        uint32_t b0 = (w0 & 0xffffu) - sub;
        uint32_t b1 = (w0 >> 16) - sub;
        uint32_t b2 = (w1 & 0xffffu) - sub;
        uint32_t b3 = (w1 >> 16) - sub;
        d[x] = (b0 & 0xffu) | ((b1 & 0xffu) << 8) | ((b2 & 0xffu) << 16) | ((b3 & 0xffu) << 24);
    }
}

/* Full 100% bit-exact 640xH half-pel precomputation (P20, P02, P22) on ME+VME2+eDRAM. */
static void vme_build_halfpel_640(const uint8_t *y_src, int y_stride, int coded_h,
                                  uint8_t *p20_phys, uint8_t *p02_phys, uint8_t *p22_phys)
{
    if (coded_h <= 0 || coded_h > VME_HP_MAX_H) return;
    uint16_t *edram_y16 = (uint16_t *)((VME_EDRAM_Y16 & 0x1fffffffu) | 0x80000000u);
    const uint16_t *edram_out_c = (const uint16_t *)((VME_EDRAM_OUT16 & 0x1fffffffu) | 0x80000000u);
    uint8_t *p20_dst = (uint8_t *)((uintptr_t)p20_phys | 0x80000000u);
    uint8_t *p02_dst = (uint8_t *)((uintptr_t)p02_phys | 0x80000000u);
    uint8_t *p22_dst = (uint8_t *)((uintptr_t)p22_phys | 0x80000000u);

    /* Invalidate cached Y input on ME */
    meCoreDcacheInvalidateRange((void *)y_src, y_stride * coded_h);

    /* Stage 1: Unpack (coded_h + 5) rows x 645 cols into ME eDRAM with safe edge clamp */
    for (int row = 0; row < coded_h + 5; ++row) {
        int sy = row - 2;
        if (sy < 0) sy = 0;
        else if (sy >= coded_h) sy = coded_h - 1;
        const uint8_t *s = y_src + sy * y_stride;
        uint16_t *d = edram_y16 + row * VME_HP_Y16_STRIDE;
        d[0] = s[0];
        d[1] = s[0];
        uint32_t *d32 = (uint32_t *)(d + 2);
        for (int x = 0; x < VME_HP_W; x += 4) {
            d32[x >> 1]       = (uint32_t)s[x]     | ((uint32_t)s[x + 1] << 16);
            d32[(x >> 1) + 1] = (uint32_t)s[x + 2] | ((uint32_t)s[x + 3] << 16);
        }
        d[VME_HP_W + 2] = s[VME_HP_W - 1];
        d[VME_HP_W + 3] = s[VME_HP_W - 1];
        d[VME_HP_W + 4] = s[VME_HP_W - 1];
        d[VME_HP_W + 5] = s[VME_HP_W - 1];
    }
    meCoreDcacheWritebackRange(edram_y16, VME_HP_Y16_STRIDE * (coded_h + 5) * 2);

    vmeLibEnable();
    vmeLibConfigTransfer(0);

    /* Stage 2: 1-DMAC 2-pass Horizontal FIR -> EDRAM_HRAW16 + p20_dst */
    for (int row = 0; row < coded_h + 5; ++row) {
        vmeLibMemTo16(VME_EDRAM_Y16 + (u32)(row * VME_HP_Y16_STRIDE * 2), VME_TOP_BUFF0_WOFF, 646, 1);
        _vmeLibStart();
        vmeLibLoadCustomContext(vme_ctx_h1);
        vmeLibTrigger();
        if (row >= 3 && row < coded_h + 3) {
            int py = row - 3;
            const uint16_t *prev = edram_out_c + (py & 1) * VME_HP_W;
            meCoreDcacheInvalidateRange((void *)prev, VME_HP_W * 2);
            vme_pack_row_640(prev, p20_dst + py * VME_HP_W, 0);
        }
        meCoreDMACPrimWaitVMEFinish();
        vme_run_ctx(vme_ctx_h2);
        vmeLibMemFrom16(VME_EDRAM_HRAW16 + (u32)(row * VME_HP_W * 2), VME_BASE_BUFF2_WOFF + 0x400, VME_HP_W, 1);
        if (row >= 2 && row < coded_h + 2) {
            int py = row - 2;
            vmeLibMemFrom16(VME_EDRAM_OUT16 + (u32)((py & 1) * VME_HP_W * 2), VME_BASE_BUFF3_WOFF + 0x400, VME_HP_W, 1);
        }
    }
    {
        const uint16_t *prev = edram_out_c + ((coded_h - 1) & 1) * VME_HP_W;
        meCoreDcacheInvalidateRange((void *)prev, VME_HP_W * 2);
        vme_pack_row_640(prev, p20_dst + (coded_h - 1) * VME_HP_W, 0);
        meCoreDcacheWritebackRange(p20_dst, VME_HP_W * coded_h);
    }

    /* Stage 3: Vertical 6-tap FIR -> p02_dst (reads EDRAM_Y16 + 4 bytes = col 0) */
    for (int i = 0; i < 5; ++i)
        vmeLibMemTo16(VME_EDRAM_Y16 + (u32)(i * VME_HP_Y16_STRIDE * 2) + 4u, vme_slot_woff[i], VME_HP_W, 1);
    for (int y = 0; y < coded_h; ++y) {
        vmeLibMemTo16(VME_EDRAM_Y16 + (u32)((y + 5) * VME_HP_Y16_STRIDE * 2) + 4u, vme_slot_woff[(y + 5) % 6], VME_HP_W, 1);
        _vmeLibStart();
        vmeLibLoadCustomContext(vme_ctx_p02[y % 6]);
        vmeLibTrigger();
        if (y > 0) {
            const uint16_t *prev = edram_out_c + ((y - 1) & 1) * VME_HP_W;
            meCoreDcacheInvalidateRange((void *)prev, VME_HP_W * 2);
            vme_pack_row_640(prev, p02_dst + (y - 1) * VME_HP_W, 0);
        }
        meCoreDMACPrimWaitVMEFinish();
        vmeLibMemFrom16(VME_EDRAM_OUT16 + (u32)((y & 1) * VME_HP_W * 2), VME_BASE_BUFF3_WOFF, VME_HP_W, 1);
    }
    {
        const uint16_t *prev = edram_out_c + ((coded_h - 1) & 1) * VME_HP_W;
        meCoreDcacheInvalidateRange((void *)prev, VME_HP_W * 2);
        vme_pack_row_640(prev, p02_dst + (coded_h - 1) * VME_HP_W, 0);
        meCoreDcacheWritebackRange(p02_dst, VME_HP_W * coded_h);
    }

    /* Stage 4: 2D 6-tap FIR -> p22_dst (reads EDRAM_HRAW16 directly) */
    for (int i = 0; i < 5; ++i)
        vmeLibMemTo16(VME_EDRAM_HRAW16 + (u32)(i * VME_HP_W * 2), vme_slot_woff[i], VME_HP_W, 1);
    for (int y = 0; y < coded_h; ++y) {
        vmeLibMemTo16(VME_EDRAM_HRAW16 + (u32)((y + 5) * VME_HP_W * 2), vme_slot_woff[(y + 5) % 6], VME_HP_W, 1);
        _vmeLibStart();
        vmeLibLoadCustomContext(vme_ctx_p22[y % 6]);
        vmeLibTrigger();
        if (y > 0) {
            const uint16_t *prev = edram_out_c + ((y - 1) & 1) * VME_HP_W;
            meCoreDcacheInvalidateRange((void *)prev, VME_HP_W * 2);
            vme_pack_row_640(prev, p22_dst + (y - 1) * VME_HP_W, 128);
        }
        meCoreDMACPrimWaitVMEFinish();
        vmeLibMemFrom16(VME_EDRAM_OUT16 + (u32)((y & 1) * VME_HP_W * 2), VME_BASE_BUFF3_WOFF, VME_HP_W, 1);
    }
    {
        const uint16_t *prev = edram_out_c + ((coded_h - 1) & 1) * VME_HP_W;
        meCoreDcacheInvalidateRange((void *)prev, VME_HP_W * 2);
        vme_pack_row_640(prev, p22_dst + (coded_h - 1) * VME_HP_W, 128);
        meCoreDcacheWritebackRange(p22_dst, VME_HP_W * coded_h);
    }

    vmeLibDisable();
}

#endif
