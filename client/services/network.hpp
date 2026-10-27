#pragma once
#include "app_error.hpp"
#include <functional>
#include <string>

namespace pspapp {
struct HttpResponse {
    std::string body;
    std::string cookies; // Login response only; never logged.
    AppError error;
};
class WifiManager {
  public:
    AppError initialize() const;
    bool ready() const;
    AppError connect() const;
};
class HttpClient {
  public:
    using Transport = std::function<HttpResponse(const std::string &, ErrorOperation)>;
    using PostTransport = std::function<HttpResponse(const std::string &, const std::string &, ErrorOperation)>;
    explicit HttpClient(Transport transport = {}, PostTransport postTransport = {});
    Result<HttpResponse> get(const std::string &url, ErrorOperation operation,
                             bool retrySafe = true) const;
    Result<HttpResponse> post(const std::string &url, const std::string &body,
                              ErrorOperation operation) const;

  private:
    Transport transport_;
    PostTransport postTransport_;
};
HttpClient &defaultHttpClient();
} // namespace pspapp
