#pragma once
#include "../api/wiliwili/bilibili/util/json.hpp"
#include "network.hpp"
#include <utility>
#include <vector>

namespace pspapp {
using ApiParams = std::vector<std::pair<std::string, std::string>>;
struct ApiResponse {
    nlohmann::json data;
    std::string cookies;
};
class BilibiliApi {
  public:
    explicit BilibiliApi(HttpClient &http = defaultHttpClient()) : http_(http) {}
    Result<ApiResponse> get(const std::string &endpoint, const ApiParams &params,
                            bool signedRequest, ErrorOperation operation, bool retrySafe = true);
    Result<ApiResponse> post(const std::string &endpoint, const ApiParams &form,
                             ErrorOperation operation);

  private:
    HttpClient &http_;
};
std::string urlEncode(const std::string &text);
} // namespace pspapp
