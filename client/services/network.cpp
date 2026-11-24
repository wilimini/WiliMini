#include "network.hpp"
#include "playback_request_policy.h"
#include "../bilibili.h"
#include <cstdio>

namespace pspapp {
HttpResponse HttpResponse::fromSpillFile(std::string path, size_t bytes, bool autoDelete) {
    HttpResponse out;
    out.bodyPath = std::move(path);
    out.bodyBytes = bytes;
    if (autoDelete && !out.bodyPath.empty()) {
        auto *owned = new std::string(out.bodyPath);
        out.bodyFileGuard = std::shared_ptr<const void>(owned, [](const void *ptr) {
            const auto *p = static_cast<const std::string *>(ptr);
            std::remove(p->c_str());
            delete p;
        });
    }
    return out;
}
AppError WifiManager::initialize() const {
    const int result = network_init();
    return result < 0 ? AppError{ErrorDomain::Network, ErrorKind::Disconnected,
                                 ErrorOperation::ConnectWifi, result}
                      : AppError{};
}
bool WifiManager::ready() const {
    return network_ready() != 0;
}
AppError WifiManager::connect() const {
    const int result = network_connect();
    if (result >= 0)
        return {};
    return {ErrorDomain::Network, transfer_cancel ? ErrorKind::Cancelled : ErrorKind::Disconnected,
            ErrorOperation::ConnectWifi, result};
}
static AppError mapHttpError(const BiliHttpResponse &response, ErrorOperation op) {
    if (!response.failure) return {};
    ErrorKind kind = ErrorKind::Unknown;
    switch (response.failure) {
    case BILI_HTTP_CANCELLED:
        kind = ErrorKind::Cancelled;
        break;
    case BILI_HTTP_DISCONNECTED:
        kind = ErrorKind::Disconnected;
        break;
    case BILI_HTTP_MEMORY:
        kind = ErrorKind::OutOfMemory;
        break;
    case BILI_HTTP_IO:
        kind = ErrorKind::IoFailure;
        break;
    case BILI_HTTP_TRANSPORT:
        kind = response.transport_code == 28 ? ErrorKind::Timeout : ErrorKind::HttpFailure;
        break;
    case BILI_HTTP_STATUS:
        kind = ErrorKind::HttpFailure;
        break;
    }
    return {ErrorDomain::Network, kind, op, response.transport_code, response.status};
}
static HttpResponse deviceRequest(const std::string &url, const std::string *form, ErrorOperation op) {
    auto wifiError = WifiManager{}.connect();
    if (wifiError) {
        wifiError.operation = op;
        HttpResponse out;
        out.error = std::move(wifiError);
        return out;
    }
    BiliHttpResponse response{};
    if (form) bili_http_post_response(url.c_str(), form->c_str(), &response, BILI_HTTP_RAM_THRESHOLD);
    else bili_http_get_response_spill(url.c_str(), &response, BILI_HTTP_RAM_THRESHOLD, BILI_HTTP_SPILL_LIMIT, nullptr);
    HttpResponse out;
    if (response.spilled && response.body_path[0]) {
        out = HttpResponse::fromSpillFile(response.body_path, response.body_bytes, true);
    } else if (response.text) {
        out.body = response.text;
        out.bodyBytes = out.body.size();
        bili_http_free(response.text);
    }
    out.cookies = response.cookies;
    out.error = mapHttpError(response, op);
    return out;
}
HttpClient::HttpClient(Transport transport, PostTransport postTransport)
    : transport_(transport ? std::move(transport) : [](const std::string &url, ErrorOperation op) {
          return deviceRequest(url, nullptr, op);
      }), postTransport_(postTransport ? std::move(postTransport) :
          [](const std::string &url, const std::string &body, ErrorOperation op) {
              return deviceRequest(url, &body, op);
          }) {}
Result<HttpResponse> HttpClient::post(const std::string &url, const std::string &body,
                                      ErrorOperation op) const {
    if (transfer_cancel)
        return Result<HttpResponse>::failure({ErrorDomain::Network, ErrorKind::Cancelled, op});
    auto response = postTransport_(url, body, op);
    if (response.error) return Result<HttpResponse>::failure(response.error);
    return {std::move(response), {}};
}
Result<HttpResponse> HttpClient::get(const std::string &url, ErrorOperation op,
                                     bool retrySafe) const {
    for (int attempt = 0;; ++attempt) {
        if (transfer_cancel)
            return Result<HttpResponse>::failure({ErrorDomain::Network, ErrorKind::Cancelled, op});
        HttpResponse response = transport_(url, op);
        if (!response.error)
            return {std::move(response), {}};
        response.error.operation = op;
        if (attempt == 0 && retrySafe && !playback_address_request(url.c_str()) && mayRetry(response.error) && !transfer_cancel)
            continue;
        return Result<HttpResponse>::failure(std::move(response.error));
    }
}
HttpClient &defaultHttpClient() {
    static HttpClient instance;
    return instance;
}
} // namespace pspapp
