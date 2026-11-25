#include "auth_service.hpp"
#include "../localization.hpp"
#include "../bilibili.h"
#include "../api/wiliwili/bilibili/api.h"
#include "../third_party/qrcodegen/qrcodegen.h"

namespace pspapp {
using namespace bilibili;
static AppError invalid(ErrorOperation op) {
    return {ErrorDomain::Auth, ErrorKind::InvalidResponse, op};
}
std::string authStateMessage(AuthState state, bool confirmWithCircle) {
    const char *confirm = buttonGlyph(confirmWithCircle ? ControllerButton::Circle : ControllerButton::Cross);
    switch (state) {
    case AuthState::Restored:
        return tr(TextId::auth_restored);
    case AuthState::WaitingScan:
        return tr(TextId::auth_wait_scan);
    case AuthState::WaitingConfirm:
        return tr(TextId::auth_wait_confirm);
    case AuthState::QrExpired:
        return tr(TextId::auth_qr_expired);
    case AuthState::SignedIn:
        return tr(TextId::auth_signed_in);
    case AuthState::SessionExpired:
        return formatTr(TextId::auth_session_expired, {{"confirm", confirm}});
    default:
        return formatTr(TextId::auth_signed_out, {{"confirm", confirm}});
    }
}
Result<AuthView> AuthService::restore() {
    auto session = sessions_.load();
    if (!session)
        return Result<AuthView>::failure(session.error);
    AuthView out;
    if (session.value.valid()) {
        out.state = AuthState::Restored;
        out.mid = session.value.mid;
    }
    return {std::move(out), {}};
}
Result<AuthView> AuthService::generate() {
    auto response = api_.get(Api::QrLoginUrlV2, {}, false, ErrorOperation::GenerateQr, false);
    if (!response)
        return Result<AuthView>::failure(response.error);
    try {
        const auto &data = response.value.data;
        auto url = data.at("url").get<std::string>();
        AuthView out;
        out.key = data.at("qrcode_key").get<std::string>();
        if (out.key.empty() || out.key.size() > 256 || url.size() > 1024 ||
            url.rfind("https://", 0) != 0)
            throw std::runtime_error("Invalid QR payload");
        unsigned char temp[qrcodegen_BUFFER_LEN_FOR_VERSION(12)],
            qr[qrcodegen_BUFFER_LEN_FOR_VERSION(12)];
        if (!qrcodegen_encodeText(url.c_str(), temp, qr, qrcodegen_Ecc_MEDIUM, 1, 12,
                                  qrcodegen_Mask_AUTO, true))
            throw std::runtime_error("QR payload too large");
        out.qrSize = qrcodegen_getSize(qr);
        out.qr.resize(out.qrSize * out.qrSize);
        for (int y = 0; y < out.qrSize; ++y)
            for (int x = 0; x < out.qrSize; ++x)
                out.qr[y * out.qrSize + x] = qrcodegen_getModule(qr, x, y);
        out.state = AuthState::WaitingScan;
        return {std::move(out), {}};
    } catch (const std::exception &) {
        return Result<AuthView>::failure(invalid(ErrorOperation::GenerateQr));
    }
}
Result<AuthView> AuthService::poll(const std::string &key) {
    if (key.empty())
        return Result<AuthView>::failure(invalid(ErrorOperation::PollLogin));
    auto response = api_.get(Api::QrLoginInfoV2, {{"qrcode_key", key}}, false,
                             ErrorOperation::PollLogin, false);
    if (!response)
        return Result<AuthView>::failure(response.error);
    try {
        int code = response.value.data.at("code").get<int>();
        AuthView out;
        switch (code) {
        case 86101:
            out.state = AuthState::WaitingScan;
            break;
        case 86090:
            out.state = AuthState::WaitingConfirm;
            break;
        case 86038:
            out.state = AuthState::QrExpired;
            break;
        case 0: {
            auto session = SessionStore::parseCookies(response.value.cookies);
            if (!session)
                return Result<AuthView>::failure(session.error);
            if (transfer_cancel)
                return Result<AuthView>::failure(
                    {ErrorDomain::Auth, ErrorKind::Cancelled, ErrorOperation::PollLogin});
            auto error = sessions_.save(session.value);
            if (error)
                return Result<AuthView>::failure(std::move(error));
            out.state = AuthState::SignedIn;
            out.mid = session.value.mid;
            break;
        }
        default:
            return Result<AuthView>::failure(
                {ErrorDomain::Auth, ErrorKind::ApiRejected, ErrorOperation::PollLogin, code});
        }
        return {std::move(out), {}};
    } catch (const std::exception &) {
        return Result<AuthView>::failure(invalid(ErrorOperation::PollLogin));
    }
}
Result<AuthView> AuthService::check() {
    if (!sessions_.current().valid())
        return {AuthView{}, {}};
    auto response = api_.get(Api::Nav, {}, false, ErrorOperation::CheckSession);
    if (!response) {
        if (response.error.kind == ErrorKind::SessionExpired ||
            response.error.kind == ErrorKind::LoginRequired) {
            auto error = sessions_.clear();
            if (error)
                return Result<AuthView>::failure(std::move(error));
            AuthView out;
            out.state = AuthState::SessionExpired;
            return {std::move(out), {}};
        }
        return Result<AuthView>::failure(response.error);
    }
    try {
        const auto &data = response.value.data;
        if (!data.at("isLogin").get<bool>()) {
            auto error = sessions_.clear();
            if (error)
                return Result<AuthView>::failure(std::move(error));
            AuthView out;
            out.state = AuthState::SessionExpired;
            return {std::move(out), {}};
        }
        AuthView out;
        out.state = AuthState::SignedIn;
        out.name = data.at("uname").get<std::string>();
        out.mid = std::to_string(data.at("mid").get<uint64_t>());
        if (out.mid != sessions_.current().mid)
            return Result<AuthView>::failure(invalid(ErrorOperation::CheckSession));
        return {std::move(out), {}};
    } catch (const std::exception &) {
        return Result<AuthView>::failure(invalid(ErrorOperation::CheckSession));
    }
}
Result<AuthView> AuthService::logout() {
    auto error = sessions_.clear();
    if (error)
        return Result<AuthView>::failure(std::move(error));
    return {AuthView{}, {}};
}
} // namespace pspapp
