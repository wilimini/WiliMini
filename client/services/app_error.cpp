#include "app_error.hpp"
#include "../localization.hpp"
#include <cstdio>
#include <wili_log.h>

namespace pspapp {
static const char *safeDetail(const std::string &detail) {
    if (detail.empty()) return "none";
    if (detail.size() > 120) return "redacted";
    for (unsigned char c : detail) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '=' || c == '.' || c == ' ')) return "redacted";
    }
    return detail.c_str();
}
const char *operationName(ErrorOperation op) {
    switch (op) {
    case ErrorOperation::ConnectWifi:
        return "connect_wifi";
    case ErrorOperation::SignRequest:
        return "sign_request";
    case ErrorOperation::Search:
        return "search";
    case ErrorOperation::SearchTopics:
        return "search_topics";
    case ErrorOperation::GenerateQr:
        return "generate_qr";
    case ErrorOperation::PollLogin:
        return "poll_login";
    case ErrorOperation::CheckSession:
        return "check_session";
    case ErrorOperation::SaveSession:
        return "save_session";
    case ErrorOperation::Logout:
        return "logout";
    case ErrorOperation::InitMe:
        return "me_init";
    case ErrorOperation::Decode:
        return "decode";
    case ErrorOperation::Cleanup:
        return "cleanup";
    case ErrorOperation::PreparePlayback:
        return "prepare_playback";
    default:
        return "request";
    }
}
std::string errorMessage(const AppError &e) {
    TextId id = TextId::err_operation_failed;
    switch (e.kind) {
    case ErrorKind::None:
        return {};
    case ErrorKind::Cancelled:
        return tr(TextId::err_cancelled);
    case ErrorKind::Timeout:
        id = TextId::err_timeout;
        break;
    case ErrorKind::Disconnected:
        id = TextId::err_disconnected;
        break;
    case ErrorKind::HttpFailure:
        id = TextId::err_http_failure;
        break;
    case ErrorKind::ApiRejected:
        id = TextId::err_api_rejected;
        break;
    case ErrorKind::InvalidResponse:
        id = TextId::err_invalid_response;
        break;
    case ErrorKind::LoginRequired:
        id = TextId::err_login_required;
        break;
    case ErrorKind::SessionExpired:
        id = TextId::err_session_expired;
        break;
    case ErrorKind::MeImageUnknown:
        id = TextId::err_me_unknown;
        break;
    case ErrorKind::MeInitFailed:
        if (e.rawCode <= -100 && e.rawCode >= -103)
            id = TextId::err_me_incompatible;
        else if (e.rawCode == -6)
            id = TextId::err_me_dma_timeout;
        else if (e.rawCode == -202 || e.rawCode == -212)
            id = TextId::err_me_start_timeout;
        else
            id = TextId::err_me_init_failed;
        break;
    case ErrorKind::OutOfMemory:
        id = e.domain == ErrorDomain::Playback ? TextId::err_oom_playback : TextId::err_oom;
        break;
    case ErrorKind::IoFailure:
        id = TextId::err_io_failure;
        break;
    default:
        if (e.domain == ErrorDomain::Playback) {
            if (e.operation == ErrorOperation::PreparePlayback) {
                if (e.detail == "dash_unavailable")
                    id = TextId::err_no_dash_stream;
                else if (e.detail == "video_source")
                    id = TextId::err_connect_video_failed;
                else if (e.detail == "audio_init")
                    id = TextId::err_audio_init_failed;
                else if (e.detail == "video_init")
                    id = TextId::err_video_init_failed;
                else
                    id = TextId::err_stream_read_failed;
            } else if (e.operation == ErrorOperation::Cleanup)
                id = TextId::err_playback_cleanup_failed;
            else
                id = TextId::err_playback_failed;
        }
        break;
    }
    std::string out = tr(id);
    if (e.rawCode)
        out += " (" + std::to_string(e.rawCode) + ")";
    if (e.httpStatus)
        out += " · HTTP " + std::to_string(e.httpStatus);
    // Raw transport details may contain signed URLs or account information.
    // UI and logs use typed codes; credentials and response bodies stay private.
    return out;
}
bool mayRetry(const AppError &e) {
    return e.kind == ErrorKind::Timeout ||
           (e.kind == ErrorKind::HttpFailure &&
            (e.httpStatus == 502 || e.httpStatus == 503 || e.httpStatus == 504));
}
void logError(const std::string &path, const AppError &e) {
    if (!e || e.cancelled())
        return;
    const char *detail = safeDetail(e.detail);
    WLOG_E(WLC_CLIENT, "error operation=%s domain=%d kind=%d raw_code=%d http_status=%d detail=%s",
           operationName(e.operation), int(e.domain), int(e.kind), e.rawCode, e.httpStatus, detail);
    if (FILE *out = std::fopen(path.c_str(), "w")) {
        std::fprintf(out,
                     "operation=%s\ndomain=%d\nkind=%d\nraw_code=%d\nhttp_status=%d\ndetail=%s\nmessage=%s\n",
                     operationName(e.operation), int(e.domain), int(e.kind), e.rawCode,
                     e.httpStatus, detail, errorMessage(e).c_str());
        // The newest log lines (including the error above) show what led up to it, but only
        // when log files are enabled in Settings.
        if (wili_log_files()) {
            std::fputs("recent_log_begin\n", out);
            const int lines = wili_log_dump(out);
            std::fprintf(out, "recent_log_end lines=%d\n", lines);
        }
        std::fclose(out);
    }
}
} // namespace pspapp
