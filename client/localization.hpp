#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

namespace pspapp {
enum class Locale : std::uint8_t { ZhHans, EnUs };
enum class ControllerButton : std::uint8_t { Cross, Circle };
} // namespace pspapp

#include "localization.generated.hpp"

namespace pspapp {
struct TextArg {
    const char* name;
    std::string value;
};

const char* text(TextId id, Locale locale);
std::string formatText(TextId id, Locale locale, std::initializer_list<TextArg> args);
const char* buttonGlyph(ControllerButton button);
const char* localeCode(Locale locale);
} // namespace pspapp
