#pragma once

// Also used by host checks, which default to the PSP-1000 profile.
#ifndef BILI_CLIENT_HEAP_KB
#define BILI_CLIENT_HEAP_KB (12 * 1024)
#endif
#ifndef BILI_DEFAULT_RGB_FRAMES
#define BILI_DEFAULT_RGB_FRAMES 2
#endif

namespace pspapp {
int defaultPlaybackBufferFrames();
}
