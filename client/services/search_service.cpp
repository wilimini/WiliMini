#include "search_service.hpp"
#include "../api/wiliwili/bilibili/api.h"

namespace pspapp {
Result<SearchPage> SearchService::search(const SearchRequest &request) {
    if (request.keyword.empty() || request.page == 0)
        return Result<SearchPage>::failure(
            {ErrorDomain::Api, ErrorKind::InvalidResponse, ErrorOperation::Search});
    auto response = api_.get(bilibili::Api::Search,
                             {{"keyword", request.keyword},
                              {"page", std::to_string(request.page)},
                              {"page_size", "20"},
                              {"search_type", "video"},
                              {"order", ""},
                              {"platform", "pc"},
                              {"__refresh__", "true"},
                              {"highlight", "1"}},
                             true, ErrorOperation::Search);
    if (!response)
        return Result<SearchPage>::failure(response.error);
    try {
        const auto &data = response.value.data;
        for (const char *field : {"page", "pagesize", "numResults", "numPages"})
            if (!data.contains(field) || !data.at(field).is_number_integer() ||
                data.at(field).get<int64_t>() < 0)
                throw std::runtime_error("Invalid search pagination");
        if (data.at("page").get<unsigned>() != request.page ||
            (data.contains("result") && !data.at("result").is_array()))
            throw std::runtime_error("Invalid search page");
        auto result = data.get<bilibili::SearchResult>();
        SearchPage out;
        out.request = request;
        out.hasMore = result.page < result.numPages && !result.result.empty();
        for (auto &video : result.result)
            if (!video.bvid.empty() && !video.notSupported)
                out.videos.push_back(std::move(video));
        return {std::move(out), {}};
    } catch (const std::exception &) {
        return Result<SearchPage>::failure(
            {ErrorDomain::Api, ErrorKind::InvalidResponse, ErrorOperation::Search});
    }
}
Result<std::vector<SearchTopic>> SearchService::topics() {
    auto response =
        api_.get(bilibili::Api::SearchHots, {{"limit", "20"}}, false, ErrorOperation::SearchTopics);
    if (!response)
        return Result<std::vector<SearchTopic>>::failure(response.error);
    try {
        if (!response.value.data.at("trending").at("list").is_array())
            throw std::runtime_error("Invalid topics");
        auto result = response.value.data.get<bilibili::SearchHotsResultWrapper>();
        std::vector<SearchTopic> topics;
        for (const auto &item : result.list) {
            auto keyword = item.keyword.empty() ? item.show_name : item.keyword;
            if (!keyword.empty())
                topics.push_back({keyword, item.show_name.empty() ? keyword : item.show_name});
            if (topics.size() == 20)
                break;
        }
        return {std::move(topics), {}};
    } catch (const std::exception &) {
        return Result<std::vector<SearchTopic>>::failure(
            {ErrorDomain::Api, ErrorKind::InvalidResponse, ErrorOperation::SearchTopics});
    }
}
} // namespace pspapp
