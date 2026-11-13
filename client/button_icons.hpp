#pragma once
#include <cstdint>

// Controller button icons embedded in text as private-use codepoints (U+E000..U+E023). Localized
// strings reference them as {icon_cross}, {icon_up_down}, ... (expanded by the localization
// generator); C++ literals use the BTN_* macros. The icons are drawn in code at the text size and
// tinted with the text colour, so no image assets are needed.
#define BTN_CROSS "\xEE\x80\x80"
#define BTN_CIRCLE "\xEE\x80\x81"
#define BTN_SQUARE "\xEE\x80\x82"
#define BTN_TRIANGLE "\xEE\x80\x83"
#define BTN_UP "\xEE\x80\x90"
#define BTN_DOWN "\xEE\x80\x91"
#define BTN_LEFT "\xEE\x80\x92"
#define BTN_RIGHT "\xEE\x80\x93"
#define BTN_UP_DOWN "\xEE\x80\x94"
#define BTN_LEFT_RIGHT "\xEE\x80\x95"
#define BTN_DPAD "\xEE\x80\x96"
#define BTN_L "\xEE\x80\xA0"
#define BTN_R "\xEE\x80\xA1"
#define BTN_START "\xEE\x80\xA2"
#define BTN_SELECT "\xEE\x80\xA3"

namespace pspapp {
constexpr uint32_t kButtonIconFirst = 0xE000;
constexpr int kButtonIconMaxWidth = 80;
constexpr int kButtonIconMaxSize = 24;
constexpr int kButtonIconMaxHeight = kButtonIconMaxSize + 4;

constexpr bool isButtonIcon(uint32_t cp) {
    return (cp >= 0xE000 && cp <= 0xE003) || (cp >= 0xE010 && cp <= 0xE016) || (cp >= 0xE020 && cp <= 0xE023);
}

// L, R, START and SELECT carry a text label drawn with the body font; nullptr for the others.
constexpr const char* buttonIconLabel(uint32_t cp) {
    return cp == 0xE020 ? "L" : cp == 0xE021 ? "R" : cp == 0xE022 ? "START" : cp == 0xE023 ? "SELECT" : nullptr;
}

// The icon box is as tall as a body-font glyph cell (size + 4) and is drawn at the same (x, y) as
// text, so it shares the text's baseline. Capital letters are `cap` pixels tall and centred on
// `mid`; icons are sized from those.
constexpr int buttonIconHeight(int size) { return size + 4; }
constexpr int buttonIconCap(int size) { return (size * 74 + 50) / 100; }
constexpr float buttonIconMid(int size) { return static_cast<float>(size) / 2.f + 1.5f; }

// Label letters are the body font scaled down so the pills stay close to the size of the text.
constexpr int kButtonLabelPercent = 85;
constexpr int buttonLabelWidth(int measured) { return (measured * kButtonLabelPercent + 50) / 100; }
constexpr float buttonLabelCap(int size) { return static_cast<float>(buttonIconCap(size)) * kButtonLabelPercent / 100.f; }

// `labelWidth` is buttonLabelWidth() of the body-font width of buttonIconLabel(cp) at `size` (0 for
// icons without a label).
constexpr int buttonIconWidth(uint32_t cp, int size, int labelWidth) {
    return (cp == 0xE020 || cp == 0xE021) ? labelWidth + 11
         : (cp == 0xE022 || cp == 0xE023) ? labelWidth + 11
         : buttonIconCap(size) + 2;
}
// Horizontal space the icon takes in a line of text.
constexpr int buttonIconAdvance(uint32_t cp, int size, int labelWidth) {
    return buttonIconWidth(cp, size, labelWidth) + 2;
}

// Source of the label letters: the body font, expanded to 8-bit coverage.
struct ButtonLabelFont {
    int (*measure)(const char* text, int size);
    // Fills `alpha` (at least (size + 2) * buttonIconHeight(size) bytes, row stride *width) with the
    // glyph cell for an ASCII letter and returns false if it is missing.
    bool (*glyph)(char letter, int size, int* advance, int* width, int* height, uint8_t* alpha);
};

// Renders the 8-bit coverage mask (row-major, buttonIconWidth x buttonIconHeight). Dim parts of a
// D-pad use partial alpha. `alpha` must hold kButtonIconMaxWidth * kButtonIconMaxHeight bytes;
// returns false for an unknown codepoint, unsupported size, or a label without a font.
bool renderButtonIcon(uint32_t cp, int size, uint8_t* alpha, const ButtonLabelFont* font);
} // namespace pspapp
