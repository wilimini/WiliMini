#include "danmaku.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace pspapp {
namespace {
void appendUtf8(std::string& text, unsigned cp) {
    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp < 32) return;
    if (cp < 0x80) text += char(cp);
    else if (cp < 0x800) { text += char(0xc0 | cp >> 6); text += char(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { text += char(0xe0 | cp >> 12); text += char(0x80 | (cp >> 6 & 63)); text += char(0x80 | (cp & 63)); }
    else { text += char(0xf0 | cp >> 18); text += char(0x80 | (cp >> 12 & 63)); text += char(0x80 | (cp >> 6 & 63)); text += char(0x80 | (cp & 63)); }
}
std::string decodeText(std::string_view input) {
    std::string text;
    for (size_t i = 0, count = 0; i < input.size() && count < 28; ++count) {
        unsigned char first = input[i];
        if (first == '&') {
            auto end = input.find(';', i + 1);
            if (end == input.npos || end - i > 12) return {};
            const auto entity = input.substr(i + 1, end - i - 1);
            if (entity == "amp") text += '&';
            else if (entity == "lt") text += '<';
            else if (entity == "gt") text += '>';
            else if (entity == "quot") text += '"';
            else if (entity == "apos") text += '\'';
            else if (!entity.empty() && entity[0] == '#') {
                const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                std::string digits(entity.substr(hex ? 2 : 1));
                char* tail = nullptr;
                unsigned long cp = std::strtoul(digits.c_str(), &tail, hex ? 16 : 10);
                if (digits.empty() || *tail || cp > 0x10ffff) return {};
                appendUtf8(text, static_cast<unsigned>(cp));
            } else return {};
            i = end + 1;
        } else {
            size_t bytes = first < 0x80 ? 1 : first >= 0xc2 && first < 0xe0 ? 2 : first < 0xf0 && first >= 0xe0 ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
            if (!bytes || i + bytes > input.size() || first == '<') return {};
            for (size_t j = 1; j < bytes; ++j) if ((static_cast<unsigned char>(input[i+j]) & 0xc0) != 0x80) return {};
            if (bytes >= 3) {
                const unsigned char second = input[i + 1];
                if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
                    (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return {};
            }
            if (first >= 32) text.append(input.substr(i, bytes));
            i += bytes;
        }
    }
    return text;
}
}
std::vector<DanmakuComment> parseDanmaku(std::string_view xml) {
    std::vector<DanmakuComment> result;
    if (xml.size() > 512 * 1024 || xml.find("<!DOCTYPE") != xml.npos || xml.find("<!ENTITY") != xml.npos) return result;
    size_t pos = 0;
    while (result.size() < 4096 && (pos = xml.find("<d ", pos)) != xml.npos) {
        const size_t tag = xml.find('>', pos), end = xml.find("</d>", tag);
        if (tag == xml.npos || end == xml.npos) break;
        const auto head = xml.substr(pos, tag - pos);
        pos = end + 4;
        const auto attr = head.find("p=");
        if (attr == head.npos || attr + 3 >= head.size()) continue;
        const char quote = head[attr + 2];
        if (quote != '"' && quote != '\'') continue;
        const auto finish = head.find(quote, attr + 3);
        if (finish == head.npos || finish - attr > 256) continue;
        std::string params(head.substr(attr + 3, finish - attr - 3));
        char* tail = nullptr;
        double seconds = std::strtod(params.c_str(), &tail);
        if (tail == params.c_str() || !std::isfinite(seconds) || seconds < 0 || seconds > 86400 || *tail != ',') continue;
        long mode = std::strtol(tail + 1, &tail, 10);
        if ((mode != 1 && mode != 4 && mode != 5) || *tail != ',') continue;
        std::strtol(tail + 1, &tail, 10); // Source font size; normalize for PSP.
        if (*tail != ',') continue;
        unsigned long color = std::strtoul(tail + 1, &tail, 10);
        if (color > 0xffffff || (*tail && *tail != ',')) continue;
        auto text = decodeText(xml.substr(tag + 1, end - tag - 1));
        if (!text.empty()) result.push_back({static_cast<int64_t>(seconds * 1000), static_cast<unsigned>(color), static_cast<int>(mode), std::move(text)});
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.timeMs < b.timeMs; });
    return result;
}
int DanmakuLanes::place(int64_t start, int width, int mode, int lanes) {
    lanes = std::clamp(lanes, 1, 6);
    for (int i = 0; i < lanes; ++i) {
        int lane = mode == 4 ? lanes - 1 - i : i;
        if (start < (mode == 1 ? gate[lane] : end[lane])) continue;
        end[lane] = start + (mode == 1 ? (480 + width) * 1000 / 60 : 4000);
        gate[lane] = mode == 1 ? start + (width + 16) * 1000 / 60 : end[lane];
        return lane;
    }
    return -1;
}
}
