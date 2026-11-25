#pragma once
#include "psp_video.h"

namespace pspapp {
/* Converts a YUV420P frame to RGBA8888 with the GE: each plane is a T8 texture
 * looked up through a CLUT (Y replaces; U and V add and subtract their
 * contributions in two passes each), drawn with bilinear scaling and
 * letterboxing into the current GU draw buffer. The planes are read from main
 * RAM, so no CPU colour conversion or RGB staging buffer is needed.
 * BT.709 limited-range coefficients, matching the former CPU converter. */
class GeYuvRenderer {
public:
    GeYuvRenderer();
    /* Call inside sceGuStart/sceGuFinish with a 480x272 viewport. The planes
     * must be in memory (not only in the CPU cache) and stay valid until the
     * GU list completes. */
    void draw(const PspVideoYuvFrame& frame) const;

private:
    alignas(16) unsigned int clut_[5][256]{};
    static void fill(int x, int y, int w, int h, unsigned color);
};
}
