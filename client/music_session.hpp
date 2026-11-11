#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <pspkernel.h>

#include "bilibili.h"
#include "services/music_queue.hpp"
#include "services/subtitles.hpp"

namespace pspapp {

enum class MusicState { Opening, Playing, Buffering, Paused, Ended, Failed };

// One consistent copy for the UI thread; reading it never blocks on the network.
struct MusicView {
    MusicState state = MusicState::Opening;
    size_t index = 0;
    RepeatMode repeat = RepeatMode::All;
    int64_t positionMs = 0, durationMs = 0;
    bool paused = false;
    std::string error;
    // Subtitle tracks of the current part, loaded after audio starts. Null until ready.
    std::shared_ptr<const std::vector<SubtitleTrack>> lyrics;
    size_t lyricTrack = 0;
    bool lossless = false;        // the FLAC track was asked for
    bool losslessActive = false;  // the part that is open is FLAC
};

// Music mode for the parts of one video. A single worker thread owns the audio
// engine (psp_audio_*), the queue, the next part's prefetch and the lyrics; the UI
// thread posts commands and reads view(). Only one session exists at a time and it
// must not overlap psp_video_play.
class MusicSession {
public:
    struct Source {
        std::string bvid;
        uint64_t aid = 0;
    };

    MusicSession(Source source, std::vector<MusicTrack> tracks, size_t start, RepeatMode repeat,
                 bool lossless = false);
    ~MusicSession();
    MusicSession(const MusicSession&) = delete;
    MusicSession& operator=(const MusicSession&) = delete;

    bool start();
    // Blocks until the worker has closed the engine and finished every source.
    void stop();

    MusicView view(bool livePosition = false) const;
    const std::vector<MusicTrack>& tracks() const { return tracks_; }  // fixed after construction

    // Non-blocking commands for the UI thread.
    void togglePause();
    void next();
    void previous();
    void jump(size_t index);
    void seekBy(int64_t deltaMs);
    void cycleRepeat();
    void cycleLyricTrack();
    // Prefer the FLAC track. A change reopens the current part where it is playing.
    void setLossless(bool enabled);

private:
    static int entry(SceSize size, void* arg);
    void run();
    void post();
    void queueStep(const MusicStep& step);
    bool stepPending() const;
    bool seekPending() const;
    void handleCommands();
    void pollEngine();
    void playStep(const MusicStep& step, bool drain = false);
    bool openTrack(size_t index, int64_t startMs, bool drain);
    bool seekTo(int64_t ms);
    bool recover(int64_t positionMs);
    bool prepare(size_t index, BiliPlayback& out, std::string& error);
    void releaseSource(BiliPlayback& playback);
    void discardNext();
    void maybePrefetch(int64_t positionMs, int64_t durationMs);
    void loadLyrics(size_t index);
    void setState(MusicState state);
    void setFailed(const std::string& message);
    void applyPause();

    const Source source_;
    const std::vector<MusicTrack> tracks_;

    mutable std::mutex mutex_;  // guards everything below up to the worker-only block
    MusicQueue queue_;
    MusicState state_ = MusicState::Opening;
    int64_t positionMs_ = 0, durationMs_ = 0;
    std::string error_;
    std::shared_ptr<const std::vector<SubtitleTrack>> lyrics_;
    size_t lyricTrack_ = 0;
    bool wantLossless_ = false, losslessActive_ = false, reloadPending_ = false;
    bool wantPaused_ = false;
    bool hasStep_ = false;
    MusicStep pendingStep_;
    int64_t pendingSeekMs_ = -1;
    uint64_t seekCommitAt_ = 0;
    bool seeking_ = false;
    bool lyricsPending_ = false;

    std::atomic<bool> stopping_{false};
    std::atomic<bool> prefetching_{false};
    std::atomic<bool> loadingLyrics_{false};
    SceUID thread_ = -1;
    SceUID wake_ = -1;

    // Worker thread only.
    BiliPlayback current_{}, next_{};
    size_t nextIndex_ = MusicQueue::npos;
    bool nextReady_ = false;
    bool engineOpen_ = false;
    uint64_t prefetchRetryAt_ = 0, lastPowerTick_ = 0, recoverWindowStart_ = 0;
    int recoverAttempts_ = 0;
    // A FLAC part the decoder could not open: the session stays on AAC until asked again.
    bool losslessRejected_ = false;
};

}  // namespace pspapp
