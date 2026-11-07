#pragma once
// Platform-independent core of the self-update feature (issue #26):
// version comparison, release JSON parsing and the swap journal.
// No PSP or network dependencies; everything here is host-testable.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace pspapp {

struct SemVer {
    int major = 0, minor = 0, patch = 0;
    bool prerelease = false;
};
// Accepts "v1.2.3", "1.2", "1.2.3-rc1". Build suffixes such as git-describe
// output ("1.2.3-4-gabcdef") count as prerelease. Returns false when invalid.
bool parseSemVer(std::string_view text, SemVer& out);
// -1, 0, 1. A prerelease sorts below the same numeric version.
int compareSemVer(const SemVer& a, const SemVer& b);
// True only for a strictly newer stable release: never offers a downgrade.
bool isUpdateAvailable(std::string_view running, std::string_view candidate);

struct ReleaseInfo {
    std::string tag;
    std::string notes;      // truncated to kMaxReleaseNotes
    std::string zipUrl;
    std::string zipName;
    std::string sha256;     // lowercase hex, empty if unknown
    std::string sumsUrl;    // SHA256SUMS asset, empty if absent
    uint64_t zipSize = 0;
};
constexpr size_t kMaxReleaseJson = 256 * 1024;
constexpr size_t kMaxReleaseNotes = 4096;
// Parses GET /releases/latest. Rejects drafts, prereleases and payloads
// without a usable "WiliMini-<tag>.zip" asset. Works for GitHub and Codeberg
// (Forgejo) payloads, which share this shape.
bool parseRelease(std::string_view json, ReleaseInfo& out);
// Looks up `name` in SHA256SUMS text ("<hex>  <name>" per line).
bool findChecksum(std::string_view sums, std::string_view name, std::string& hexOut);

// Journaled swap: each step is recorded before and after it runs so a power
// loss can be resumed. Steps are idempotent: re-running a done step is safe.
enum class SwapOp : uint8_t { BackupOld, InstallNew, Cleanup };
struct SwapStep {
    SwapOp op;
    std::string path;       // relative to the package root
    bool done = false;
};
struct SwapJournal {
    std::vector<SwapStep> steps;
};
// Plans a swap over the manifest of files (user data not in the manifest is
// never touched). The loader's EBOOT.PBP is always ordered last.
SwapJournal planSwap(const std::vector<std::string>& manifest);
std::string serializeJournal(const SwapJournal& j);
bool parseJournal(std::string_view text, SwapJournal& out);
// Index of the first step not done, or steps.size() when finished.
size_t nextStep(const SwapJournal& j);
// True if the path is safe to extract: relative, no "..", no backslash/drive.
bool isSafeArchivePath(std::string_view path);
}
