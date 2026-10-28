#include "app_error.hpp"
#include <cstdio>

namespace pspapp {
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
    const char *text = "操作失败";
    switch (e.kind) {
    case ErrorKind::None:
        return {};
    case ErrorKind::Cancelled:
        return "已取消";
    case ErrorKind::Timeout:
        text = "网络请求超时，请重试";
        break;
    case ErrorKind::Disconnected:
        text = "网络未连接 · SELECT 选择 Wi-Fi";
        break;
    case ErrorKind::HttpFailure:
        text = "服务器请求失败";
        break;
    case ErrorKind::ApiRejected:
        text = "接口拒绝请求";
        break;
    case ErrorKind::InvalidResponse:
        text = "服务器响应无法识别";
        break;
    case ErrorKind::LoginRequired:
        text = "需要登录账号";
        break;
    case ErrorKind::SessionExpired:
        text = "登录已失效，请重新扫码";
        break;
    case ErrorKind::MeImageUnknown:
        text = "ME 固件无法识别 · 请重启 PSP";
        break;
    case ErrorKind::MeInitFailed:
        if (e.rawCode <= -100 && e.rawCode >= -103)
            text = "ME 固件不兼容";
        else if (e.rawCode == -6)
            text = "ME 初始化 DMA 超时 · 请重启 PSP";
        else if (e.rawCode == -202 || e.rawCode == -212)
            text = "ME 启动超时 · 请重启 PSP";
        else
            text = "ME 初始化失败 · 请重启 PSP";
        break;
    case ErrorKind::OutOfMemory:
        text = e.domain == ErrorDomain::Playback ? "可用内存不足 · 请在设置中减小播放缓冲" : "可用内存不足";
        break;
    case ErrorKind::IoFailure:
        text = "记忆棒读写失败";
        break;
    default:
        if (e.domain == ErrorDomain::Playback) {
            if (e.operation == ErrorOperation::PreparePlayback) {
                if (e.detail == "audio_init")
                    text = "音频初始化失败";
                else if (e.detail == "video_init")
                    text = "视频解码器初始化失败";
                else
                    text = "视频流读取失败";
            } else if (e.operation == ErrorOperation::Cleanup)
                text = "播放收尾失败";
            else
                text = "播放失败";
        }
        break;
    }
    std::string out = text;
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
    if (FILE *out = std::fopen(path.c_str(), "w")) {
        std::fprintf(out,
                     "operation=%s\ndomain=%d\nkind=%d\nraw_code=%d\nhttp_status=%d\nmessage=%s\n",
                     operationName(e.operation), int(e.domain), int(e.kind), e.rawCode,
                     e.httpStatus, errorMessage(e).c_str());
        std::fclose(out);
    }
}
} // namespace pspapp
