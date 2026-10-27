#pragma once
#include "app_error.hpp"
extern "C" {
#include "psp_video.h"
}
namespace pspapp {
AppError playbackError(int code, PspVideoStage stage);
}
