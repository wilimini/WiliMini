#pragma once
#include "bilibili_api.hpp"
#include "session_store.hpp"
#include <vector>

namespace pspapp {
enum class AuthState {
    SignedOut,
    Restored,
    WaitingScan,
    WaitingConfirm,
    QrExpired,
    SignedIn,
    SessionExpired
};
struct AuthView {
    AuthState state = AuthState::SignedOut;
    std::string key, name, mid;
    std::vector<unsigned char> qr;
    int qrSize = 0;
};
class AuthService {
  public:
    AuthService(BilibiliApi &api, SessionStore &sessions) : api_(api), sessions_(sessions) {}
    Result<AuthView> restore();
    Result<AuthView> generate();
    Result<AuthView> poll(const std::string &key);
    Result<AuthView> check();
    Result<AuthView> logout();

  private:
    BilibiliApi &api_;
    SessionStore &sessions_;
};
const char *authStateMessage(AuthState state, bool confirmWithCircle = false);
} // namespace pspapp
