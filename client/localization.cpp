#include "localization.hpp"
#include "button_icons.hpp"

namespace pspapp {
namespace {
Locale gActiveLocale = Locale::ZhHans;
} // namespace

Locale currentLocale() {
    return gActiveLocale;
}

void setCurrentLocale(Locale locale) {
    gActiveLocale = locale;
}

const char* text(TextId id, Locale locale) {
    const auto index = static_cast<std::size_t>(id);
    return locale == Locale::EnUs ? kTranslationsEnUs[index] : kTranslationsZhHans[index];
}

const char* tr(TextId id) {
    return text(id, gActiveLocale);
}

std::string formatText(TextId id, Locale locale, std::initializer_list<TextArg> args) {
    std::string result = text(id, locale);
    for (const auto& arg : args) {
        const std::string token = "{" + std::string(arg.name) + "}";
        size_t position = 0;
        while ((position = result.find(token, position)) != std::string::npos) {
            result.replace(position, token.size(), arg.value);
            position += arg.value.size();
        }
    }
    return result;
}

std::string formatTr(TextId id, std::initializer_list<TextArg> args) {
    return formatText(id, gActiveLocale, args);
}

const char* buttonGlyph(ControllerButton button) {
    return button == ControllerButton::Circle ? BTN_CIRCLE : BTN_CROSS;
}

const char* localeCode(Locale locale) {
    return locale == Locale::EnUs ? "en-US" : "zh-Hans";
}
} // namespace pspapp
