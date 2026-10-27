#pragma once
#include "app_error.hpp"

namespace pspapp {
struct Session {
    std::string cookies;
    std::string mid;
    bool valid() const { return !cookies.empty() && !mid.empty(); }
};
class SessionStore {
  public:
    explicit SessionStore(std::string path) : path_(std::move(path)) {}
    Result<Session> load();
    AppError save(const Session &session);
    AppError clear();
    const Session &current() const { return current_; }
    static Result<Session> parseCookies(const std::string &cookies);

  private:
    std::string path_;
    Session current_;
};
} // namespace pspapp
