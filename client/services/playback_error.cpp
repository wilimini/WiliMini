#include "playback_error.hpp"
namespace pspapp {
AppError playbackError(int code, PspVideoStage stage) {
    ErrorOperation operation = ErrorOperation::Decode;
    ErrorKind kind = ErrorKind::Unknown;
    if (stage == PSP_VIDEO_STAGE_ME_INIT || stage == PSP_VIDEO_STAGE_ME_RENDER_INIT) {
        operation = ErrorOperation::InitMe;
        kind = code == -99  ? ErrorKind::MeImageUnknown
               : code == -7 ? ErrorKind::OutOfMemory
                            : ErrorKind::MeInitFailed;
    } else if (stage == PSP_VIDEO_STAGE_CLEANUP)
        operation = ErrorOperation::Cleanup;
    else if (stage == PSP_VIDEO_STAGE_SOURCE || stage == PSP_VIDEO_STAGE_VIDEO_INIT ||
             stage == PSP_VIDEO_STAGE_AUDIO_INIT)
        operation = ErrorOperation::PreparePlayback;
    else if (stage == PSP_VIDEO_STAGE_LOG) {
        kind = ErrorKind::IoFailure;
        operation = ErrorOperation::PreparePlayback;
    }
    return {ErrorDomain::Playback, kind, operation, code, 0, psp_video_stage_name(stage)};
}
} // namespace pspapp
