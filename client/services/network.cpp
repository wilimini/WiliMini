#include "network.hpp"
#include "../bilibili.h"

namespace pspapp {
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
static HttpResponse deviceRequest(const std::string &url, const std::string *form, ErrorOperation op) {
    auto wifiError = WifiManager{}.connect();
    if (wifiError) {
        wifiError.operation = op;
        return {{}, {}, std::move(wifiError)};
    }
    BiliHttpResponse response{};
    if (form) bili_http_post_response(url.c_str(), form->c_str(), &response, 512 * 1024);
    else bili_http_get_response(url.c_str(), &response, 512 * 1024);
    HttpResponse out;
    if (response.text) {
        out.body = response.text;
        bili_http_free(response.text);
    }
    out.cookies = response.cookies;
    if (response.failure) {
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
        case BILI_HTTP_TRANSPORT:
            kind = response.transport_code == 28 ? ErrorKind::Timeout : ErrorKind::HttpFailure;
            break;
        case BILI_HTTP_STATUS:
            kind = ErrorKind::HttpFailure;
            break;
        }
        out.error = {ErrorDomain::Network, kind, op, response.transport_code, response.status};
    }
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
        if (attempt == 0 && retrySafe && mayRetry(response.error) && !transfer_cancel)
            continue;
        return Result<HttpResponse>::failure(std::move(response.error));
    }
}
HttpClient &defaultHttpClient() {
    static HttpClient instance;
    return instance;
}
} // namespace pspapp
