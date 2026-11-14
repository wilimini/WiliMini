#pragma once
#include <psppower.h>

// The bridge may release its own lock before audio, overlays and UI teardown
// finish. Keep a separate application lock until the whole playback scope ends.
class PlaybackPowerLock {
public:
    PlaybackPowerLock() : result_(scePowerLock(0)) {}
    ~PlaybackPowerLock() { if (result_ >= 0 && safe_) scePowerUnlock(0); }
    PlaybackPowerLock(const PlaybackPowerLock&) = delete;
    PlaybackPowerLock& operator=(const PlaybackPowerLock&) = delete;
    int result() const { return result_; }
    void retain() { safe_ = false; } // ME shutdown did not prove it stopped.
private:
    int result_;
    bool safe_ = true;
};
