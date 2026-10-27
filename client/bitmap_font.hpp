#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

struct BitmapGlyph {
    uint32_t codepoint = 0;
    uint8_t size = 0, advance = 0, width = 0, height = 0;
    unsigned char alpha[180]{};
};
bool bitmapFontInit(const char* path);
void bitmapFontShutdown();
const BitmapGlyph* bitmapGlyph(uint32_t codepoint, int size);
int bitmapAdvance(uint32_t codepoint, int size);
uint32_t bitmapNext(const std::string& text, size_t& position);
int bitmapMeasure(const std::string& text, int size);
