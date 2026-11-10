#include "../client/services/music_queue.hpp"
#include <cassert>
#include <iostream>
int main() {
    using namespace pspapp;
    MusicQueue q;
    assert(q.next().kind == MusicStep::None && q.onEnded().kind == MusicStep::None);
    q.reset({{1, "a", 100}, {2, "b", 100}, {3, "c", 100}}, 1);
    assert(q.current() == 1 && q.hasQueue());
    // Repeat all wraps; repeat once stops after the last track; repeat one replays.
    q.moveTo(2);
    assert(q.onEnded().kind == MusicStep::Play && q.onEnded().index == 0);
    q.setRepeat(RepeatMode::Once);
    assert(q.onEnded().kind == MusicStep::None && q.next().kind == MusicStep::None);
    q.moveTo(0);
    assert(q.onEnded().kind == MusicStep::Play && q.onEnded().index == 1);
    q.setRepeat(RepeatMode::One);
    assert(q.onEnded().kind == MusicStep::Replay && q.onEnded().index == 0);
    assert(q.cycleRepeat() == RepeatMode::Once && q.cycleRepeat() == RepeatMode::All);
    // Previous restarts a track that is already playing, otherwise steps back and wraps on All.
    assert(q.previous(5000).kind == MusicStep::Replay);
    assert(q.previous(1000).kind == MusicStep::Play && q.previous(1000).index == 2);
    q.setRepeat(RepeatMode::Once);
    assert(q.previous(1000).kind == MusicStep::Replay);
    // Jump and prefetch.
    assert(q.jump(9).kind == MusicStep::None && q.jump(0).kind == MusicStep::Replay);
    assert(q.prefetchIndex() == 1 && q.shouldPrefetch(86000, 100000) && !q.shouldPrefetch(80000, 100000));
    q.setRepeat(RepeatMode::One);
    assert(q.prefetchIndex() == MusicQueue::npos && !q.shouldPrefetch(99000, 100000));
    // A single-part video has no queue; repeat all replays it.
    q.reset({{1, "only", 60}}, 0);
    q.setRepeat(RepeatMode::All);
    assert(!q.hasQueue() && q.onEnded().kind == MusicStep::Replay && q.next().kind == MusicStep::None);
    std::cout << "music queue: PASS\n";
}
