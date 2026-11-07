#include "update_controller.hpp"
#include <cstdio>
namespace pspapp {

UpdateController::UpdateController(std::string root, std::string running, TransportFactory factory, UpdatePolicy policy)
    : root_(std::move(root)), running_(std::move(running)), factory_(std::move(factory)), policy_(std::move(policy)) {}

void UpdateController::set(UpdateState s, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.state = s;
    snap_.error = error;
}
bool UpdateController::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_.state == UpdateState::Checking || snap_.state == UpdateState::Downloading ||
           snap_.state == UpdateState::Staging;
}
UpdateSnapshot UpdateController::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_;
}
void UpdateController::cancel() { cancel_ = true; }
void UpdateController::setSource(size_t index) {
    if (busy() || index >= 2) return;
    std::lock_guard<std::mutex> lock(mutex_);
    source_ = index;
    snap_ = UpdateSnapshot{};
}
void UpdateController::reset() {
    if (busy()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    snap_ = UpdateSnapshot{};
}

void UpdateController::runCheck() {
    if (busy()) return;
    cancel_ = false;
    set(UpdateState::Checking);
    auto transport = factory_(&cancel_);
    ReleaseInfo info;
    size_t source;
    { std::lock_guard<std::mutex> lock(mutex_); source = source_; }
    UpdateResult r = checkForUpdate(*transport, kUpdateSources[source].apiUrl, running_, info);
    if (cancel_) { set(UpdateState::Idle); return; }
    if (r == UpdateResult::NoUpdate) { set(UpdateState::UpToDate); return; }
    if (r != UpdateResult::Ok) { set(UpdateState::Failed, updateResultName(r)); return; }
    std::lock_guard<std::mutex> lock(mutex_);
    info_ = info;
    snap_.tag = info.tag;
    snap_.notes = info.notes;
    snap_.source = kUpdateSources[source].name;
    snap_.total = info.zipSize;
    snap_.done = 0;
    snap_.error.clear();
    snap_.state = UpdateState::Available;
}

void UpdateController::runInstall() {
    ReleaseInfo info;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snap_.state != UpdateState::Available) return;
        info = info_;
    }
    cancel_ = false;
    std::string reason;
    if (policy_.preflight && !policy_.preflight(reason)) { set(UpdateState::Failed, reason); return; }
    if (policy_.freeSpace) {
        uint64_t free = policy_.freeSpace();
        if (free && info.zipSize && free < requiredFreeSpace(info.zipSize)) { set(UpdateState::Failed, "not enough free space"); return; }
    }
    set(UpdateState::Downloading);
    auto transport = factory_(&cancel_);
    const std::string part = root_ + "/update.part", zip = root_ + "/update.zip";
    UpdateResult r = downloadRelease(*transport, info, part, zip, [this](const UpdateProgress& p) {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.done = p.done;
        if (p.total) snap_.total = p.total;
        return !cancel_;
    });
    if (r == UpdateResult::Cancelled || cancel_) { set(UpdateState::Available); return; } // .part kept for resume
    if (r != UpdateResult::Ok) { set(UpdateState::Failed, updateResultName(r)); return; }
    set(UpdateState::Staging);
    // The zip was just verified by downloadRelease; stageRelease verifies once more from disk.
    std::string sha = info.sha256;
    if (sha.empty()) {
        // SHA256SUMS path: recompute from the stored file so the staged bytes are what was checked.
        std::string sums;
        auto t2 = factory_(&cancel_);
        bool ok = t2->get(info.sumsUrl, 0, [](int s, const std::string&) { return s == 200; },
                          [&](const void* d, size_t n) { if (sums.size() + n > 16384) return false; sums.append((const char*)d, n); return true; });
        if (!ok || !findChecksum(sums, info.zipName, sha)) { set(UpdateState::Failed, "bad release data"); return; }
    }
    r = stageRelease(zip, sha, root_);
    std::remove(zip.c_str());
    if (r != UpdateResult::Ok) { set(UpdateState::Failed, updateResultName(r)); return; }
    set(UpdateState::Ready);
}
}
