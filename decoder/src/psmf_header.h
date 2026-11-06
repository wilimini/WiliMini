#ifndef WILI_PSMF_HEADER_H
#define WILI_PSMF_HEADER_H
/* The 2048-byte PSMF header handed to sceMpegQueryStreamOffset, laid out like a real one.
 *
 * A real PMF is this header followed by an MPEG program stream. The field offsets here come from the
 * header Sony's UMD Stream Composer writes (decoded from the template in the open-source pmftools
 * "mps2pmf" converter, which patches only the stream size, the duration and a few stream fields) and
 * from what PPSSPP's sceMpeg.cpp reads (offset 0x08/0x0C, timestamps at 0x54/0x5A, frame size at 0x8E).
 * Fields that the firmware may use are filled in rather than left zero:
 *
 *   0x00 "PSMF", 0x04 version "0012"     0x08 stream offset (2048)    0x0C stream size
 *   0x50 u32 length of the block to 0xA2 0x54/0x5A  first / last timestamp, 48-bit, 90 kHz
 *   0x60 u32 mux rate (0x61A8)           0x64 u32 90000 (the tick rate)
 *   0x68 02 01                           0x6A u32 length of the block to 0xA2
 *   0x6E/0x74 first / last timestamp again, 0x7A u16 1, 0x7C u32 length of the stream table
 *   0x80 u16 stream count, then 16 bytes per stream from 0x82:
 *        video  E0 00 | P-STD 21EF | ... | width/16 height/16 at +12/+13
 *        audio  BD 00 | P-STD 2004 | ... | channels 2, 2 at +14/+15
 *
 * The audio entry is there because every real file has one; no audio is ever sent through sceMpeg
 * here (FFmpeg decodes it), so no ATRAC stream is registered. No PSP or FFmpeg headers. */
#include <stdint.h>
#include <string.h>

#define PSMF_HEADER_SIZE 2048
#define PSMF_FIRST_TIMESTAMP 90000u    /* real files start one second in */
#define PSMF_LAST_TIMESTAMP 0x7fffffffu /* a streamed file has no known end */

static inline void psmf_put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void psmf_put32(uint8_t *p, uint32_t v) {
    psmf_put16(p, v >> 16); psmf_put16(p + 2, v & 0xffff);
}
/* Timestamps are 48-bit big-endian fields; ours fit in 32 bits. */
static inline void psmf_put48(uint8_t *p, uint32_t v) {
    psmf_put16(p, 0); psmf_put32(p + 2, v);
}

/* header must hold PSMF_HEADER_SIZE bytes. Width and height are the coded picture size. version is the
 * "00NN" in the file (12..15). full = 0 writes only the fields PPSSPP reads and a single video stream,
 * which is what the first working build used; full = 1 writes everything a real header has. */
static inline void psmf_build_header(uint8_t *header, int width, int height, int version, int full) {
    memset(header, 0, PSMF_HEADER_SIZE);
    memcpy(header, "PSMF0000", 8);
    header[6] = (uint8_t)('0' + (version / 10) % 10);
    header[7] = (uint8_t)('0' + version % 10);
    psmf_put32(header + 0x08, PSMF_HEADER_SIZE);
    psmf_put32(header + 0x0c, 0x7ffff800u);
    if (!full) {
        psmf_put32(header + 0x56, PSMF_FIRST_TIMESTAMP);
        psmf_put32(header + 0x5c, PSMF_LAST_TIMESTAMP);
        psmf_put16(header + 0x80, 1);
        header[0x82] = 0xe0;
        header[0x8e] = (uint8_t)((width + 15) / 16);
        header[0x8f] = (uint8_t)((height + 15) / 16);
        return;
    }
    psmf_put32(header + 0x50, 0x4e);
    psmf_put48(header + 0x54, PSMF_FIRST_TIMESTAMP);
    psmf_put48(header + 0x5a, PSMF_LAST_TIMESTAMP);
    psmf_put32(header + 0x60, 0x61a8);
    psmf_put32(header + 0x64, 90000);
    header[0x68] = 2; header[0x69] = 1;
    psmf_put32(header + 0x6a, 0x34);
    psmf_put48(header + 0x6e, PSMF_FIRST_TIMESTAMP);
    psmf_put48(header + 0x74, PSMF_LAST_TIMESTAMP);
    psmf_put16(header + 0x7a, 1);
    psmf_put32(header + 0x7c, 0x22);
    psmf_put16(header + 0x80, 2);
    uint8_t *video = header + 0x82;
    video[0] = 0xe0;
    psmf_put16(video + 2, 0x21ef);
    video[12] = (uint8_t)((width + 15) / 16);
    video[13] = (uint8_t)((height + 15) / 16);
    uint8_t *audio = header + 0x92;
    audio[0] = 0xbd;
    psmf_put16(audio + 2, 0x2004);
    audio[14] = 2; audio[15] = 2;
}
#endif
