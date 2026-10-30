#include "danmaku.hpp"
#include "network.hpp"
namespace pspapp {
std::vector<DanmakuComment> loadDanmaku(uint64_t cid) {
    if (!cid) return {};
    auto response = defaultHttpClient().get("https://api.bilibili.com/x/v1/dm/list.so?oid=" + std::to_string(cid),
                                            ErrorOperation::Request, false);
    if (!response) return {}; // Optional overlay never prevents video playback.
    return parseDanmaku(response.value.body);
}
}
