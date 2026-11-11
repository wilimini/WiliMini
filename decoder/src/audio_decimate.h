#ifndef PSP_AUDIO_DECIMATE_H
#define PSP_AUDIO_DECIMATE_H
#include <stdint.h>

/* The PSP audio hardware plays 44.1 and 48 kHz. Lossless sources are often
 * 88.2/96 kHz (and rarely 176.4/192 kHz), which are exact multiples, so they are
 * brought down by averaging each group of 2 or 4 samples. A box average is a
 * crude low-pass filter, but it is cheap and keeps the CPU free for decoding.
 * No FFmpeg types: this file is checked on the host. */
typedef struct {
    int factor, phase;
    int32_t sum[2];
} AudioDecimator;

/* Ratio to a rate the hardware plays, or 0 when the source rate is not supported. */
static inline int audio_decimation_factor(int rate)
{
    switch (rate) {
    case 44100: case 48000: return 1;
    case 88200: case 96000: return 2;
    case 176400: case 192000: return 4;
    default: return 0;
    }
}

static inline void audio_decimator_init(AudioDecimator *d, int factor)
{
    d->factor = factor < 1 ? 1 : factor;
    d->phase = 0;
    d->sum[0] = d->sum[1] = 0;
}

/* Feeds one 16-bit stereo sample. Returns 1 and fills out when an output sample is ready. */
static inline int audio_decimator_push(AudioDecimator *d, int left, int right, int16_t out[2])
{
    if (d->factor <= 1) {
        out[0] = (int16_t)left;
        out[1] = (int16_t)right;
        return 1;
    }
    d->sum[0] += left;
    d->sum[1] += right;
    if (++d->phase < d->factor) return 0;
    out[0] = (int16_t)(d->sum[0] / d->factor);
    out[1] = (int16_t)(d->sum[1] / d->factor);
    d->phase = 0;
    d->sum[0] = d->sum[1] = 0;
    return 1;
}
#endif
