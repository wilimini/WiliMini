#include "../client/bitmap_font.hpp"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>

static bool failSeeks, failOpens, failReads;
static unsigned seeks, opens;
extern "C" int __real_fseek(FILE*, long, int);
extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" size_t __real_fread(void*, size_t, size_t, FILE*);
extern "C" int __wrap_fseek(FILE* file, long offset, int whence) {
    ++seeks;
    if (failSeeks) { errno = EIO; return -1; }
    return __real_fseek(file, offset, whence);
}
extern "C" FILE* __wrap_fopen(const char* path, const char* mode) {
    ++opens;
    if (failOpens) { errno = EIO; return nullptr; }
    return __real_fopen(path, mode);
}
extern "C" size_t __wrap_fread(void* out, size_t size, size_t count, FILE* file) {
    if (failReads) { errno = EIO; return 0; }
    return __real_fread(out, size, count, file);
}
int main(int argc, char** argv) {
    assert(argc == 2 && bitmapFontInit(argv[1]));
    const auto* glyph = bitmapGlyph('A', 14);
    assert(glyph);
    const BitmapGlyph expected = *glyph;
    bitmapFontShutdown();
    assert(bitmapFontInit(argv[1]));
    seeks = opens = 0;
    failSeeks = true;
    assert(!bitmapGlyph('A', 14));
    assert(seeks == 2 && opens == 1); // One retry, then release failed handle.
    failSeeks = false;
    failOpens = true;
    assert(!bitmapGlyph('A', 14));
    failOpens = false;
    glyph = bitmapGlyph('A', 14);
    assert(glyph && !std::memcmp(glyph, &expected, sizeof(expected)));
    bitmapFontShutdown();
    assert(bitmapFontInit(argv[1]));
    failReads = true;
    assert(!bitmapGlyph('A', 14));
    failReads = false;
    glyph = bitmapGlyph('A', 14);
    assert(glyph && !std::memcmp(glyph, &expected, sizeof(expected)));
    bitmapFontShutdown();
    std::puts("Font seek/read failure and reopen recovery passed");
}
