#pragma once
// Download and staging half of the updater (issue #26). stdio only, so it is
// identical on the PSP and in host tests. The network is behind UpdateTransport.
#include "update_core.hpp"
#include "update_package.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Parallel Range connections used for the release zip (1 = single connection).
// Set with -DBILI_UPDATE_CONNECTIONS=N in CMake; host tests override it with -D.
#ifndef BILI_UPDATE_CONNECTIONS
#define BILI_UPDATE_CONNECTIONS 4
#endif
#define BILI_UPDATE_MAX_CONNECTIONS 8
static_assert(BILI_UPDATE_CONNECTIONS >= 1 && BILI_UPDATE_CONNECTIONS <= BILI_UPDATE_MAX_CONNECTIONS,
              "BILI_UPDATE_CONNECTIONS must be between 1 and 8");
namespace pspapp {

// One HTTP GET, optionally ranged. Redirects are followed by the implementation.
class UpdateTransport {
  public:
    virtual ~UpdateTransport() = default;
    // onHeaders runs once with the final status and Content-Range ("" if none);
    // returning false aborts. onData receives body bytes; false aborts.
    virtual bool get(const std::string& url, uint64_t rangeFrom,
                     const std::function<bool(int status, const std::string& contentRange)>& onHeaders,
                     const std::function<bool(const void*, size_t)>& onData) = 0;
    // Like get() for bytes [from, to] inclusive. The default implementation calls get() and
    // stops reading at `to` (returning false); transports with real Range support override it.
    // A transport object serves one request at a time: parallel downloads use one per connection.
    virtual bool getRange(const std::string& url, uint64_t from, uint64_t to,
                          const std::function<bool(int status, const std::string& contentRange)>& onHeaders,
                          const std::function<bool(const void*, size_t)>& onData);
    // Short reason for the last failed get() ("Couldn't connect to server"), for the UI and logs.
    virtual std::string lastError() const { return {}; }
};

enum class UpdateResult { Ok, NoUpdate, Network, BadRelease, Cancelled, ChecksumMismatch, Package, Storage };
const char* updateResultName(UpdateResult r);

struct UpdateProgress { uint64_t done = 0, total = 0; };
using ProgressFn = std::function<bool(const UpdateProgress&)>; // false = cancel

// GET the release JSON (size-capped) and parse it. Returns NoUpdate if the
// newest stable release is not newer than `running`.
UpdateResult checkForUpdate(UpdateTransport& t, const std::string& apiUrl, const std::string& running, ReleaseInfo& out);

// Selectable release sources (Codeberg mirrors GitHub, see docs/auto-update.md).
// The user picks one on the update page; there is no automatic fallback.
struct UpdateSource { const char* name; const char* apiUrl; };
extern const UpdateSource kUpdateSources[2];
// Optional multi-connection download. Without makeTransport and runParallel (or when the
// size is unknown or small) the zip is fetched over the single transport as before.
struct DownloadOptions {
    // A fresh transport for each extra connection; the caller's transport serves the first.
    std::function<std::unique_ptr<UpdateTransport>()> makeTransport;
    // Runs every task and returns when all have finished. Tasks run at the same time;
    // the implementation may run the first one on the calling thread.
    std::function<void(std::vector<std::function<void()>>&)> runParallel;
    unsigned connections = BILI_UPDATE_CONNECTIONS;
    uint64_t minSegment = 256u << 10; // never make segments smaller than this
};

// Downloads info.zipUrl to `partPath`, resuming from an existing file, then
// verifies SHA-256 (API digest or SHA256SUMS). On success the file is renamed
// to `zipPath`. Interrupted downloads keep the .part file for the next try;
// a checksum mismatch deletes it.
// With options, the zip is split into equal segments fetched in parallel: segment 0 is
// `partPath` itself (always a valid prefix of the zip), segment i is
// `partPath.<n>.<i>`; the segments are appended to `partPath` once all are complete.
UpdateResult downloadRelease(UpdateTransport& t, const ReleaseInfo& info, const std::string& partPath,
                             const std::string& zipPath, const ProgressFn& progress,
                             const DownloadOptions* options = nullptr);

// Verifies again, extracts "WiliMini/..." into <root>/pending/ and writes
// <root>/pending/MANIFEST last. Anything already in pending/ is removed first.
UpdateResult stageRelease(const std::string& zipPath, const std::string& expectedSha256, const std::string& root);

// Space the update needs on the Memory Stick: zip + extracted + backups.
uint64_t requiredFreeSpace(uint64_t zipSize);
}
