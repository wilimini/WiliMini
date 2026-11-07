#pragma once
// Download and staging half of the updater (issue #26). stdio only, so it is
// identical on the PSP and in host tests. The network is behind UpdateTransport.
#include "update_core.hpp"
#include "update_package.hpp"
#include <cstdint>
#include <functional>
#include <string>
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
};

enum class UpdateResult { Ok, NoUpdate, Network, BadRelease, Cancelled, ChecksumMismatch, Package, Storage };
const char* updateResultName(UpdateResult r);

struct UpdateProgress { uint64_t done = 0, total = 0; };
using ProgressFn = std::function<bool(const UpdateProgress&)>; // false = cancel

// GET the release JSON (size-capped) and parse it. Returns NoUpdate if the
// newest stable release is not newer than `running`.
UpdateResult checkForUpdate(UpdateTransport& t, const std::string& apiUrl, const std::string& running, ReleaseInfo& out);

// Release endpoints in order of preference. Codeberg mirrors GitHub (see docs/auto-update.md).
struct UpdateSource { const char* name; const char* apiUrl; };
extern const UpdateSource kUpdateSources[2];
// Tries each source until one answers. A source that answers "no update" or
// "bad release" ends the search; only network failures fall through.
UpdateResult checkForUpdateAny(UpdateTransport& t, const UpdateSource* sources, size_t count,
                               const std::string& running, ReleaseInfo& out, const char** usedSource = nullptr);

// Downloads info.zipUrl to `partPath`, resuming from an existing file, then
// verifies SHA-256 (API digest or SHA256SUMS). On success the file is renamed
// to `zipPath`. Interrupted downloads keep the .part file for the next try;
// a checksum mismatch deletes it.
UpdateResult downloadRelease(UpdateTransport& t, const ReleaseInfo& info, const std::string& partPath,
                             const std::string& zipPath, const ProgressFn& progress);

// Verifies again, extracts "WiliMini/..." into <root>/pending/ and writes
// <root>/pending/MANIFEST last. Anything already in pending/ is removed first.
UpdateResult stageRelease(const std::string& zipPath, const std::string& expectedSha256, const std::string& root);

// Space the update needs on the Memory Stick: zip + extracted + backups.
uint64_t requiredFreeSpace(uint64_t zipSize);
}
