#include "title_font.hpp"
#include "bitmap_font.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
struct Record {
    uint32_t cp;
    uint8_t size, advance, width, height;
    uint32_t offset;
};
static_assert(sizeof(Record) == 12, "Font index layout");
std::vector<Record> titleIndex;
std::vector<unsigned char> titlePixels;
TitleGlyph titleCache;
}

bool titleFontInit(const char* path) {
    titleFontShutdown();
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    char magic[8];
    uint32_t count = 0;
    bool ok = std::fread(magic, 1, 8, file) == 8 && !std::memcmp(magic, "BMFONT01", 8) &&
              std::fread(&count, 4, 1, file) == 1 && count > 0 && count < 4096;
    if (ok) {
        titleIndex.resize(count);
        ok = std::fread(titleIndex.data(), sizeof(Record), count, file) == count;
    }
    if (ok) {
        const long start = std::ftell(file);
        std::fseek(file, 0, SEEK_END);
        const long end = std::ftell(file);
        ok = start > 0 && end > start && end - start < 512 * 1024;
        if (ok) {
            titlePixels.resize(static_cast<size_t>(end - start));
            std::fseek(file, start, SEEK_SET);
            ok = std::fread(titlePixels.data(), 1, titlePixels.size(), file) == titlePixels.size();
        }
    }
    std::fclose(file);
    if (!ok) titleFontShutdown();
    return ok;
}

void titleFontShutdown() {
    titleIndex.clear();
    titleIndex.shrink_to_fit();
    titlePixels.clear();
    titlePixels.shrink_to_fit();
}

bool titleFontReady() { return !titleIndex.empty(); }

const TitleGlyph* titleGlyph(uint32_t cp) {
    const auto found = std::lower_bound(titleIndex.begin(), titleIndex.end(), cp,
                                        [](const Record& r, uint32_t key) { return r.cp < key; });
    if (found == titleIndex.end() || found->cp != cp) return nullptr;
    const size_t bytes = (static_cast<size_t>(found->width) * found->height + 1) / 2;
    if (found->offset + bytes > titlePixels.size()) return nullptr;
    titleCache = {found->advance, found->width, found->height, titlePixels.data() + found->offset};
    return &titleCache;
}

int titleMeasure(std::string_view text) {
    int width = 0;
    for (size_t pos = 0; pos < text.size();) {
        const TitleGlyph* glyph = titleGlyph(bitmapNext(text, pos));
        if (!glyph) return -1;
        width += glyph->advance;
    }
    return width;
}
