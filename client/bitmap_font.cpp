#include "bitmap_font.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace {
struct Record {
    uint32_t cp;
    uint8_t size, advance, width, height;
    uint32_t offset;
};
static_assert(sizeof(Record) == 12, "Font index layout");
FILE* fontFile = nullptr;
std::vector<Record> fontIndex;
BitmapGlyph cache[384];
size_t nextSlot = 0;
long dataOffset = 0;
const Record* findRecord(uint32_t cp, int size) {
    auto found = std::lower_bound(fontIndex.begin(), fontIndex.end(), std::make_pair(size, cp),
        [](const Record& r, const std::pair<int, uint32_t>& key) {
            return r.size < key.first || (r.size == key.first && r.cp < key.second);
        });
    if (found != fontIndex.end() && found->cp == cp && found->size == size) return &*found;
    return cp == '?' ? nullptr : findRecord('?', size);
}
}

bool bitmapFontInit(const char* path) {
    bitmapFontShutdown();
    fontFile = std::fopen(path, "rb");
    if (!fontFile) return false;
    char magic[8]; uint32_t count = 0;
    if (std::fread(magic, 1, 8, fontFile) != 8 || std::memcmp(magic, "BMFONT01", 8) ||
        std::fread(&count, 4, 1, fontFile) != 1 || count > 200000) {
        bitmapFontShutdown(); return false;
    }
    fontIndex.resize(count);
    if (std::fread(fontIndex.data(), sizeof(Record), count, fontFile) != count) {
        bitmapFontShutdown(); return false;
    }
    dataOffset = std::ftell(fontFile);
    return true;
}

void bitmapFontShutdown() {
    if (fontFile) std::fclose(fontFile);
    fontFile = nullptr;
    fontIndex.clear();
    for (auto& glyph : cache) glyph = BitmapGlyph{};
    nextSlot = 0;
}

const BitmapGlyph* bitmapGlyph(uint32_t cp, int size) {
    if (!fontFile) return nullptr;
    for (const auto& glyph : cache)
        if (glyph.codepoint == cp && glyph.size == size) return &glyph;
    const auto* found = findRecord(cp, size);
    if (!found) return nullptr;
    BitmapGlyph& glyph = cache[nextSlot++ % 384];
    glyph = BitmapGlyph{};
    const size_t bytes = (found->width * found->height + 1) / 2;
    if (bytes > sizeof(glyph.alpha) || std::fseek(fontFile, dataOffset + found->offset, SEEK_SET) ||
        std::fread(glyph.alpha, 1, bytes, fontFile) != bytes) return nullptr;
    glyph.codepoint = cp; glyph.size = size; glyph.advance = found->advance;
    glyph.width = found->width; glyph.height = found->height;
    return &glyph;
}

int bitmapAdvance(uint32_t cp, int size) {
    const auto* record = findRecord(cp, size);
    return record ? record->advance : size;
}

uint32_t bitmapNext(std::string_view text, size_t& pos) {
    if (pos >= text.size()) return 0;
    const unsigned char first = text[pos++];
    if (first < 0x80) return first;
    int remaining = first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : first >= 0xc0 ? 1 : 0;
    uint32_t cp = first & (remaining == 3 ? 7 : remaining == 2 ? 15 : 31);
    if (!remaining) return '?';
    while (remaining--) {
        if (pos >= text.size() || (static_cast<unsigned char>(text[pos]) & 0xc0) != 0x80) return '?';
        cp = (cp << 6) | (static_cast<unsigned char>(text[pos++]) & 63);
    }
    return cp;
}

int bitmapMeasure(std::string_view text, int size) {
    int width = 0;
    for (size_t pos = 0; pos < text.size();) {
        width += bitmapAdvance(bitmapNext(text, pos), size);
    }
    return width;
}
