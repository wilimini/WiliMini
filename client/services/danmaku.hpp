#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace pspapp {
struct DanmakuComment {
    int64_t timeMs = 0;
    unsigned color = 0xffffff;
    int mode = 1;
    std::string text;
};
// Bounded ordinary XML comments only; unsupported advanced/script modes are ignored.
std::vector<DanmakuComment> parseDanmaku(std::string_view xml);
std::vector<DanmakuComment> loadDanmaku(uint64_t cid);
struct DanmakuLanes {
    int64_t gate[6]{}, end[6]{};
    int place(int64_t start, int width, int mode, int lanes);
};
}
