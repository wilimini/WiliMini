#include "../client/bitmap_font.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
struct Record {
    uint32_t cp;
    uint8_t size, advance, width, height;
    uint32_t offset;
};
static_assert(sizeof(Record) == 12);

void checkGlyph(FILE* file, long pixels, const Record& record, uint32_t cp) {
    const auto* glyph = bitmapGlyph(cp, record.size);
    assert(glyph && glyph->codepoint == cp && glyph->size == record.size);
    assert(glyph->advance == record.advance && bitmapAdvance(cp, record.size) == record.advance);
    assert(glyph->width == record.width && glyph->height == record.height);
    unsigned char expected[sizeof(glyph->alpha)];
    const size_t bytes = (record.width * record.height + 1) / 2;
    assert(bytes <= sizeof(expected));
    assert(std::fseek(file, pixels + record.offset, SEEK_SET) == 0);
    assert(std::fread(expected, 1, bytes, file) == bytes);
    assert(std::memcmp(glyph->alpha, expected, bytes) == 0);
}
}

int main(int argc, char** argv) {
    assert(argc == 2);
    FILE* file = std::fopen(argv[1], "rb");
    assert(file && bitmapFontInit(argv[1]));
    char magic[8];
    uint32_t count = 0;
    assert(std::fread(magic, 1, sizeof(magic), file) == sizeof(magic));
    assert(std::memcmp(magic, "BMFONT01", sizeof(magic)) == 0);
    assert(std::fread(&count, sizeof(count), 1, file) == 1 && count > 0 && count <= 200000);
    std::vector<Record> records(count);
    assert(std::fread(records.data(), sizeof(Record), count, file) == count);
    const long pixels = std::ftell(file);
    assert(pixels >= 0);

    for (const auto& record : records) checkGlyph(file, pixels, record, record.cp);
    for (const auto& record : records) {
        if (record.cp != '?') continue;
        checkGlyph(file, pixels, record, 0x007f);  // Between the ASCII and Latin ranges.
        checkGlyph(file, pixels, record, 0x10ffff);
    }
    assert(bitmapAdvance('A', 99) == 99 && !bitmapGlyph('A', 99));
    bitmapFontShutdown();
    assert(bitmapAdvance('A', 14) == 14 && !bitmapGlyph('A', 14));
    assert(bitmapFontInit(argv[1]));
    checkGlyph(file, pixels, records.front(), records.front().cp);
    bitmapFontShutdown();
    std::fclose(file);
    std::printf("Bitmap font index: %u glyphs and fallbacks passed\n", count);
}
