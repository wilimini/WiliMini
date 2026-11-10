#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

struct BitmapGlyph {
    uint32_t codepoint = 0;
    uint8_t size = 0, advance = 0, width = 0, height = 0;
    unsigned char alpha[180]{};
};
bool bitmapFontInit(const char* path);
void bitmapFontShutdown();
const BitmapGlyph* bitmapGlyph(uint32_t codepoint, int size);
int bitmapAdvance(uint32_t codepoint, int size);
uint32_t bitmapNext(std::string_view text, size_t& position);
int bitmapMeasure(std::string_view text, int size);

// Optional 24 px music font. Missing characters can use the body font.
constexpr int kLyricFontSize = 24;
struct LyricGlyph {
    uint8_t advance, width, height;
    unsigned char alpha[28 * 32 / 2];
};
bool lyricFontInit(const char* path);
void lyricFontShutdown();
const LyricGlyph* lyricGlyph(uint32_t codepoint);
int lyricAdvance(uint32_t codepoint);
