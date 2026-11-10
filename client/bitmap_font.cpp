#include "bitmap_font.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

namespace {
struct Record {
    uint32_t cp;
    uint8_t size, advance, width, height;
    uint32_t offset;
};
static_assert(sizeof(Record) == 12, "Font index layout");
struct GlyphRange {
    uint32_t first, last, index;
    uint8_t size, width, height;
};
FILE* fontFile = nullptr;
// Store five bytes per glyph in separate arrays to keep the offsets aligned.
std::vector<uint32_t> glyphOffsets;
std::vector<uint8_t> glyphAdvances;
std::vector<GlyphRange> glyphRanges;
BitmapGlyph cache[384];
// Recent lookups by hash, validated against the glyph in the slot (slots are reused round-robin).
constexpr size_t kFront = 1024;
const BitmapGlyph* front[kFront];
size_t frontIndex(uint32_t cp, int size) { return ((cp * 2654435761u) ^ (static_cast<uint32_t>(size) * 40503u)) >> 22 & (kFront - 1); }
size_t nextSlot = 0;
long dataOffset = 0;
bool readFontIndex(uint32_t count) {
    glyphOffsets.resize(count);
    glyphAdvances.resize(count);
    Record batch[128];
    for (uint32_t base = 0; base < count;) {
        const uint32_t length = std::min(count - base, uint32_t{128});
        if (std::fread(batch, sizeof(Record), length, fontFile) != length) return false;
        for (uint32_t i = 0; i < length; ++i) {
            const Record& record = batch[i];
            const uint32_t index = base + i;
            glyphOffsets[index] = record.offset;
            glyphAdvances[index] = record.advance;
            if (!glyphRanges.empty()) {
                auto& range = glyphRanges.back();
                if (record.size < range.size || (record.size == range.size && record.cp < range.last))
                    return false;
                if (record.size == range.size && record.cp - range.last == 1 &&
                    record.width == range.width && record.height == range.height) {
                    range.last = record.cp;
                    continue;
                }
            }
            glyphRanges.push_back({record.cp, record.cp, index, record.size, record.width, record.height});
        }
        base += length;
    }
    return true;
}

bool findRecord(uint32_t cp, int size, Record& record) {
    const auto found = std::lower_bound(glyphRanges.begin(), glyphRanges.end(), std::make_pair(size, cp),
        [](const GlyphRange& range, const std::pair<int, uint32_t>& key) {
            return range.size < key.first || (range.size == key.first && range.last < key.second);
        });
    if (found == glyphRanges.end() || found->size != size || cp < found->first)
        return cp != '?' && findRecord('?', size, record);
    const uint32_t index = found->index + (cp - found->first);
    record = {cp, found->size, glyphAdvances[index], found->width, found->height, glyphOffsets[index]};
    return true;
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
    try {
        if (readFontIndex(count) && (dataOffset = std::ftell(fontFile)) >= 0) return true;
    } catch (const std::bad_alloc&) {
        // Release any partially built index below.
    }
    bitmapFontShutdown();
    return false;
}

void bitmapFontShutdown() {
    if (fontFile) std::fclose(fontFile);
    fontFile = nullptr;
    std::vector<uint32_t>{}.swap(glyphOffsets);
    std::vector<uint8_t>{}.swap(glyphAdvances);
    std::vector<GlyphRange>{}.swap(glyphRanges);
    for (auto& glyph : cache) glyph = BitmapGlyph{};
    for (auto& entry : front) entry = nullptr;
    nextSlot = 0;
}

const BitmapGlyph* bitmapGlyph(uint32_t cp, int size) {
    if (!fontFile) return nullptr;
    const size_t slotHash = frontIndex(cp, size);
    if (const BitmapGlyph* hit = front[slotHash]; hit && hit->codepoint == cp && hit->size == size) return hit;
    for (const auto& glyph : cache)
        if (glyph.codepoint == cp && glyph.size == size) { front[slotHash] = &glyph; return &glyph; }
    Record record;
    if (!findRecord(cp, size, record)) return nullptr;
    BitmapGlyph& glyph = cache[nextSlot++ % 384];
    glyph = BitmapGlyph{};
    const size_t bytes = (record.width * record.height + 1) / 2;
    if (bytes > sizeof(glyph.alpha) || std::fseek(fontFile, dataOffset + record.offset, SEEK_SET) ||
        std::fread(glyph.alpha, 1, bytes, fontFile) != bytes) return nullptr;
    glyph.codepoint = cp; glyph.size = size; glyph.advance = record.advance;
    glyph.width = record.width; glyph.height = record.height;
    front[slotHash] = &glyph;
    return &glyph;
}

int bitmapAdvance(uint32_t cp, int size) {
    Record record;
    return findRecord(cp, size, record) ? record.advance : size;
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
