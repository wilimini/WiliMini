#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pspapp {

// Platform-independent play order for music mode. The queue is the parts of one
// video; it holds no network or decoder state and never mutates on a query.
enum class RepeatMode { All, One, Once };

struct MusicTrack {
    uint64_t cid = 0;
    std::string title;
    int durationSec = 0;
};

struct MusicStep {
    enum Kind { None, Replay, Play } kind = None;
    size_t index = 0;  // Track to start for Replay and Play.
};

class MusicQueue {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);
    static constexpr int64_t kRestartThresholdMs = 3000;
    static constexpr int64_t kPrefetchLeadMs = 15000;

    void reset(std::vector<MusicTrack> tracks, size_t start);
    const std::vector<MusicTrack>& tracks() const { return tracks_; }
    bool empty() const { return tracks_.empty(); }
    size_t size() const { return tracks_.size(); }
    size_t current() const { return current_; }
    // A single track has nothing to queue; the screen hides the Up Next list for it.
    bool hasQueue() const { return tracks_.size() > 1; }

    RepeatMode repeat() const { return repeat_; }
    void setRepeat(RepeatMode mode) { repeat_ = mode; }
    RepeatMode cycleRepeat();

    // What follows when the current track plays to its end.
    MusicStep onEnded() const;
    // Next key. At the last track it wraps only for RepeatMode::All.
    MusicStep next() const;
    // Previous key: restarts the track when it is already past the threshold.
    MusicStep previous(int64_t positionMs) const;
    MusicStep jump(size_t index) const;
    // Track that natural advance would start, or npos when none needs warming.
    size_t prefetchIndex() const;
    bool shouldPrefetch(int64_t positionMs, int64_t durationMs) const;
    // Applies the result of next(), previous(), jump() or onEnded().
    void moveTo(size_t index);

private:
    std::vector<MusicTrack> tracks_;
    size_t current_ = 0;
    RepeatMode repeat_ = RepeatMode::All;
};

}  // namespace pspapp
