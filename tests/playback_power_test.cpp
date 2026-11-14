#include "playback_power.hpp"
#include <cassert>
#include <cstdio>

static int locks, unlocks, error;
int scePowerLock(int value) {
    assert(value == 0);
    if (error < 0) return error;
    ++locks;
    return 0;
}
int scePowerUnlock(int value) {
    assert(value == 0 && locks > 0);
    --locks;
    ++unlocks;
    return 0;
}
int main() {
    {
        PlaybackPowerLock application;
        assert(application.result() == 0 && locks == 1);
        // Bridge restoration must not release the application's sleep barrier.
        scePowerLock(0);
        scePowerUnlock(0);
        assert(locks == 1);
        // Audio/overlay/source/UI cleanup happens while this scope is alive.
        assert(unlocks == 1);
    }
    assert(locks == 0 && unlocks == 2);
    error = -1;
    {
        PlaybackPowerLock failed;
        assert(failed.result() == -1 && locks == 0);
    }
    assert(unlocks == 2); // A failed acquisition must not unlock another owner.
    error = 0;
    {
        PlaybackPowerLock unsafe;
        unsafe.retain(); // Incomplete ME shutdown must not allow suspend.
    }
    assert(locks == 1 && unlocks == 2);
    std::puts("Playback power ownership passed");
}
