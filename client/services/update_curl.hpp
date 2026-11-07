#pragma once
// libcurl implementation of UpdateTransport for the PSP client (issue #26).
// Needs curl_global_init() to have run (network_init does it).
#include "update_install.hpp"
namespace pspapp {
class CurlUpdateTransport : public UpdateTransport {
  public:
    // cancelled is polled about once a second while a transfer is stalled.
    explicit CurlUpdateTransport(const char* caBundlePath, const volatile bool* cancel = nullptr)
        : ca_(caBundlePath), cancel_(cancel) {}
    bool get(const std::string& url, uint64_t rangeFrom,
             const std::function<bool(int, const std::string&)>& onHeaders,
             const std::function<bool(const void*, size_t)>& onData) override;
    std::string lastError() const override { return error_; }
  private:
    std::string error_;
    const char* ca_;
    const volatile bool* cancel_;
};
}
