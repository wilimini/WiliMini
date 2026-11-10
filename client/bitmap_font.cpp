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
struct FontIndex {
    FILE* file = nullptr;
    // Five bytes per glyph, in separate arrays to keep the offsets aligned.
    std::vector<uint32_t> offsets;
    std::vector<uint8_t> advances;
    std::vector<GlyphRange> ranges;
    long dataOffset = 0;

    bool open(const char* path);
    void close();
    bool read(uint32_t count);
    bool find(uint32_t cp, int size, Record& record) const;
    bool pixels(const Record& record, unsigned char* out, size_t capacity) const;
};
FontIndex bodyFont, lyricsFont;
BitmapGlyph cache[384];
LyricGlyph lyricsGlyph;
// Recent lookups by hash, validated against the glyph in the slot (slots are reused round-robin).
constexpr size_t kFront = 1024;
const BitmapGlyph* front[kFront];
size_t frontIndex(uint32_t cp, int size) { return ((cp * 2654435761u) ^ (static_cast<uint32_t>(size) * 40503u)) >> 22 & (kFront - 1); }
size_t nextSlot = 0;

bool FontIndex::read(uint32_t count) {
    offsets.resize(count);
    advances.resize(count);
    Record batch[128];
    for (uint32_t base = 0; base < count;) {
        const uint32_t length = std::min(count - base, uint32_t{128});
        if (std::fread(batch, sizeof(Record), length, file) != length) return false;
        for (uint32_t i = 0; i < length; ++i) {
            const Record& record = batch[i];
            const uint32_t index = base + i;
            offsets[index] = record.offset;
            advances[index] = record.advance;
            if (!ranges.empty()) {
                auto& range = ranges.back();
                if (record.size < range.size || (record.size == range.size && record.cp < range.last))
                    return false;
                if (record.size == range.size && record.cp - range.last == 1 &&
                    record.width == range.width && record.height == range.height) {
                    range.last = record.cp;
                    continue;
                }
            }
            ranges.push_back({record.cp, record.cp, index, record.size, record.width, record.height});
        }
        base += length;
    }
    ranges.shrink_to_fit();
    return true;
}

bool FontIndex::find(uint32_t cp, int size, Record& record) const {
    const auto found = std::lower_bound(ranges.begin(), ranges.end(), std::make_pair(size, cp),
        [](const GlyphRange& range, const std::pair<int, uint32_t>& key) {
            return range.size < key.first || (range.size == key.first && range.last < key.second);
        });
    if (found == ranges.end() || found->size != size || cp < found->first)
        return false;
    const uint32_t index = found->index + (cp - found->first);
    record = {cp, found->size, advances[index], found->width, found->height, offsets[index]};
    return true;
}

bool FontIndex::open(const char* path) {
    close();
    file = std::fopen(path, "rb");
    if (!file) return false;
    char magic[8];
    uint32_t count = 0;
    if (std::fread(magic, 1, 8, file) == 8 && !std::memcmp(magic, "BMFONT01", 8) &&
        std::fread(&count, 4, 1, file) == 1 && count > 0 && count <= 200000) {
        try {
            if (read(count) && (dataOffset = std::ftell(file)) >= 0) return true;
        } catch (const std::bad_alloc&) {
            // Release any partially built index below.
        }
    }
    close();
    return false;
}

void FontIndex::close() {
    if (file) std::fclose(file);
    file = nullptr;
    std::vector<uint32_t>{}.swap(offsets);
    std::vector<uint8_t>{}.swap(advances);
    std::vector<GlyphRange>{}.swap(ranges);
}

bool FontIndex::pixels(const Record& record, unsigned char* out, size_t capacity) const {
    const size_t bytes = (record.width * record.height + 1) / 2;
    return bytes <= capacity && !std::fseek(file, dataOffset + record.offset, SEEK_SET) &&
           std::fread(out, 1, bytes, file) == bytes;
}

bool bodyRecord(uint32_t cp, int size, Record& record) {
    return bodyFont.find(cp, size, record) || (cp != '?' && bodyFont.find('?', size, record));
}
}

bool bitmapFontInit(const char* path) {
    bitmapFontShutdown();
    return bodyFont.open(path);
}

void bitmapFontShutdown() {
    bodyFont.close();
    for (auto& glyph : cache) glyph = BitmapGlyph{};
    for (auto& entry : front) entry = nullptr;
    nextSlot = 0;
}

const BitmapGlyph* bitmapGlyph(uint32_t cp, int size) {
    if (!bodyFont.file) return nullptr;
    const size_t slotHash = frontIndex(cp, size);
    if (const BitmapGlyph* hit = front[slotHash]; hit && hit->codepoint == cp && hit->size == size) return hit;
    for (const auto& glyph : cache)
        if (glyph.codepoint == cp && glyph.size == size) { front[slotHash] = &glyph; return &glyph; }
    Record record;
    if (!bodyRecord(cp, size, record)) return nullptr;
    BitmapGlyph& glyph = cache[nextSlot++ % 384];
    glyph = BitmapGlyph{};
    if (!bodyFont.pixels(record, glyph.alpha, sizeof(glyph.alpha))) return nullptr;
    glyph.codepoint = cp; glyph.size = size; glyph.advance = record.advance;
    glyph.width = record.width; glyph.height = record.height;
    front[slotHash] = &glyph;
    return &glyph;
}

int bitmapAdvance(uint32_t cp, int size) {
    Record record;
    return bodyRecord(cp, size, record) ? record.advance : size;
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

bool lyricFontInit(const char* path) { return lyricsFont.open(path); }
void lyricFontShutdown() { lyricsFont.close(); }

const LyricGlyph* lyricGlyph(uint32_t cp) {
    Record record;
    if (!lyricsFont.find(cp, kLyricFontSize, record) ||
        !lyricsFont.pixels(record, lyricsGlyph.alpha, sizeof(lyricsGlyph.alpha))) return nullptr;
    lyricsGlyph.advance = record.advance;
    lyricsGlyph.width = record.width;
    lyricsGlyph.height = record.height;
    return &lyricsGlyph;
}

int lyricAdvance(uint32_t cp) {
    Record record;
    return lyricsFont.find(cp, kLyricFontSize, record) ? record.advance : 0;
}
