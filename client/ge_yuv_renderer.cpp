#include "ge_yuv_renderer.hpp"
#include <algorithm>
#include <cstdint>
#include <pspgu.h>
#include <pspkernel.h>

namespace pspapp {
GeYuvRenderer::GeYuvRenderer() {
    auto clamp8 = [](int v) -> unsigned {
        return static_cast<unsigned>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    for (int i = 0; i < 256; ++i) {
        const unsigned yc = clamp8((298 * (i - 16) + 128) >> 8);
        clut_[0][i] = 0xff000000u | (yc << 16) | (yc << 8) | yc;

        const int d = i - 128;
        const int gu = -55 * d;
        const int bu = 541 * d;
        const unsigned guPos = gu > 0 ? clamp8((gu + 128) >> 8) : 0u;
        const unsigned guNeg = gu < 0 ? clamp8((-gu + 128) >> 8) : 0u;
        const unsigned buPos = bu > 0 ? clamp8((bu + 128) >> 8) : 0u;
        const unsigned buNeg = bu < 0 ? clamp8((-bu + 128) >> 8) : 0u;
        clut_[1][i] = (buPos << 16) | (guPos << 8);
        clut_[2][i] = (buNeg << 16) | (guNeg << 8);

        const int rv = 459 * d;
        const int gv = -136 * d;
        const unsigned rvPos = rv > 0 ? clamp8((rv + 128) >> 8) : 0u;
        const unsigned rvNeg = rv < 0 ? clamp8((-rv + 128) >> 8) : 0u;
        const unsigned gvPos = gv > 0 ? clamp8((gv + 128) >> 8) : 0u;
        const unsigned gvNeg = gv < 0 ? clamp8((-gv + 128) >> 8) : 0u;
        clut_[3][i] = (gvPos << 8) | rvPos;
        clut_[4][i] = (gvNeg << 8) | rvNeg;
    }
    sceKernelDcacheWritebackRange(clut_, sizeof(clut_));
}
void GeYuvRenderer::fill(int x, int y, int w, int h, unsigned color) {
    struct Vertex { short x, y, z; };
    auto* vertices = static_cast<Vertex*>(sceGuGetMemory(2 * sizeof(Vertex)));
    vertices[0] = {static_cast<short>(x), static_cast<short>(y), 0};
    vertices[1] = {static_cast<short>(x + w), static_cast<short>(y + h), 0};
    sceGuDisable(GU_TEXTURE_2D);
    sceGuColor(color);
    sceGuDrawArray(GU_SPRITES, GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, nullptr, vertices);
}
void GeYuvRenderer::draw(const PspVideoYuvFrame& frame) const {
    if (!frame.planes[0] || !frame.planes[1] || !frame.planes[2] ||
        frame.width <= 1 || frame.height <= 1) return;

    int dstW = 480, dstH = 270;
    if (frame.width * 270 > frame.height * 480) {
        dstH = std::max(2, (480 * frame.height + frame.width / 2) / frame.width);
    } else if (frame.width * 270 < frame.height * 480) {
        dstW = std::max(2, (270 * frame.width + frame.height / 2) / frame.height);
    }
    const int dstX0 = (480 - dstW) / 2;
    const int dstY0 = (272 - dstH) / 2;

    sceGuDisable(GU_BLEND);
    if (dstY0 > 0) {
        fill(0, 0, 480, dstY0, 0xff000000u);
        fill(0, dstY0 + dstH, 480, 272 - (dstY0 + dstH), 0xff000000u);
    }
    if (dstX0 > 0) {
        fill(0, dstY0, dstX0, dstH, 0xff000000u);
        fill(dstX0 + dstW, dstY0, 480 - (dstX0 + dstW), dstH, 0xff000000u);
    }

    auto nextPow2 = [](int v) {
        int p = 16;
        while (p < v && p < 512) p <<= 1;
        return p;
    };

    struct YuvVertex { float u, v; float x, y, z; };
    auto drawPlaneTiles = [&](const uint8_t* plane, int stride, int srcW, int srcH,
                              const unsigned int* clutA, int opA,
                              const unsigned int* clutB, int opB) {
        if (srcW <= 1 || srcH <= 1 || stride < srcW || (stride & 15) != 0) return;
        const uintptr_t addr = reinterpret_cast<uintptr_t>(plane);
        const int alignShift = static_cast<int>(addr & 15u);
        const uint8_t* base = plane - alignShift;
        const float uScale = static_cast<float>(srcW - 1) / static_cast<float>(dstW);
        const float vScale = static_cast<float>(srcH - 1) / static_cast<float>(dstH);
        constexpr int tileStep = 480;
        constexpr int stripW = 64;

        for (int ty = 0; ty < srcH; ty += tileStep) {
            const int y0 = (ty == 0) ? dstY0
                : dstY0 + (ty * dstH + srcH - 2) / (srcH - 1);
            const int nextTy = ty + tileStep;
            const int y1 = (nextTy >= srcH) ? (dstY0 + dstH)
                : dstY0 + (nextTy * dstH + srcH - 2) / (srcH - 1);
            if (y1 <= y0) continue;

            const float v0 = 0.5f + static_cast<float>(y0 - dstY0) * vScale - static_cast<float>(ty);
            const float v1 = 0.5f + static_cast<float>(y1 - dstY0) * vScale - static_cast<float>(ty);
            const int texH = nextPow2(std::min(512, static_cast<int>(v1) + 2));

            for (int tx = 0; tx < srcW; tx += tileStep) {
                const int x0 = (tx == 0) ? dstX0
                    : dstX0 + (tx * dstW + srcW - 2) / (srcW - 1);
                const int nextTx = tx + tileStep;
                const int x1 = (nextTx >= srcW) ? (dstX0 + dstW)
                    : dstX0 + (nextTx * dstW + srcW - 2) / (srcW - 1);
                if (x1 <= x0) continue;

                const float tileU0 = 0.5f + static_cast<float>(x0 - dstX0) * uScale - static_cast<float>(tx) + static_cast<float>(alignShift);
                const float tileU1 = 0.5f + static_cast<float>(x1 - dstX0) * uScale - static_cast<float>(tx) + static_cast<float>(alignShift);
                const int texW = nextPow2(std::min(512, static_cast<int>(tileU1) + 2));

                const uint8_t* tilePtr = base + static_cast<size_t>(ty) * stride + tx;
                sceGuTexImage(0, texW, texH, stride, tilePtr);
                sceGuTexFlush();

                const int strips = (x1 - x0 + stripW - 1) / stripW;
                auto* verts = static_cast<YuvVertex*>(sceGuGetMemory(2 * strips * sizeof(YuvVertex)));
                for (int s = 0; s < strips; ++s) {
                    const int sx0 = x0 + s * stripW;
                    const int sx1 = std::min(x1, sx0 + stripW);
                    const float u0 = (s == 0) ? tileU0
                        : 0.5f + static_cast<float>(sx0 - dstX0) * uScale - static_cast<float>(tx) + static_cast<float>(alignShift);
                    const float u1 = (sx1 == x1) ? tileU1
                        : 0.5f + static_cast<float>(sx1 - dstX0) * uScale - static_cast<float>(tx) + static_cast<float>(alignShift);
                    verts[2 * s]     = {u0, v0, static_cast<float>(sx0), static_cast<float>(y0), 0.0f};
                    verts[2 * s + 1] = {u1, v1, static_cast<float>(sx1), static_cast<float>(y1), 0.0f};
                }

                if (opA < 0) {
                    sceGuDisable(GU_BLEND);
                    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
                } else {
                    sceGuEnable(GU_BLEND);
                    sceGuBlendFunc(opA, GU_FIX, GU_FIX, 0xffffffffu, 0xffffffffu);
                    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
                }
                sceGuClutLoad(32, clutA);
                sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                               2 * strips, nullptr, verts);

                if (clutB) {
                    sceGuBlendFunc(opB, GU_FIX, GU_FIX, 0xffffffffu, 0xffffffffu);
                    sceGuClutLoad(32, clutB);
                    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                                   2 * strips, nullptr, verts);
                }
            }
        }
    };

    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);

    // Pass 1 replaces with Y; passes 2/3 add/subtract the U terms and 4/5 the
    // V terms, each a CLUT lookup of the plane in main RAM.
    drawPlaneTiles(frame.planes[0], frame.strides[0], frame.width, frame.height,
                   clut_[0], -1, nullptr, -1);
    drawPlaneTiles(frame.planes[1], frame.strides[1], frame.width / 2, frame.height / 2,
                   clut_[1], GU_ADD, clut_[2], GU_REVERSE_SUBTRACT);
    drawPlaneTiles(frame.planes[2], frame.strides[2], frame.width / 2, frame.height / 2,
                   clut_[3], GU_ADD, clut_[4], GU_REVERSE_SUBTRACT);
}
}
