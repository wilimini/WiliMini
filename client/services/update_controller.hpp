#pragma once
// Orchestrates check -> download -> stage for the Settings update page
// (issue #26). runCheck()/runInstall() block, so the caller runs them on a
// worker thread; snapshot() and cancel() are safe to call from the UI thread.
#include "update_install.hpp"
#include <memory>
#include <mutex>
namespace pspapp {

enum class UpdateState { Idle, Checking, UpToDate, Available, Downloading, Staging, Ready, Failed };

struct UpdateSnapshot {
    UpdateState state = UpdateState::Idle;
    std::string tag, notes, source, error;
    uint64_t done = 0, total = 0;
};
using TransportFactory = std::function<std::unique_ptr<UpdateTransport>(const volatile bool* cancel)>;
struct UpdatePolicy {
    // Return false and set `reason` to refuse (low battery etc.). May be empty.
    std::function<bool(std::string& reason)> preflight;
    // Free bytes on the storage holding the package, 0 if unknown (not enforced).
    std::function<uint64_t()> freeSpace;
};

class UpdateController {
  public:
    UpdateController(std::string root, std::string runningVersion, TransportFactory factory, UpdatePolicy policy);
    void runCheck();
    void runInstall();       // only from Available
    void cancel();
    void setSource(size_t index); // index into kUpdateSources; ignored while busy
    bool busy() const;       // Checking, Downloading or Staging
    void reset();            // back to Idle unless busy
    UpdateSnapshot snapshot() const;

  private:
    static std::string describe(UpdateResult r, const UpdateTransport* t);
    void set(UpdateState s, const std::string& error = {});
    std::string root_, running_;
    TransportFactory factory_;
    UpdatePolicy policy_;
    mutable std::mutex mutex_;
    UpdateSnapshot snap_;
    ReleaseInfo info_;
    volatile bool cancel_ = false;
    size_t source_ = 0;
};
}
