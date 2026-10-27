#pragma once
#include "../api/wiliwili/bilibili/result/search_result.h"
#include "bilibili_api.hpp"
#include <cstdint>

namespace pspapp {
struct SearchRequest {
    std::string keyword;
    unsigned page = 1;
    uint64_t generation = 0;
};
struct SearchPage {
    SearchRequest request;
    std::vector<bilibili::VideoItemSearchResult> videos;
    bool hasMore = false;
};
struct SearchTopic {
    std::string keyword, title;
};
class SearchService {
  public:
    explicit SearchService(BilibiliApi &api) : api_(api) {}
    Result<SearchPage> search(const SearchRequest &request);
    Result<std::vector<SearchTopic>> topics();

  private:
    BilibiliApi &api_;
};
} // namespace pspapp
