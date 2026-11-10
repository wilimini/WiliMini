#include "music_session.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

#include <psppower.h>

#include "playback.hpp"
#include "psp_audio.h"
#include "services/app_error.hpp"
#include "services/network.hpp"

namespace pspapp {

namespace {
constexpr uint64_t kPollUs = 150000;
constexpr uint64_t kSeekCoalesceUs = 400000;
constexpr uint64_t kPowerTickUs = 5000000;
constexpr uint64_t kPrefetchRetryUs = 10000000;
constexpr uint64_t kRecoverWindowUs = 60000000;
constexpr int kRecoverLimit = 2;
constexpr size_t kMaxLyricTracks = 3;

uint64_t nowUs() { return sceKernelGetSystemTimeWide(); }
}  // namespace

MusicSession::MusicSession(Source source, std::vector<MusicTrack> tracks, size_t start, RepeatMode repeat)
    : source_(std::move(source)), tracks_(std::move(tracks)) {
    queue_.reset(tracks_, start);
    queue_.setRepeat(repeat);
}

MusicSession::~MusicSession() { stop(); }

bool MusicSession::start() {
    if (thread_ >= 0 || tracks_.empty()) return false;
    wake_ = sceKernelCreateSema("music_wake", 0, 0, 8, nullptr);
    if (wake_ < 0) { wake_ = -1; return false; }
    // VFPU: probing the audio source decodes a frame on this thread.
    thread_ = sceKernelCreateThread("music_session", entry, 0x28, 224 * 1024,
                                    PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU, nullptr);
    if (thread_ < 0) {
        sceKernelDeleteSema(wake_);
        thread_ = wake_ = -1;
        return false;
    }
    transfer_cancel = 0;
    MusicSession* self = this;
    if (sceKernelStartThread(thread_, sizeof(self), &self) < 0) {
        sceKernelDeleteThread(thread_);
        sceKernelDeleteSema(wake_);
        thread_ = wake_ = -1;
        return false;
    }
    return true;
}

void MusicSession::stop() {
    if (thread_ < 0) return;
    stopping_ = true;
    transfer_cancel = 1;  // abort a prepare or lyrics request in flight
    psp_audio_stop();     // cancels pending source reads so a blocked open returns
    sceKernelSignalSema(wake_, 1);
    sceKernelWaitThreadEnd(thread_, nullptr);
    sceKernelDeleteThread(thread_);
    sceKernelDeleteSema(wake_);
    thread_ = wake_ = -1;
    transfer_cancel = 0;
}

int MusicSession::entry(SceSize, void* arg) {
    auto* self = *static_cast<MusicSession**>(arg);
    try {
        self->run();
    } catch (...) {
        psp_audio_close(0);
        self->setFailed("music_session_exception");
    }
    return 0;
}

MusicView MusicSession::view() const {
    std::lock_guard<std::mutex> lock(mutex_);
    MusicView out;
    out.state = state_;
    out.index = queue_.current();
    out.repeat = queue_.repeat();
    out.positionMs = positionMs_;
    out.durationMs = durationMs_;
    out.paused = wantPaused_;
    out.error = error_;
    out.lyrics = lyrics_;
    out.lyricTrack = lyricTrack_;
    return out;
}

void MusicSession::post() {
    // A prefetch or lyric fetch can hold the worker for seconds; abort it so the command runs now.
    if (prefetching_ || loadingLyrics_) transfer_cancel = 1;
    if (wake_ >= 0) sceKernelSignalSema(wake_, 1);
}

void MusicSession::queueStep(const MusicStep& step) {
    if (step.kind == MusicStep::None) return;
    const bool sameTrack = step.kind == MusicStep::Replay && step.index == queue_.current() &&
                           state_ != MusicState::Failed;
    queue_.moveTo(step.index);
    positionMs_ = 0;
    durationMs_ = static_cast<int64_t>(tracks_[step.index].durationSec) * 1000;
    error_.clear();
    pendingSeekMs_ = -1;
    seekCommitAt_ = 0;
    if (!sameTrack) {
        state_ = MusicState::Opening;
        lyrics_.reset();
        lyricTrack_ = 0;
        lyricsPending_ = false;
        pendingStep_ = {MusicStep::Play, step.index};
        hasStep_ = true;
        transfer_cancel = 1;
        psp_audio_stop();
    } else if (!hasStep_) {
        pendingStep_ = step;
        hasStep_ = true;
        if (prefetching_ || loadingLyrics_) transfer_cancel = 1;
        if (seeking_) psp_audio_interrupt();
    } else {
        pendingStep_ = {MusicStep::Play, step.index};
    }
    if (wake_ >= 0) sceKernelSignalSema(wake_, 1);
}

bool MusicSession::stepPending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hasStep_;
}

bool MusicSession::seekPending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pendingSeekMs_ >= 0;
}

void MusicSession::togglePause() {
    bool restart = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == MusicState::Ended || state_ == MusicState::Failed) {
            queueStep({MusicStep::Replay, queue_.current()});
            restart = true;
        } else {
            wantPaused_ = !wantPaused_;
        }
    }
    if (!restart) {
        applyPause();
        if (wake_ >= 0) sceKernelSignalSema(wake_, 1);
    }
}

void MusicSession::next() {
    std::lock_guard<std::mutex> lock(mutex_);
    queueStep(queue_.next());
}

void MusicSession::previous() {
    std::lock_guard<std::mutex> lock(mutex_);
    queueStep(queue_.previous(positionMs_));
}

void MusicSession::jump(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    queueStep(queue_.jump(index));
}

void MusicSession::seekBy(int64_t deltaMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == MusicState::Ended || state_ == MusicState::Failed) return;
    int64_t target = (pendingSeekMs_ >= 0 ? pendingSeekMs_ : positionMs_) + deltaMs;
    if (target < 0) target = 0;
    if (durationMs_ > 0) {
        const int64_t maxSeek = durationMs_ > 1000 ? durationMs_ - 1000 : durationMs_;
        if (target > maxSeek) target = maxSeek;
    }
    positionMs_ = target;
    pendingSeekMs_ = target;
    seekCommitAt_ = nowUs() + kSeekCoalesceUs;
    if (seeking_) psp_audio_interrupt();
    post();
}

void MusicSession::cycleRepeat() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.cycleRepeat();
}

void MusicSession::cycleLyricTrack() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lyrics_ && !lyrics_->empty()) lyricTrack_ = (lyricTrack_ + 1) % lyrics_->size();
}

void MusicSession::applyPause() {
    bool paused;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paused = wantPaused_;
    }
    psp_audio_pause(paused);
}

void MusicSession::setState(MusicState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
}

void MusicSession::setFailed(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = MusicState::Failed;
    error_ = message;
}

void MusicSession::releaseSource(BiliPlayback& playback) {
    bili_finish_playback(&playback);
    playback = BiliPlayback{};
}

void MusicSession::discardNext() {
    if (nextReady_) releaseSource(next_);
    nextReady_ = false;
    nextIndex_ = MusicQueue::npos;
}

bool MusicSession::prepare(size_t index, BiliPlayback& out, std::string& error) {
    BiliVideo video{};
    std::snprintf(video.bvid, sizeof(video.bvid), "%s", source_.bvid.c_str());
    std::snprintf(video.title, sizeof(video.title), "%s", tracks_[index].title.c_str());
    video.cid = static_cast<long long>(tracks_[index].cid);
    video.duration = tracks_[index].durationSec;
    out = BiliPlayback{};
    AppError typed;
    return prepareAudioPlayback(video, out, error, &typed) == 0;
}

void MusicSession::run() {
    size_t first;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        first = queue_.current();
    }
    openTrack(first, 0, false);
    while (!stopping_) {
        SceUInt timeout = kPollUs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (hasStep_) {
                timeout = 1;
            } else if (pendingSeekMs_ >= 0) {
                const uint64_t now = nowUs();
                timeout = now >= seekCommitAt_
                              ? 1
                              : static_cast<SceUInt>(std::min<uint64_t>(kPollUs, seekCommitAt_ - now));
            } else if (lyricsPending_) {
                timeout = 1;
            }
        }
        sceKernelWaitSema(wake_, 1, &timeout);
        if (stopping_) break;
        handleCommands();
        if (!stopping_ && !stepPending()) pollEngine();
    }
    psp_audio_close(0);
    releaseSource(current_);
    discardNext();
}

void MusicSession::handleCommands() {
    while (sceKernelPollSema(wake_, 1) >= 0) {}
    MusicStep step;
    int64_t seek = -1;
    bool hasStep = false;
    size_t lyricsIndex = MusicQueue::npos;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hasStep_) {
            hasStep = true;
            step = pendingStep_;
            hasStep_ = false;
            pendingSeekMs_ = -1;
            seekCommitAt_ = 0;
        } else if (pendingSeekMs_ >= 0 && nowUs() >= seekCommitAt_) {
            seek = pendingSeekMs_;
            pendingSeekMs_ = -1;
            seekCommitAt_ = 0;
            seeking_ = true;
        } else if (pendingSeekMs_ < 0 && lyricsPending_) {
            lyricsPending_ = false;
            lyricsIndex = queue_.current();
        }
    }
    if (prefetching_ == false && loadingLyrics_ == false) transfer_cancel = 0;
    if (hasStep) playStep(step);
    else if (seek >= 0) seekTo(seek);
    else if (lyricsIndex != MusicQueue::npos) loadLyrics(lyricsIndex);
}

void MusicSession::playStep(const MusicStep& step, bool drain) {
    if (step.kind == MusicStep::None) return;
    if (step.kind == MusicStep::Replay) {
        // A running (or naturally ended) engine restarts the same source at zero;
        // after a failure the source is gone and the part is opened again.
        PspAudioStatus status;
        psp_audio_status(&status);
        const bool live = engineOpen_ && status.state != PSP_AUDIO_IDLE &&
                          status.state != PSP_AUDIO_FAILED;
        if (live) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                seeking_ = true;
            }
            seekTo(0);
            return;
        }
    }
    recoverAttempts_ = 0;  // an explicit request starts a fresh recovery budget
    openTrack(step.index, 0, drain);
}

bool MusicSession::openTrack(size_t index, int64_t startMs, bool drain) {
    bool paused;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.moveTo(index);
        state_ = MusicState::Opening;
        if (pendingSeekMs_ < 0) positionMs_ = startMs;
        durationMs_ = static_cast<int64_t>(tracks_[index].durationSec) * 1000;
        error_.clear();
        lyrics_.reset();
        lyricTrack_ = 0;
        lyricsPending_ = false;
        paused = wantPaused_;
    }
    psp_audio_close(drain);
    engineOpen_ = false;
    releaseSource(current_);
    if (stopping_ || stepPending()) return false;
    transfer_cancel = 0;
    std::string error;
    if (nextReady_ && nextIndex_ == index) {
        current_ = next_;
        next_ = BiliPlayback{};
        nextReady_ = false;
        nextIndex_ = MusicQueue::npos;
    } else {
        discardNext();
        if (!prepare(index, current_, error)) {
            releaseSource(current_);
            if (!stopping_ && !stepPending()) setFailed(error.empty() ? "audio_unavailable" : error);
            return false;
        }
    }
    if (stopping_ || stepPending()) {
        releaseSource(current_);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingSeekMs_ >= 0) {
            startMs = pendingSeekMs_;
            if (nowUs() >= seekCommitAt_) {
                pendingSeekMs_ = -1;
                seekCommitAt_ = 0;
            }
        }
        paused = wantPaused_;
    }
    PspAudioOptions options{};
    options.source = bili_playback_audio_source(&current_);
    options.start_ms = startMs;
    options.start_paused = paused;
    options.duration_hint_ms = static_cast<int64_t>(tracks_[index].durationSec) * 1000;
    const int ret = options.source ? psp_audio_open(&options) : -1;
    if (ret < 0) {
        releaseSource(current_);
        if (!stopping_ && !stepPending()) setFailed("audio_open_failed:" + std::to_string(ret));
        return false;
    }
    engineOpen_ = true;
    applyPause();  // a toggle during the open must not be lost
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasStep_) {
            state_ = wantPaused_ ? MusicState::Paused : MusicState::Playing;
            lyricsPending_ = true;
        }
    }
    return true;
}

bool MusicSession::seekTo(int64_t ms) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingSeekMs_ < 0) positionMs_ = ms;
        if (state_ != MusicState::Paused) state_ = MusicState::Buffering;
    }
    const int ret = psp_audio_seek(ms);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seeking_ = false;
    }
    if (ret >= 0) {
        applyPause();
        return true;
    }
    if (stopping_ || stepPending() || seekPending()) return false;
    return recover(ms);
}

// The source URL can expire or the connection can drop on a long session: prepare the
// part again and resume where it stopped. Bounded, so a dead network ends in Failed.
bool MusicSession::recover(int64_t positionMs) {
    const uint64_t now = nowUs();
    if (now - recoverWindowStart_ > kRecoverWindowUs) {
        recoverWindowStart_ = now;
        recoverAttempts_ = 0;
    }
    if (++recoverAttempts_ > kRecoverLimit) {
        psp_audio_close(0);
        engineOpen_ = false;
        releaseSource(current_);
        setFailed("playback_failed");
        return false;
    }
    size_t index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        index = queue_.current();
    }
    return openTrack(index, positionMs, false);
}

void MusicSession::maybePrefetch(int64_t positionMs, int64_t durationMs) {
    if (nextReady_ || prefetching_ || nowUs() < prefetchRetryAt_) return;
    size_t index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_.shouldPrefetch(positionMs, durationMs)) return;
        index = queue_.prefetchIndex();
    }
    if (index == MusicQueue::npos) return;
    prefetching_ = true;
    transfer_cancel = 0;
    BiliPlayback prepared{};
    std::string error;
    const bool ok = prepare(index, prepared, error);
    prefetching_ = false;
    if (ok && !stopping_) {
        next_ = prepared;
        nextIndex_ = index;
        nextReady_ = true;
    } else {
        if (ok) releaseSource(prepared);
        prefetchRetryAt_ = nowUs() + kPrefetchRetryUs;
    }
}

void MusicSession::loadLyrics(size_t index) {
    loadingLyrics_ = true;
    transfer_cancel = 0;
    bool failed = false;
    auto tracks = subtitleTracks(source_.bvid, tracks_[index].cid, source_.aid, defaultHttpClient(), failed);
    if (tracks.size() > kMaxLyricTracks) tracks.resize(kMaxLyricTracks);
    for (auto& track : tracks) {
        if (stopping_ || transfer_cancel) break;
        auto response = defaultHttpClient().get(track.url, ErrorOperation::Request, false);
        if (response) track.cues = parseSubtitles(response.value.body);
    }
    loadingLyrics_ = false;
    if (stopping_) return;
    if (transfer_cancel) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasStep_ && queue_.current() == index) lyricsPending_ = true;
        return;
    }
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                [](const SubtitleTrack& track) { return track.cues.empty(); }),
                 tracks.end());
    if (tracks.empty()) return;
    auto shared = std::make_shared<const std::vector<SubtitleTrack>>(std::move(tracks));
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.current() == index) lyrics_ = std::move(shared);
}

void MusicSession::pollEngine() {
    PspAudioStatus status;
    psp_audio_status(&status);
    switch (status.state) {
    case PSP_AUDIO_IDLE:
    case PSP_AUDIO_OPENING:
        return;
    case PSP_AUDIO_FAILED:
        if (!seekPending() && !stepPending()) recover(status.position_ms);
        return;
    case PSP_AUDIO_ENDED: {
        MusicStep step;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pendingSeekMs_ >= 0 || hasStep_) return;
            step = queue_.onEnded();
            positionMs_ = status.duration_ms > 0 ? status.duration_ms : status.position_ms;
        }
        if (step.kind == MusicStep::None) {
            psp_audio_close(true);
            engineOpen_ = false;
            releaseSource(current_);
            discardNext();
            setState(MusicState::Ended);
        } else {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.moveTo(step.index);
            }
            playStep(step, true);
        }
        return;
    }
    default:
        break;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingSeekMs_ < 0) positionMs_ = status.position_ms;
        if (status.duration_ms > 0) durationMs_ = status.duration_ms;
        state_ = status.state == PSP_AUDIO_PAUSED ? MusicState::Paused
               : status.state == PSP_AUDIO_BUFFERING ? MusicState::Buffering : MusicState::Playing;
    }
    // Playing steadily again: forget earlier recoveries.
    if (status.state == PSP_AUDIO_PLAYING && status.position_ms > 5000) recoverAttempts_ = 0;
    // Hold the suspend timer off so Wi-Fi and audio survive a blank screen. The backlight
    // timer is left alone: the system's own setting turns the screen off.
    const uint64_t now = nowUs();
    if (status.state != PSP_AUDIO_PAUSED && now - lastPowerTick_ >= kPowerTickUs) {
        scePowerTick(PSP_POWER_TICK_SUSPEND);
        lastPowerTick_ = now;
    }
    maybePrefetch(status.position_ms, status.duration_ms);
}

}  // namespace pspapp
