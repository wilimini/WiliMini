#ifndef WILI_HW_AVC_H
#define WILI_HW_AVC_H
/* The PSP's hardware H.264 decoder, driven the way Moonlight-PSP does it
 * (https://github.com/k4idyn/Moonlight-PSP, GPL-3.0; see src/psp_avc_session.c and
 * src/psp_avc_submit.c there):
 *
 *  1. flash0:/kd/mpeg_vsh.prx (the XMB's sceMpeg) is loaded into the user partition and the
 *     client's sceMpeg imports are bound to it by avc_helper.prx. Only that module exports
 *     sceMpegGetAvcNalAu, which takes one access unit with its SPS/PPS directly: no program
 *     stream, no ring buffer.
 *  2. avc_helper.prx boots the Media Engine in H.264 mode 3 (Main/CABAC) or 4 (Baseline/CAVLC).
 *  3. sceMpeg is created in mode 4 with a 2 MB DDR work area; each access unit goes through
 *     GetAvcNalAu + AvcDecode, and the pictures are read with AvcDecodeDetail2 and converted to
 *     RGBA8888 with sceMpegBaseCscAvc.
 *
 * Needs custom firmware 6.60/6.61 with kubridge (both checked), H.264 Baseline or Main, at most
 * 480x272 coded, progressive, cropped only on the right/bottom. */
#include <pspkernel.h>
#include <pspmodulemgr.h>
#include <psputility_avmodules.h>
#include <pspmpeg.h>
#include <kubridge.h>
#include <string.h>
#include <malloc.h>

int wiliAvcBootMode(int mode);
int wiliAvcFixupImports(SceUID consumer, SceUID provider);

/* Firmware ABI, as verified by Moonlight-PSP on 6.61. */
typedef struct { void *sps; int sps_size; void *pps; int pps_size;
                 int prefix; void *data; int size; int mode; } HwAvcNal;
typedef struct { int reserved0[2]; int width, height; int reserved1[6]; } HwAvcInfo;
typedef struct { void *planes[8]; int reserved[3]; } HwAvcYuv;
typedef struct { int reserved0[4]; HwAvcInfo *info; int reserved1[6];
                 HwAvcYuv *yuv; int reserved2[12]; } HwAvcDetail;
typedef struct { int height_mb, width_mb, mode0, mode1; void *planes[8]; } HwAvcCsc;
int sceMpegGetAvcNalAu(SceMpeg *mpeg, HwAvcNal *nal, SceMpegAu *au);
int sceMpegAvcDecodeDetail2(SceMpeg *mpeg, HwAvcDetail **detail);
int sceMpegBaseCscAvc(void *rgba, void *unused, int stride, HwAvcCsc *csc);

#define HW_AVC_STRIDE 512
#define HW_AVC_MAX_W 480
#define HW_AVC_MAX_H 272
#define HW_AVC_PICTURES 3 /* AvcDecode reports up to three pictures per call */
#define HW_AVC_PICTURE_BYTES ((size_t)HW_AVC_STRIDE * HW_AVC_MAX_H * 4)
#define HW_AVC_DDR_BYTES 0x200000
#define HW_AVC_MAX_AU (256 * 1024)

typedef struct {
    SceMpeg mpeg;
    SceMpegRingbuffer ring; /* required by sceMpegCreate, never filled */
    SceMpegAu au __attribute__((aligned(64)));
    void *context, *ddr;
    SceInt32 decode_state;
    int mode, initialized, created;
} HwAvc;

static SceUID hw_avc_vsh_uid = -1;

/* Loads mpeg_vsh and binds the imports once per process; later sessions reuse it. */
static int hw_avc_load(void) {
    if (hw_avc_vsh_uid >= 0) return 0;
    unsigned version = sceKernelDevkitVersion();
    if (version != 0x06060010 && version != 0x06060110) {
        WLOG_E(WLC_PLAYER, "hw-avc: firmware 0x%08x; needs 6.60 or 6.61", version);
        return -1;
    }
    /* The bootstrap already loaded AVCODEC; loading it again answers "already loaded", which
     * is 0x80110f02 for AV modules (0x80111102 is the plain utility modules' code). */
    int r = sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
    if (r < 0 && (unsigned)r != 0x80110f02u && (unsigned)r != 0x80111102u) {
        WLOG_E(WLC_PLAYER, "hw-avc: avcodec load failed 0x%08x", (unsigned)r);
        return r;
    }
    SceKernelLMOption option;
    memset(&option, 0, sizeof(option));
    option.size = sizeof(option);
    option.mpidtext = option.mpiddata = PSP_MEMORY_PARTITION_USER;
    SceUID uid = kuKernelLoadModule("flash0:/kd/mpeg_vsh.prx", 0, &option);
    if (uid < 0) {
        WLOG_E(WLC_PLAYER, "hw-avc: mpeg_vsh load failed 0x%08x (kubridge?)", (unsigned)uid);
        return uid;
    }
    int status = 0;
    r = sceKernelStartModule(uid, 0, NULL, &status, NULL);
    if (r >= 0 && status < 0) r = status;
    if (r < 0) {
        WLOG_E(WLC_PLAYER, "hw-avc: mpeg_vsh start failed 0x%08x", (unsigned)r);
        sceKernelUnloadModule(uid);
        return r;
    }
    r = wiliAvcFixupImports(sceKernelGetModuleIdByAddress((const void *)&hw_avc_load), uid);
    WLOG_I(WLC_PLAYER, "hw-avc: mpeg_vsh=0x%08x imports bound=%d", (unsigned)uid, r);
    if (r < 1) {
        WLOG_E(WLC_PLAYER, "hw-avc: binding sceMpeg to mpeg_vsh failed %d (is avc_helper.prx loaded?)", r);
        /* The client's sceMpeg stubs may point into the module: keep it resident. */
        return r < 0 ? r : -1;
    }
    hw_avc_vsh_uid = uid;
    return 0;
}

static void hw_avc_close(HwAvc *s) {
    if (s->created) sceMpegDelete(&s->mpeg);
    if (s->initialized) sceMpegFinish();
    /* Leave the ME in Main mode, as Moonlight does: that is what the firmware expects. */
    if (s->mode == 4) wiliAvcBootMode(3);
    free(s->context); free(s->ddr);
    memset(s, 0, sizeof(*s));
}

/* main_mode: 1 for Main/CABAC streams, 0 for Baseline/CAVLC. */
static int hw_avc_open(HwAvc *s, int main_mode) {
    memset(s, 0, sizeof(*s));
    int r = hw_avc_load();
    if (r < 0) return r;
    /* The mode switch runs at priority 33 or lower, like Moonlight's. */
    int priority = sceKernelGetThreadCurrentPriority(), changed = 0;
    if (priority >= 0 && priority < 33 && sceKernelChangeThreadPriority(0, 33) >= 0) changed = 1;
    r = wiliAvcBootMode(main_mode ? 3 : 4);
    if (changed) sceKernelChangeThreadPriority(0, priority);
    WLOG_I(WLC_PLAYER, "hw-avc: boot mode=%d result=0x%08x", main_mode ? 3 : 4, (unsigned)r);
    if (r < 0) return r;
    s->mode = main_mode ? 3 : 4;
    r = sceMpegInit();
    if (r < 0) { WLOG_E(WLC_PLAYER, "hw-avc: sceMpegInit 0x%08x", (unsigned)r); goto fail; }
    s->initialized = 1;
    s->ddr = memalign(0x400000, HW_AVC_DDR_BYTES);
    int size = sceMpegQueryMemSize(4);
    if (!s->ddr || size <= 0) {
        WLOG_E(WLC_PLAYER, "hw-avc: no memory ddr=%p size=%d free=%u", s->ddr, size,
               (unsigned)sceKernelMaxFreeMemSize());
        r = size < 0 ? size : -1; goto fail;
    }
    memset(s->ddr, 0, HW_AVC_DDR_BYTES);
    s->context = memalign(64, (size_t)size);
    if (!s->context) { r = -1; goto fail; }
    memset(s->context, 0, (size_t)size);
    r = sceMpegCreate(&s->mpeg, s->context, size, &s->ring, HW_AVC_STRIDE, 4, (int)s->ddr);
    if (r < 0) { WLOG_E(WLC_PLAYER, "hw-avc: sceMpegCreate 0x%08x", (unsigned)r); goto fail; }
    s->created = 1;
    SceMpegAvcMode mode = {-1, SCE_MPEG_AVC_FORMAT_8888};
    r = sceMpegAvcDecodeMode(&s->mpeg, &mode);
    if (r < 0) goto fail;
    memset(&s->au, 0xff, sizeof(s->au));
    r = sceMpegInitAu(&s->mpeg, (char *)s->ddr + 0x10000, &s->au);
    if (r < 0) goto fail;
    return 0;
fail:
    hw_avc_close(s);
    return r < 0 ? r : -1;
}

/* Converts the pictures the last decode produced into rgba[i] (RGBA8888, stride 512). */
static int hw_avc_pictures(HwAvc *s, int count, int coded_height, void *const rgba[HW_AVC_PICTURES]) {
    HwAvcDetail *d = NULL;
    if (count <= 0) return 0;
    if (count > HW_AVC_PICTURES) count = HW_AVC_PICTURES;
    int r = sceMpegAvcDecodeDetail2(&s->mpeg, &d);
    if (r || !d || !d->info || !d->yuv) return r ? r : -1;
    const size_t bytes = (size_t)HW_AVC_STRIDE * (size_t)coded_height * 4;
    for (int i = 0; i < count; ++i) {
        HwAvcCsc csc;
        memset(&csc, 0, sizeof(csc));
        csc.width_mb = (d->info[i].width + 15) >> 4;
        csc.height_mb = (d->info[i].height + 15) >> 4;
        if (csc.width_mb <= 0 || csc.width_mb * 16 > HW_AVC_MAX_W ||
            csc.height_mb <= 0 || csc.height_mb * 16 > HW_AVC_MAX_H) return -1;
        memcpy(csc.planes, d->yuv[i].planes, sizeof(csc.planes));
        sceKernelDcacheWritebackInvalidateRange(rgba[i], bytes);
        r = sceMpegBaseCscAvc(rgba[i], NULL, HW_AVC_STRIDE, &csc);
        if (r) return r;
        sceKernelDcacheInvalidateRange(rgba[i], bytes);
    }
    return count;
}

/* Decodes one access unit: `avcc` holds its slices with 4-byte big-endian lengths, `sps`/`pps`
 * the parameter sets without start codes. Returns the number of pictures written to rgba, or a
 * negative error; after an error the session must be reopened. */
static int hw_avc_decode(HwAvc *s, const uint8_t *sps, int sps_size, const uint8_t *pps, int pps_size,
                         uint8_t *avcc, int size, int idr, int coded_height,
                         void *const rgba[HW_AVC_PICTURES]) {
    HwAvcNal nal;
    nal.sps = (void *)sps; nal.sps_size = sps_size;
    nal.pps = (void *)pps; nal.pps_size = pps_size;
    nal.prefix = 4; nal.data = avcc; nal.size = size; nal.mode = idr ? 3 : 0;
    sceKernelDcacheWritebackInvalidateAll();
    int r = sceMpegGetAvcNalAu(&s->mpeg, &nal, &s->au);
    if (r) { WLOG_E(WLC_PLAYER, "hw-avc: GetAvcNalAu 0x%08x bytes=%d idr=%d", (unsigned)r, size, idr); return r < 0 ? r : -1; }
    s->decode_state = 0;
    r = sceMpegAvcDecode(&s->mpeg, &s->au, HW_AVC_STRIDE, NULL, &s->decode_state);
    if (r) { WLOG_E(WLC_PLAYER, "hw-avc: AvcDecode 0x%08x bytes=%d idr=%d", (unsigned)r, size, idr); return r < 0 ? r : -1; }
    r = hw_avc_pictures(s, (int)s->decode_state, coded_height, rgba);
    if (r < 0) WLOG_E(WLC_PLAYER, "hw-avc: picture conversion 0x%08x", (unsigned)r);
    return r;
}

/* Pictures still held by the decoder at the end of the stream. */
static int hw_avc_drain(HwAvc *s, int coded_height, void *const rgba[HW_AVC_PICTURES]) {
    SceInt32 count = 0;
    sceKernelDcacheWritebackInvalidateAll();
    int r = sceMpegAvcDecodeStop(&s->mpeg, HW_AVC_STRIDE, (void *)rgba, &count);
    if (r) return r < 0 ? r : -1;
    return hw_avc_pictures(s, (int)count, coded_height, rgba);
}

/* Exp-Golomb reader over an RBSP that still has emulation-prevention bytes. */
typedef struct { const uint8_t *p; int size, pos, zeros, bit; uint8_t byte; } HwAvcBits;
static int hw_avc_bit(HwAvcBits *b) {
    if (!b->bit) {
        if (b->pos >= b->size) return -1;
        uint8_t v = b->p[b->pos++];
        if (b->zeros >= 2 && v == 3) {
            b->zeros = 0;
            if (b->pos >= b->size) return -1;
            v = b->p[b->pos++];
        }
        b->zeros = v ? 0 : b->zeros + 1;
        b->byte = v; b->bit = 8;
    }
    return (b->byte >> --b->bit) & 1;
}
static int hw_avc_ue(HwAvcBits *b) {
    int zeros = 0, v;
    while ((v = hw_avc_bit(b)) == 0) if (++zeros > 30) return -1;
    if (v < 0) return -1;
    unsigned value = 0;
    for (int i = 0; i < zeros; ++i) { v = hw_avc_bit(b); if (v < 0) return -1; value = value << 1 | (unsigned)v; }
    return (int)((1u << zeros) - 1 + value);
}
/* entropy_coding_mode_flag of a PPS NAL (with its header byte): 1 CABAC, 0 CAVLC, -1 malformed. */
static int hw_avc_pps_cabac(const uint8_t *pps, int size) {
    HwAvcBits b = {pps + 1, size - 1, 0, 0, 0, 0};
    if (size < 2 || hw_avc_ue(&b) < 0 || hw_avc_ue(&b) < 0) return -1;
    return hw_avc_bit(&b);
}
#endif
