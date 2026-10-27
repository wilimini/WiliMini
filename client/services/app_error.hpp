#pragma once
#include <string>
#include <utility>

namespace pspapp {
enum class ErrorDomain { None, Network, Api, Auth, Playback, Storage };
enum class ErrorKind {
    None,
    Cancelled,
    Timeout,
    Disconnected,
    HttpFailure,
    ApiRejected,
    InvalidResponse,
    LoginRequired,
    SessionExpired,
    MeImageUnknown,
    MeInitFailed,
    OutOfMemory,
    IoFailure,
    Unknown
};
enum class ErrorOperation {
    None,
    ConnectWifi,
    Request,
    SignRequest,
    Search,
    SearchTopics,
    GenerateQr,
    PollLogin,
    CheckSession,
    SaveSession,
    Logout,
    PreparePlayback,
    InitMe,
    Decode,
    Cleanup
};
struct AppError {
    ErrorDomain domain = ErrorDomain::None;
    ErrorKind kind = ErrorKind::None;
    ErrorOperation operation = ErrorOperation::None;
    int rawCode = 0;
    int httpStatus = 0;
    std::string detail;
    AppError() = default;
    AppError(ErrorDomain source, ErrorKind type, ErrorOperation op, int code = 0, int status = 0,
             std::string context = {})
        : domain(source), kind(type), operation(op), rawCode(code), httpStatus(status),
          detail(std::move(context)) {}
    explicit operator bool() const { return kind != ErrorKind::None; }
    bool cancelled() const { return kind == ErrorKind::Cancelled; }
};
template <class T> struct Result {
    T value{};
    AppError error{};
    explicit operator bool() const { return !error; }
    static Result failure(AppError error) { return {T{}, std::move(error)}; }
};
std::string errorMessage(const AppError &error);
bool mayRetry(const AppError &error);
const char *operationName(ErrorOperation operation);
void logError(const std::string &path, const AppError &error);
} // namespace pspapp
