#pragma once

// Also used by host checks, which default to the PSP-1000 profile.
#ifndef BILI_CLIENT_HEAP_KB
#define BILI_CLIENT_HEAP_KB (12 * 1024)
#endif
#ifndef BILI_DEFAULT_RGB_FRAMES
#define BILI_DEFAULT_RGB_FRAMES 2
#endif
#ifndef BILI_DEFAULT_RGB565
#define BILI_DEFAULT_RGB565 1
#endif

namespace pspapp {
int defaultPlaybackBufferFrames();
bool defaultPlaybackRgb565();
}
