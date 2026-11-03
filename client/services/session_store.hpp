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
    // Server mode: the account lives on the PC, so the PSP only remembers who it is
    // in memory. Nothing is written to disk and no cookie reaches the HTTP layer.
    void setEphemeral(Session session) { current_ = std::move(session); }
    void clearEphemeral() { current_ = Session{}; }
    static Result<Session> parseCookies(const std::string &cookies);

  private:
    std::string path_;
    Session current_;
};
} // namespace pspapp
