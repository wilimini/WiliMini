#include "music_queue.hpp"

#include <utility>

namespace pspapp {

void MusicQueue::reset(std::vector<MusicTrack> tracks, size_t start) {
    tracks_ = std::move(tracks);
    current_ = start < tracks_.size() ? start : 0;
}

RepeatMode MusicQueue::cycleRepeat() {
    repeat_ = repeat_ == RepeatMode::All ? RepeatMode::One
            : repeat_ == RepeatMode::One ? RepeatMode::Once : RepeatMode::All;
    return repeat_;
}

MusicStep MusicQueue::onEnded() const {
    if (tracks_.empty()) return {};
    if (repeat_ == RepeatMode::One) return {MusicStep::Replay, current_};
    if (current_ + 1 < tracks_.size()) return {MusicStep::Play, current_ + 1};
    if (repeat_ == RepeatMode::All)
        return tracks_.size() == 1 ? MusicStep{MusicStep::Replay, current_}
                                   : MusicStep{MusicStep::Play, 0};
    return {};  // Once: stop after the last track.
}

MusicStep MusicQueue::next() const {
    if (tracks_.empty()) return {};
    if (current_ + 1 < tracks_.size()) return {MusicStep::Play, current_ + 1};
    if (repeat_ == RepeatMode::All && tracks_.size() > 1) return {MusicStep::Play, 0};
    return {};
}

MusicStep MusicQueue::previous(int64_t positionMs) const {
    if (tracks_.empty()) return {};
    if (positionMs > kRestartThresholdMs) return {MusicStep::Replay, current_};
    if (current_ > 0) return {MusicStep::Play, current_ - 1};
    if (repeat_ == RepeatMode::All && tracks_.size() > 1)
        return {MusicStep::Play, tracks_.size() - 1};
    return {MusicStep::Replay, current_};
}

MusicStep MusicQueue::jump(size_t index) const {
    if (index >= tracks_.size()) return {};
    return {index == current_ ? MusicStep::Replay : MusicStep::Play, index};
}

size_t MusicQueue::prefetchIndex() const {
    const MusicStep step = onEnded();
    return step.kind == MusicStep::Play ? step.index : npos;
}

bool MusicQueue::shouldPrefetch(int64_t positionMs, int64_t durationMs) const {
    if (durationMs <= 0 || prefetchIndex() == npos) return false;
    return durationMs - positionMs <= kPrefetchLeadMs;
}

void MusicQueue::moveTo(size_t index) {
    if (index < tracks_.size()) current_ = index;
}

}  // namespace pspapp
