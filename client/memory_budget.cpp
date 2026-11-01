#include "memory_budget.hpp"

namespace pspapp {
int defaultPlaybackBufferFrames() { return BILI_DEFAULT_RGB_FRAMES; }
bool defaultPlaybackRgb565() { return BILI_DEFAULT_RGB565 != 0; }
}
