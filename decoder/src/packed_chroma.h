/* Exact 8-bit chroma interpolation using two independent 16-bit lanes. */
#ifndef PSP_LAB_PACKED_CHROMA_H
#define PSP_LAB_PACKED_CHROMA_H
#if PSP_PACKED_CHROMA
static h264_chroma_mc_func packed_chroma_original[2][2];
unsigned packed_chroma_test_failures;
static inline __attribute__((always_inline)) uint32_t chroma_load4(const uint8_t *p)
{
    uint32_t value;
    __asm__ volatile("ulw %0, 0(%1)" : "=r"(value) : "r"(p) : "memory");
    return value;
}
static inline __attribute__((always_inline)) uint32_t chroma_mix4(
    uint32_t a, uint32_t b, uint32_t c, uint32_t d,
    unsigned wa, unsigned wb, unsigned wc, unsigned wd, unsigned round, unsigned shift)
{
    const uint32_t mask = 0x00ff00ffu;
    uint32_t lo = ((a & mask)*wa + (b & mask)*wb + (c & mask)*wc + (d & mask)*wd + round) >> shift;
    uint32_t hi = (((a >> 8) & mask)*wa + ((b >> 8) & mask)*wb +
                   ((c >> 8) & mask)*wc + ((d >> 8) & mask)*wd + round) >> shift;
    return (lo & mask) | ((hi & mask) << 8);
}
static inline __attribute__((always_inline)) void chroma_store4(uint8_t *d, uint32_t value, unsigned avg)
{
    if (avg) {
        uint32_t old = chroma_load4(d);
        value = (old | value) - (((old ^ value) & 0xfefefefeu) >> 1);
    }
    __asm__ volatile("usw %0, 0(%1)" :: "r"(value), "r"(d) : "memory");
}
static inline __attribute__((always_inline)) void packed_chroma_predict(
    uint8_t *d, const uint8_t *s, ptrdiff_t st, int h, int x, int y, unsigned index, unsigned avg)
{
    if (!x && !y) {
        /* H.264 partitions have word-aligned destinations and strides. Avoid
         * memcpy's conservative unaligned destination operations and spills;
         * keep the original implementation for callers outside that contract. */
        if (((uintptr_t)d | (uintptr_t)st) & 3) {
            packed_chroma_original[avg][index](d,s,st,h,x,y);
            return;
        }
        const unsigned width = 8 >> index;
        for (int row = 0; row < h; ++row) {
            for (unsigned col = 0; col < width; col += 4) {
                uint32_t value = chroma_load4(s + col);
                if (avg) {
                    uint32_t old;
                    __asm__ volatile("lw %0, 0(%1)" : "=r"(old) : "r"(d + col) : "memory");
                    value = (old | value) - (((old ^ value) & 0xfefefefeu) >> 1);
                }
                __asm__ volatile("sw %0, 0(%1)" :: "r"(value), "r"(d + col) : "memory");
            }
            d += st; s += st;
        }
        return;
    }
    unsigned width = 8 >> index;
    if (x && y) {
        unsigned wa = (8-x)*(8-y), wb = x*(8-y), wc = (8-x)*y, wd = x*y;
        for (int row = 0; row < h; ++row) {
            for (unsigned col = 0; col < width; col += 4) {
                uint32_t value = chroma_mix4(chroma_load4(s+col), chroma_load4(s+col+1),
                    chroma_load4(s+col+st), chroma_load4(s+col+st+1), wa,wb,wc,wd,0x00200020u,6);
                chroma_store4(d+col,value,avg);
            }
            d += st; s += st;
        }
    } else {
        unsigned fraction = x ? x : y;
        ptrdiff_t step = x ? 1 : st;
        for (int row = 0; row < h; ++row) {
            for (unsigned col = 0; col < width; col += 4) {
                uint32_t value = chroma_mix4(chroma_load4(s+col), chroma_load4(s+col+step),
                    0,0,8-fraction,fraction,0,0,0x00040004u,3);
                chroma_store4(d+col,value,avg);
            }
            d += st; s += st;
        }
    }
}
#define CHROMA_WRAPPERS(N) \
static void packed_chroma_put##N(uint8_t *d,const uint8_t *s,ptrdiff_t st,int h,int x,int y) \
{packed_chroma_predict(d,s,st,h,x,y,N,0);} \
static void packed_chroma_avg##N(uint8_t *d,const uint8_t *s,ptrdiff_t st,int h,int x,int y) \
{packed_chroma_predict(d,s,st,h,x,y,N,1);}
CHROMA_WRAPPERS(0) CHROMA_WRAPPERS(1)
#undef CHROMA_WRAPPERS
static void packed_chroma_init(H264ChromaContext *c)
{
    h264_chroma_mc_func put[2] = {packed_chroma_put0,packed_chroma_put1};
    h264_chroma_mc_func avg[2] = {packed_chroma_avg0,packed_chroma_avg1};
    for (unsigned i = 0; i < 2; ++i) {
        packed_chroma_original[0][i] = c->put_h264_chroma_pixels_tab[i];
        packed_chroma_original[1][i] = c->avg_h264_chroma_pixels_tab[i];
        c->put_h264_chroma_pixels_tab[i] = put[i];
        c->avg_h264_chroma_pixels_tab[i] = avg[i];
    }
    uint8_t source[48*12] __attribute__((aligned(16)));
    uint8_t target[48*12] __attribute__((aligned(16)));
    packed_chroma_test_failures = 0;
    for (unsigned i = 0; i < sizeof(source); ++i) source[i] = (i*97 + (i>>2)*13) & 255;
    for (unsigned offset = 0; offset < 4; ++offset)
        for (unsigned size = 0; size < 2; ++size)
            for (int height = 2; height <= 8; height *= 2)
                for (unsigned x = 0; x < 8; ++x)
                    for (unsigned y = 0; y < 8; ++y)
                        for (unsigned average = 0; average < 2; ++average) {
                            memset(target,71,sizeof(target));
                            (average ? avg[size] : put[size])(target,source+offset,48,height,x,y);
                            unsigned width = 8 >> size;
                            for (unsigned row = 0; row < (unsigned)height; ++row)
                                for (unsigned col = 0; col < width; ++col) {
                                    const uint8_t *p = source+offset+row*48+col;
                                    unsigned value = ((8-x)*(8-y)*p[0] + x*(8-y)*p[1] +
                                        (8-x)*y*p[48] + x*y*p[49] + 32) >> 6;
                                    if (average) value = (71+value+1) >> 1;
                                    if (target[row*48+col] != value) ++packed_chroma_test_failures;
                                }
                            for (unsigned i = 0; i < sizeof(target); ++i)
                                if ((i/48 >= (unsigned)height || i%48 >= width) && target[i] != 71)
                                    ++packed_chroma_test_failures;
                        }
}
#endif
#endif
