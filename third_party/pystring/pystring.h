#pragma once
// Minimal pystring-compatible helpers required by the unmodified wiliwili result headers.
#include <algorithm>
#include <string>
#include <vector>

namespace pystring {
inline bool startswith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}
inline std::string replace(const std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) return text;
    std::string out = text;
    std::string::size_type pos = 0;
    while ((pos = out.find(from, pos)) != std::string::npos) {
        out.replace(pos, from.size(), to);
        pos += to.size();
    }
    return out;
}
inline void split(const std::string& text, std::vector<std::string>& out, const std::string& delimiter) {
    out.clear();
    if (delimiter.empty()) {
        out.push_back(text);
        return;
    }
    std::string::size_type start = 0;
    for (;;) {
        const auto end = text.find(delimiter, start);
        if (end == std::string::npos) {
            out.push_back(text.substr(start));
            return;
        }
        out.push_back(text.substr(start, end - start));
        start = end + delimiter.size();
    }
}
}  // namespace pystring
