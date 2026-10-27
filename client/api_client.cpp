#include <ctime>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

#include "bilibili.h"
#include "services/bilibili_api.hpp"
#include "api/wiliwili/bilibili.h"
#include "api/wiliwili/bilibili/api.h"
#include "api/wiliwili/bilibili/result/home_hots_all_result.h"
#include "api/wiliwili/bilibili/result/home_result.h"
#include "api/wiliwili/bilibili/result/search_result.h"
#include "api/wiliwili/bilibili/result/video_detail_result.h"
#include "api/wiliwili/bilibili/util/md5.hpp"

namespace bilibili {

Cookies BilibiliClient::cookies;

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

std::string percentEncode(const std::string& text) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string joinUrl(const std::string& endpoint, const Params& input) {
    std::string url = endpoint;
    if (url.rfind("//", 0) == 0) url = "https:" + url;
    char separator = url.find('?') == std::string::npos ? '?' : '&';
    for (const auto& [key, value] : input) {
        url.push_back(separator);
        separator = '&';
        url += percentEncode(key);
        url.push_back('=');
        url += percentEncode(value);
    }
    return url;
}

std::string extractWbiKey(const std::string& url) {
    const auto slash = url.find_last_of('/');
    const auto dot = url.find_last_of('.');
    if (slash == std::string::npos || dot == std::string::npos || slash >= dot) return {};
    return url.substr(slash + 1, dot - slash - 1);
}

std::string mixinKey(const std::string& imageKey, const std::string& subKey) {
    static constexpr int table[64] = {
        46, 47, 18, 2,  53, 8,  23, 32, 15, 50, 10, 31, 58, 3,  45, 35,
        27, 43, 5,  49, 33, 9,  42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
        37, 48, 7,  16, 24, 55, 40, 61, 26, 17, 0,  1,  60, 51, 30, 4,
        22, 25, 54, 21, 56, 59, 6,  63, 57, 62, 11, 36, 20, 34, 44, 52,
    };
    const std::string raw = imageKey + subKey;
    if (raw.size() < 64) return {};
    std::string result;
    result.reserve(32);
    for (int i = 0; i < 32; ++i) result.push_back(raw[static_cast<size_t>(table[i])]);
    return result;
}

std::mutex wbiMutex;
std::string wbiMixin;
std::time_t wbiUpdated = 0;

pspapp::Result<pspapp::ApiResponse> fetchJson(pspapp::HttpClient &http, const std::string &url,
                                              pspapp::ErrorOperation op, bool retrySafe) {
    auto response = http.get(url, op, retrySafe);
    if (!response)
        return pspapp::Result<pspapp::ApiResponse>::failure(response.error);
    try {
        auto root = nlohmann::json::parse(response.value.body);
        if (!root.is_object())
            throw std::runtime_error("Expected object");
        return {{std::move(root), std::move(response.value.cookies)}, {}};
    } catch (const std::exception &) {
        return pspapp::Result<pspapp::ApiResponse>::failure(
            {pspapp::ErrorDomain::Api, pspapp::ErrorKind::InvalidResponse, op});
    }
}
[[maybe_unused]] pspapp::AppError fetchWbiMixin(pspapp::HttpClient &http) {
    {
        std::lock_guard<std::mutex> lock(wbiMutex);
        if (!wbiMixin.empty() && std::time(nullptr) - wbiUpdated < 3600)
            return {};
    }
    auto response =
        fetchJson(http, joinUrl(Api::Nav, {}), pspapp::ErrorOperation::SignRequest, true);
    if (!response)
        return response.error;
    try {
        const auto &root = response.value.data;
        const int code = root.value("code", -1);
        if (code != 0 && code != -101)
            return {pspapp::ErrorDomain::Api, pspapp::ErrorKind::ApiRejected,
                    pspapp::ErrorOperation::SignRequest, code};
        const auto &image = root.at("data").at("wbi_img");
        auto mixin = mixinKey(extractWbiKey(image.at("img_url").get<std::string>()),
                              extractWbiKey(image.at("sub_url").get<std::string>()));
        if (mixin.size() != 32)
            throw std::runtime_error("Invalid WBI key");
        std::lock_guard<std::mutex> lock(wbiMutex);
        wbiMixin = std::move(mixin);
        wbiUpdated = std::time(nullptr);
        return {};
    } catch (const std::exception &) {
        return {pspapp::ErrorDomain::Api, pspapp::ErrorKind::InvalidResponse,
                pspapp::ErrorOperation::SignRequest};
    }
}

[[maybe_unused]] Params signWbi(const Params& input) {
    std::string mixin;
    {
        std::lock_guard<std::mutex> lock(wbiMutex);
        mixin = wbiMixin;
    }
    std::map<std::string, std::string> sorted;
    for (const auto& [key, value] : input) sorted[percentEncode(key)] = percentEncode(value);
    const std::string timestamp = std::to_string(std::time(nullptr));
    sorted["wts"] = timestamp;

    std::string query;
    for (const auto& [key, value] : sorted) {
        if (!query.empty()) query.push_back('&');
        query += key;
        query.push_back('=');
        query += value;
    }
    query.erase(std::remove_if(query.begin(), query.end(), [](char c) {
        return c == '!' || c == '\'' || c == '(' || c == ')' || c == '*';
    }), query.end());

    Params output = input;
    output.emplace_back("wts", timestamp);
    output.emplace_back("w_rid", websocketpp::md5::md5_hash_hex(query + mixin));
    return output;
}

pspapp::Result<pspapp::ApiResponse> requestDocument(pspapp::HttpClient &http,
                                                    const std::string &endpoint,
                                                    const Params &params, bool signedRequest,
                                                    pspapp::ErrorOperation op, bool retrySafe) {
    using namespace pspapp;
    Result<ApiResponse> response;
#ifdef BILI_DEVICE_OFFLINE_UI
    (void)http;
    (void)signedRequest;
    (void)retrySafe;
    auto parameter = [&](const char *key) {
        for (const auto &p : params)
            if (p.first == key)
                return p.second;
        return std::string();
    };
    std::string name;
    if (endpoint == Api::Detail)
        name = "detail_" + parameter("bvid");
    else if (endpoint == Api::Comment)
        name = "comments_" + parameter("oid") + "_" + parameter("next");
    else if (endpoint == Api::SearchHots)
        name = "search_topics";
    else if (endpoint == Api::Search)
        name = "search_" + percentEncode(parameter("keyword")) + "_" + parameter("page");
    else if (endpoint == Api::QrLoginUrlV2)
        name = "login_generate";
    else if (endpoint == Api::QrLoginInfoV2)
        name = "login_poll";
    else if (endpoint == Api::Nav)
        name = "session";
    if (name.empty()) name = "api_" + websocketpp::md5::md5_hash_hex(joinUrl(endpoint, params));
    if (name.size() > 240 ||
        name.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_%") !=
            std::string::npos)
        return Result<ApiResponse>::failure({ErrorDomain::Api, ErrorKind::InvalidResponse, op});
    FILE *file = std::fopen((std::string(CLIENT_ROOT) + "/cache/" + name + ".json").c_str(), "rb");
    if (!file)
        return Result<ApiResponse>::failure({ErrorDomain::Storage, ErrorKind::IoFailure, op});
    std::fseek(file, 0, SEEK_END);
    long length = std::ftell(file);
    std::rewind(file);
    std::string body(length > 0 && length <= 512 * 1024 ? length : 0, '\0');
    bool read = !body.empty() && std::fread(&body[0], 1, body.size(), file) == body.size();
    std::fclose(file);
    try {
        if (!read)
            throw std::runtime_error("Invalid fixture");
        response.value.data = nlohmann::json::parse(body);
    } catch (const std::exception &) {
        return Result<ApiResponse>::failure({ErrorDomain::Api, ErrorKind::InvalidResponse, op});
    }
#else
    if (signedRequest) {
        auto error = fetchWbiMixin(http);
        if (error)
            return Result<ApiResponse>::failure(std::move(error));
    }
    response =
        fetchJson(http, joinUrl(endpoint, signedRequest ? signWbi(params) : params), op, retrySafe);
    if (!response)
        return response;
    // One refresh for a rejected WBI signature; transport/API failures keep their own codes.
    if (signedRequest && response.value.data.value("code", 0) == -403) {
        {
            std::lock_guard<std::mutex> lock(wbiMutex);
            wbiMixin.clear();
        }
        auto error = fetchWbiMixin(http);
        if (error)
            return Result<ApiResponse>::failure(std::move(error));
        response = fetchJson(http, joinUrl(endpoint, signWbi(params)), op, retrySafe);
        if (!response)
            return response;
    }
#endif
    try {
        const auto &root = response.value.data;
        int code = root.at("code").get<int>();
        if (code != 0)
            return Result<ApiResponse>::failure(
                {code == -101 ? ErrorDomain::Auth : ErrorDomain::Api,
                 code == -101 ? ErrorKind::LoginRequired : ErrorKind::ApiRejected, op, code});
        nlohmann::json data;
        if (root.contains("data"))
            data = root.at("data");
        else if (root.contains("result"))
            data = root.at("result");
        else
            throw std::runtime_error("Missing data");
        response.value.data = std::move(data);
        return response;
    } catch (const std::exception &) {
        return Result<ApiResponse>::failure({ErrorDomain::Api, ErrorKind::InvalidResponse, op});
    }
}

template <typename Result, typename Callback>
bool requestResult(const std::string &endpoint, const Params &params, bool signedRequest,
                   const Callback &callback, const ErrorCallback &errorCallback) {
    auto response =
        pspapp::BilibiliApi{}.get(endpoint, params, signedRequest, pspapp::ErrorOperation::Request);
    if (!response) {
        if (errorCallback) {
            auto displayError = response.error;
            displayError.rawCode = 0;
            errorCallback(pspapp::errorMessage(displayError), response.error.rawCode);
        }
        return false;
    }
    try {
        callback(response.value.data.template get<Result>());
        return true;
    } catch (const std::exception &) {
        if (errorCallback)
            errorCallback("服务器响应无法识别", -1);
        return false;
    }
}

void videoUrlRequest(const Params& videoParams, int qn, const std::function<void(VideoUrlResult)>& callback,
                     const ErrorCallback& error) {
    Params params = videoParams;
    params.emplace_back("gaia_source", "view-card");
    params.emplace_back("from_client", "BROWSER");
    params.emplace_back("is_main_page", "false");
    params.emplace_back("need_fragment", "false");
    params.emplace_back("isGaiaAvoided", "true");
    params.emplace_back("voice_balance", "1");
    params.emplace_back("web_location", "1315873");
    params.emplace_back("qn", std::to_string(qn));
    params.emplace_back("fourk", "1");
    params.emplace_back("fnval", BilibiliClient::FNVAL);
    params.emplace_back("fnver", "0");
    requestResult<VideoUrlResult>(Api::PlayUrl2, params, true, callback, error);
}

}  // namespace

void BilibiliClient::get_recommend(int index, int num, int freshType, std::string feedVersion, int xNum, int yNum,
                                   const std::function<void(RecommendVideoListResultWrapper)>& callback,
                                   const ErrorCallback& error) {
    Params params = {
        {"fresh_idx", std::to_string(index)},
        {"ps", std::to_string(num)},
        {"feed_version", feedVersion},
        {"fresh_type", std::to_string(freshType)},
        {"plat", "1"},
    };
    if (feedVersion == "V1") {
        params.emplace_back("x_num", std::to_string(xNum));
        params.emplace_back("y_num", std::to_string(yNum));
    }
    requestResult<RecommendVideoListResultWrapper>(
        Api::Recommend, params, true,
        [callback, index](RecommendVideoListResultWrapper result) {
            result.requestIndex = index;
            if (callback) callback(result);
        },
        error);
}

void BilibiliClient::get_hots_all(int index, int num,
                                  const std::function<void(HotsAllVideoListResult, bool)>& callback,
                                  const ErrorCallback& error) {
    requestResult<HotsAllVideoListResultWrapper>(
        Api::HotsAll,
        {{"pn", std::to_string(index)}, {"ps", std::to_string(num)}, {"web_location", "bilibili-electron"}},
        true,
        [callback](const HotsAllVideoListResultWrapper& result) {
            if (callback) callback(result.list, result.no_more);
        },
        error);
}

void BilibiliClient::get_video_detail(uint64_t aid,
                                      const std::function<void(VideoDetailResult)>& callback,
                                      const ErrorCallback& error) {
    requestResult<VideoDetailResult>(Api::Detail, {{"aid", std::to_string(aid)}}, false, callback, error);
}

void BilibiliClient::get_video_detail(const std::string& bvid,
                                      const std::function<void(VideoDetailResult)>& callback,
                                      const ErrorCallback& error) {
    if (bvid.size() > 2 && bvid.compare(0, 2, "av") == 0 &&
        bvid.find_first_not_of("0123456789", 2) == std::string::npos) {
        requestResult<VideoDetailResult>(Api::Detail, {{"aid", bvid.substr(2)}}, false, callback, error);
        return;
    }
    requestResult<VideoDetailResult>(Api::Detail, {{"bvid", bvid}}, false, callback, error);
}

void BilibiliClient::get_video_url(const std::string& bvid, uint64_t cid, int qn,
                                   const std::function<void(VideoUrlResult)>& callback,
                                   const ErrorCallback& error) {
    videoUrlRequest({{"bvid", bvid}, {"cid", std::to_string(cid)}}, qn, callback, error);
}

void BilibiliClient::get_video_url(uint64_t aid, uint64_t cid, int qn,
                                   const std::function<void(VideoUrlResult)>& callback,
                                   const ErrorCallback& error) {
    videoUrlRequest({{"aid", std::to_string(aid)}, {"cid", std::to_string(cid)}}, qn, callback, error);
}

void BilibiliClient::search_video(const std::string& key, const std::string& searchType, unsigned int index,
                                  const std::string& order,
                                  const std::function<void(SearchResult)>& callback,
                                  const ErrorCallback& error) {
    requestResult<SearchResult>(
        Api::Search,
        {{"__refresh__", "true"},
         {"_extra", ""},
         {"context", ""},
         {"page", std::to_string(index)},
         {"page_size", "20"},
         {"order", order},
         {"duration", ""},
         {"from_source", ""},
         {"from_spmid", "333.337"},
         {"platform", "pc"},
         {"device", "mac"},
         {"highlight", "1"},
         {"single_column", "0"},
         {"keyword", key},
         {"category_id", ""},
         {"search_type", searchType},
         {"dynamic_offset", "0"}},
        true, callback, error);
}

void BilibiliClient::get_search_hots(int limit,
                                     const std::function<void(SearchHotsResultWrapper)>& callback,
                                     const ErrorCallback& error) {
    requestResult<SearchHotsResultWrapper>(Api::SearchHots, {{"limit", std::to_string(limit)}}, false,
                                          callback, error);
}

void BilibiliClient::get_comment(const std::string& oid, int next, int mode, int type,
                                 const std::function<void(VideoCommentResultWrapper)>& callback,
                                 const ErrorCallback& error) {
    requestResult<VideoCommentResultWrapper>(
        Api::Comment,
        {{"mode", std::to_string(mode)}, {"next", std::to_string(next)}, {"oid", oid},
         {"plat", "1"}, {"type", std::to_string(type)}},
        false,
        [callback, next](VideoCommentResultWrapper result) {
            result.requestIndex = static_cast<size_t>(next);
            if (callback) callback(result);
        },
        error);
}

void BilibiliClient::init(Cookies& initialCookies, std::function<void(Cookies, std::string)> writeCookiesCallback) {
    cookies = initialCookies;
    BilibiliClient::writeCookiesCallback = std::move(writeCookiesCallback);
}

}  // namespace bilibili

namespace pspapp {
std::string urlEncode(const std::string &text) {
    return bilibili::percentEncode(text);
}
Result<ApiResponse> BilibiliApi::get(const std::string &endpoint, const ApiParams &params,
                                     bool signedRequest, ErrorOperation operation, bool retrySafe) {
    try {
        return bilibili::requestDocument(http_, endpoint, params, signedRequest, operation,
                                         retrySafe);
    } catch (const std::exception &) {
        return Result<ApiResponse>::failure(
            {ErrorDomain::Api, ErrorKind::InvalidResponse, operation});
    }
}
Result<ApiResponse> BilibiliApi::post(const std::string &endpoint, const ApiParams &form,
                                      ErrorOperation operation) {
    std::string url = endpoint.rfind("//", 0) == 0 ? "https:" + endpoint : endpoint;
    if (!bili_http_credential_host(url.c_str()))
        return Result<ApiResponse>::failure({ErrorDomain::Api, ErrorKind::InvalidResponse, operation});
#ifdef BILI_DEVICE_OFFLINE_UI
    // Offline UI builds never issue writes to a real account.
    return Result<ApiResponse>::failure({ErrorDomain::Api, ErrorKind::ApiRejected, operation});
#else
    std::string body;
    for (const auto &field : form) {
        if (!body.empty()) body += '&';
        body += urlEncode(field.first) + "=" + urlEncode(field.second);
    }
    // A timeout after a write has an unknown outcome. Never automatically retry it.
    auto response = http_.post(url, body, operation);
    if (!response) return Result<ApiResponse>::failure(response.error);
    try {
        auto root = nlohmann::json::parse(response.value.body);
        int code = root.at("code").get<int>();
        if (code != 0)
            return Result<ApiResponse>::failure({code == -101 ? ErrorDomain::Auth : ErrorDomain::Api,
                code == -101 ? ErrorKind::LoginRequired : ErrorKind::ApiRejected, operation, code});
        return {{root.value("data", nlohmann::json()), {}}, {}};
    } catch (const std::exception &) {
        return Result<ApiResponse>::failure({ErrorDomain::Api, ErrorKind::InvalidResponse, operation});
    }
#endif
}
} // namespace pspapp
