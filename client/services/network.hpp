#pragma once
#include "app_error.hpp"
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace pspapp {
struct HttpResponse {
    std::string body;
    std::string bodyPath; // Temporary file on storage when response exceeded RAM threshold.
    size_t bodyBytes = 0;
    std::shared_ptr<const void> bodyFileGuard; // Deletes bodyPath when the last owner is destroyed.
    std::string cookies; // Login response only; never logged.
    AppError error;

    bool spilled() const { return !bodyPath.empty(); }
    static HttpResponse fromSpillFile(std::string path, size_t bytes, bool autoDelete = true);
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
