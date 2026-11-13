#include "../client/button_icons.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace pspapp;

namespace {
const uint32_t kAll[] = {0xE000, 0xE001, 0xE002, 0xE003, 0xE010, 0xE011, 0xE012, 0xE013, 0xE014, 0xE015,
                         0xE016, 0xE020, 0xE021, 0xE022, 0xE023};

// Stand-in for the body font: every letter is a solid cap-height block, advance size / 2.
int fakeMeasure(const char* text, int size) { return static_cast<int>(std::strlen(text)) * (size / 2); }
bool fakeGlyph(char, int size, int* advance, int* width, int* height, uint8_t* alpha) {
    *advance = size / 2; *width = size + 2; *height = buttonIconHeight(size);
    std::memset(alpha, 0, static_cast<size_t>(*width) * *height);
    const int cap = buttonIconCap(size), top = static_cast<int>(buttonIconMid(size) - cap / 2.f);
    for (int y = top; y < top + cap; ++y)
        for (int x = 1; x < size / 2 - 1; ++x) alpha[y * *width + x] = 255;
    return true;
}
const ButtonLabelFont kFont{fakeMeasure, fakeGlyph};

int labelWidth(uint32_t cp, int size) {
    const char* label = buttonIconLabel(cp);
    return label ? buttonLabelWidth(fakeMeasure(label, size)) : 0;
}
} // namespace

int main(int argc, char** argv) {
    for (int size : {12, 14, 16}) {
        const int h = buttonIconHeight(size), cap = buttonIconCap(size);
        assert(h == size + 4 && cap >= 9 && cap <= 13);
        for (uint32_t cp : kAll) {
            assert(isButtonIcon(cp));
            const int w = buttonIconWidth(cp, size, labelWidth(cp, size));
            assert(w > 0 && w <= kButtonIconMaxWidth);
            assert(buttonIconAdvance(cp, size, labelWidth(cp, size)) == w + 2);
            std::vector<uint8_t> alpha(static_cast<size_t>(kButtonIconMaxWidth) * kButtonIconMaxHeight, 0);
            assert(renderButtonIcon(cp, size, alpha.data(), &kFont));
            int rows = 0, firstRow = h, lastRow = -1;
            for (int y = 0; y < h; ++y) {
                bool any = false;
                for (int x = 0; x < w; ++x) any |= alpha[y * w + x] > 127;
                if (any) { ++rows; firstRow = y < firstRow ? y : firstRow; lastRow = y; }
            }
            assert(rows >= 2);                                        // the icon is drawn...
            assert(lastRow - firstRow + 1 <= cap + 5);                // ...and not taller than a pill
            if (cp < 0xE010 || cp > 0xE016) {                         // ...centred on the text midline
                assert(rows >= cap - 3);
                const float centre = (firstRow + lastRow + 1) / 2.f;
                assert(centre > buttonIconMid(size) - 1.01f && centre < buttonIconMid(size) + 1.01f);
            }
        }
        // Labelled icons need a font; plain ones do not.
        uint8_t scratch[kButtonIconMaxWidth * kButtonIconMaxHeight];
        assert(!renderButtonIcon(0xE022, size, scratch, nullptr) && renderButtonIcon(0xE000, size, scratch, nullptr));
    }
    uint8_t scratch[kButtonIconMaxWidth * kButtonIconMaxHeight];
    assert(!isButtonIcon('A') && !isButtonIcon(0xE004) && !isButtonIcon(0xE017) && !isButtonIcon(0xE024));
    assert(!renderButtonIcon('A', 12, scratch, &kFont) && !renderButtonIcon(0xE000, 4, scratch, &kFont));
    assert(std::strcmp(BTN_CROSS, "\xEE\x80\x80") == 0 && std::strlen(BTN_SELECT) == 3);
    // The D-pad highlight follows the direction.
    const int w = buttonIconWidth(0xE010, 12, 0), mid = static_cast<int>(buttonIconMid(12));
    std::vector<uint8_t> up(static_cast<size_t>(kButtonIconMaxWidth) * kButtonIconMaxHeight),
        down(up.size());
    assert(renderButtonIcon(0xE010, 12, up.data(), &kFont) && renderButtonIcon(0xE011, 12, down.data(), &kFont));
    assert(up[(mid - 4) * w + w / 2] > 200 && down[(mid - 4) * w + w / 2] < 200);
    assert(down[(mid + 3) * w + w / 2] > 200 && up[(mid + 3) * w + w / 2] < 200);
    assert(up[mid * w + w / 2] < 150 && down[mid * w + w / 2] < 150);  // the centre never lights up

    if (argc > 1) {  // dump a coarse preview of every icon at the given size
        const int size = std::atoi(argv[1]), h = buttonIconHeight(size);
        for (uint32_t cp : kAll) {
            std::vector<uint8_t> alpha(static_cast<size_t>(kButtonIconMaxWidth) * kButtonIconMaxHeight, 0);
            renderButtonIcon(cp, size, alpha.data(), &kFont);
            const int w2 = buttonIconWidth(cp, size, labelWidth(cp, size));
            std::printf("U+%04X\n", cp);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w2; ++x) std::putchar(" .:*#"[alpha[y * w2 + x] / 52]);
                std::putchar('\n');
            }
        }
    }
    std::puts("button_icons_test passed");
    return 0;
}
