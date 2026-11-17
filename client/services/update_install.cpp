#include "update_install.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include "wili_update_swap.h"
#include <vector>
namespace pspapp {

const char* updateResultName(UpdateResult r) {
    switch (r) {
    case UpdateResult::Ok: return "ok";
    case UpdateResult::NoUpdate: return "no update";
    case UpdateResult::Network: return "network error";
    case UpdateResult::BadRelease: return "bad release data";
    case UpdateResult::Cancelled: return "cancelled";
    case UpdateResult::ChecksumMismatch: return "checksum mismatch";
    case UpdateResult::Package: return "bad package";
    case UpdateResult::Storage: return "storage error";
    }
    return "?";
}
bool UpdateTransport::getRange(const std::string& url, uint64_t from, uint64_t to,
                               const std::function<bool(int, const std::string&)>& onHeaders,
                               const std::function<bool(const void*, size_t)>& onData) {
    uint64_t remaining = to == UINT64_MAX ? UINT64_MAX : to - from + 1;
    return get(url, from, onHeaders, [&](const void* d, size_t n) {
        size_t take = (size_t)std::min<uint64_t>(n, remaining);
        if (take && !onData(d, take)) return false;
        remaining -= take;
        return remaining != 0; // reached `to`: abort the rest of the body
    });
}

uint64_t requiredFreeSpace(uint64_t zipSize) { return zipSize * 2 + (1u << 20); }

namespace {
struct FileSource : ByteSource {
    FILE* f;
    uint64_t n = 0;
    explicit FileSource(const std::string& p) : f(std::fopen(p.c_str(), "rb")) {
        if (f && std::fseek(f, 0, SEEK_END) == 0) { long l = std::ftell(f); n = l > 0 ? (uint64_t)l : 0; }
    }
    ~FileSource() override { if (f) std::fclose(f); }
    uint64_t size() const override { return n; }
    size_t read(uint64_t off, void* dst, size_t len) override {
        if (!f || std::fseek(f, (long)off, SEEK_SET) != 0) return 0;
        return std::fread(dst, 1, len, f);
    }
};
bool fileSize(const std::string& p, uint64_t& out) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    out = (uint64_t)st.st_size;
    return true;
}
bool mkdirs(const std::string& filePath) {
    for (size_t i = 1; i < filePath.size(); ++i) {
        if (filePath[i] != '/' || filePath[i - 1] == ':') continue;
        std::string d = filePath.substr(0, i);
        struct stat st;
        if (stat(d.c_str(), &st) != 0 && mkdir(d.c_str(), 0777) != 0) return false;
    }
    return true;
}
// Collects a small response body, refusing anything over `cap`.
struct Capped { std::string body; size_t cap; bool over = false; };
UpdateResult fetchText(UpdateTransport& t, const std::string& url, size_t cap, std::string& out) {
    Capped c{{}, cap};
    int status = 0;
    bool ok = t.get(url, 0,
        [&](int s, const std::string&) { status = s; return s == 200; },
        [&](const void* d, size_t n) {
            if (c.body.size() + n > c.cap) { c.over = true; return false; }
            c.body.append((const char*)d, n);
            return true;
        });
    if (c.over) return UpdateResult::BadRelease;
    if (!ok || status != 200) return UpdateResult::Network;
    out = std::move(c.body);
    return UpdateResult::Ok;
}
struct FileWrite : FileSink {
    std::string root, cur;
    FILE* f = nullptr;
    std::vector<std::string> names;
    bool begin(const std::string& path, uint64_t) override {
        if (path.compare(0, 9, "WiliMini/") != 0 || path.size() == 9) return false;
        std::string rel = path.substr(9);
        if (!wili_ok(rel)) return false;
        cur = root + "/pending/" + rel;
        if (!mkdirs(cur)) return false;
        f = std::fopen(cur.c_str(), "wb");
        if (!f) return false;
        names.push_back(rel);
        return true;
    }
    // Must agree with the loader, which discards a manifest containing anything else.
    static bool wili_ok(const std::string& rel) { return rel == "EBOOT.PBP" || wili_swap_path_ok(rel.c_str()); }
    bool write(const void* d, size_t n) override { return std::fwrite(d, 1, n, f) == n; }
    bool end(bool ok) override {
        bool closed = f && std::fclose(f) == 0;
        f = nullptr;
        if (!ok || !closed) { std::remove(cur.c_str()); return ok ? false : true; }
        return true;
    }
};
void removeTree(const std::string& root, const std::vector<std::string>& names) {
    for (auto& n : names) std::remove((root + "/pending/" + n).c_str());
    std::remove((root + "/pending/MANIFEST").c_str());
}
} // namespace

UpdateResult checkForUpdate(UpdateTransport& t, const std::string& apiUrl, const std::string& running, ReleaseInfo& out) {
    std::string json;
    UpdateResult r = fetchText(t, apiUrl, kMaxReleaseJson, json);
    if (r != UpdateResult::Ok) return r;
    ReleaseInfo info;
    if (!parseRelease(json, info)) return UpdateResult::BadRelease;
    if (!isUpdateAvailable(running, info.tag)) return UpdateResult::NoUpdate;
    out = std::move(info);
    return UpdateResult::Ok;
}

#ifdef BILI_UPDATE_TEST_ENDPOINT
// Test builds only (-DBILI_UPDATE_TEST_ENDPOINT=https://127.0.0.1:PORT): tools/ppsspp/test-update-flow.py
// serves both sources locally. Never set for release builds.
const UpdateSource kUpdateSources[2] = {
    {"GitHub", BILI_UPDATE_TEST_ENDPOINT "/github/releases/latest"},
    {"Codeberg", BILI_UPDATE_TEST_ENDPOINT "/codeberg/releases/latest"},
};
#else
const UpdateSource kUpdateSources[2] = {
    {"GitHub", "https://api.github.com/repos/wilimini/WiliMini/releases/latest"},
    {"Codeberg", "https://codeberg.org/api/v1/repos/wilimini/WiliMini/releases/latest"},
};
#endif

// ---- Multi-connection download ----
namespace {
std::string segmentPath(const std::string& part, unsigned n, unsigned i) {
    return part + "." + std::to_string(n) + "." + std::to_string(i);
}
// Deletes segment files of every layout except `keepN` (0 = delete all).
void removeSegments(const std::string& part, unsigned keepN) {
    for (unsigned n = 2; n <= BILI_UPDATE_MAX_CONNECTIONS; ++n) {
        if (n == keepN) continue;
        for (unsigned i = 1; i < n; ++i) std::remove(segmentPath(part, n, i).c_str());
    }
}
enum class ParallelResult { Complete, Fallback, Cancelled, Network, Storage };
struct ParallelState {
    std::mutex m;
    bool stop = false, cancelled = false, fallback = false, storage = false;
    uint64_t done = 0;
};
struct Segment { uint64_t start = 0, len = 0, have = 0; std::string path; };

// Appends `from` to `to` and deletes `from`.
bool appendFile(const std::string& from, const std::string& to) {
    FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = std::fopen(to.c_str(), "ab");
    bool ok = out != nullptr;
    std::vector<char> buf(32768);
    while (ok) {
        size_t n = std::fread(buf.data(), 1, buf.size(), in);
        if (n == 0) break;
        ok = std::fwrite(buf.data(), 1, n, out) == n;
    }
    ok = ok && !std::ferror(in);
    std::fclose(in);
    if (out) ok = std::fclose(out) == 0 && ok;
    if (ok) std::remove(from.c_str());
    return ok;
}

ParallelResult downloadParallel(UpdateTransport& t, const ReleaseInfo& info, const std::string& partPath,
                                const ProgressFn& progress, const DownloadOptions& opt, unsigned requested) {
    const uint64_t size = info.zipSize;
    const uint64_t seg = (size + requested - 1) / requested;
    const unsigned n = (unsigned)((size + seg - 1) / seg);
    std::vector<Segment> segs(n);
    for (unsigned i = 0; i < n; ++i) {
        segs[i].start = i * seg;
        segs[i].len = std::min<uint64_t>(seg, size - segs[i].start);
        segs[i].path = i ? segmentPath(partPath, n, i) : partPath;
    }
    removeSegments(partPath, n); // leftovers of a build with a different connection count
    uint64_t prefix = 0;
    fileSize(partPath, prefix);
    // A prefix past segment 0 (single-connection resume, interrupted merge) is continued alone.
    if (prefix > segs[0].len) return ParallelResult::Fallback;
    segs[0].have = prefix;
    for (unsigned i = 1; i < n; ++i) {
        uint64_t h = 0;
        fileSize(segs[i].path, h);
        if (h > segs[i].len) { std::remove(segs[i].path.c_str()); h = 0; }
        segs[i].have = h;
    }

    ParallelState st;
    for (auto& s : segs) st.done += s.have;
    std::vector<std::unique_ptr<UpdateTransport>> owned;
    std::vector<std::function<void()>> tasks;
    for (unsigned i = 0; i < n; ++i) {
        if (segs[i].have >= segs[i].len) continue;
        UpdateTransport* tr = &t;
        if (!tasks.empty()) {
            owned.push_back(opt.makeTransport());
            if (!owned.back()) return ParallelResult::Fallback;
            tr = owned.back().get();
        }
        tasks.push_back([&, i, tr] {
            Segment& s = segs[i];
            const uint64_t offset = s.start + s.have, want = s.len - s.have;
            uint64_t got = 0;
            FILE* f = nullptr;
            auto fail = [&](bool ParallelState::*flag) {
                std::lock_guard<std::mutex> lock(st.m);
                if (flag) st.*flag = true;
                st.stop = true;
                return false;
            };
            tr->getRange(info.zipUrl, offset, s.start + s.len - 1,
                [&](int status, const std::string& cr) {
                    ResumeAction a = resumeAction(status, cr, offset, size);
                    if (a == ResumeAction::Restart) return fail(&ParallelState::fallback); // no Range support
                    if (a != ResumeAction::Append) return false;                           // HTTP error: resume later
                    f = std::fopen(s.path.c_str(), s.have ? "ab" : "wb");
                    return f ? true : fail(&ParallelState::storage);
                },
                [&](const void* d, size_t len) {
                    if (got + len > want) return false; // longer than requested
                    {
                        std::lock_guard<std::mutex> lock(st.m);
                        if (st.stop) return false;
                    }
                    if (std::fwrite(d, 1, len, f) != len) return fail(&ParallelState::storage);
                    got += len;
                    std::lock_guard<std::mutex> lock(st.m);
                    st.done += len;
                    if (progress && !progress(UpdateProgress{st.done, size})) {
                        st.cancelled = st.stop = true;
                        return false;
                    }
                    return true;
                });
            if (f && std::fclose(f) != 0) fail(&ParallelState::storage);
        });
    }
    if (tasks.size() == 1) tasks[0]();
    else if (!tasks.empty()) opt.runParallel(tasks);

    if (st.cancelled) return ParallelResult::Cancelled;
    if (st.storage) return ParallelResult::Storage;
    if (st.fallback) return ParallelResult::Fallback;
    for (auto& s : segs) {
        uint64_t h = 0;
        if (!fileSize(s.path, h) || h != s.len) return ParallelResult::Network; // short: keep for resume
    }
    for (unsigned i = 1; i < n; ++i)
        if (!appendFile(segs[i].path, partPath)) return ParallelResult::Storage;
    return ParallelResult::Complete;
}
} // namespace

UpdateResult downloadRelease(UpdateTransport& t, const ReleaseInfo& info, const std::string& partPath,
                             const std::string& zipPath, const ProgressFn& progress,
                             const DownloadOptions* options) {
    std::string expected = info.sha256;
    if (expected.empty()) {
        std::string sums;
        UpdateResult r = fetchText(t, info.sumsUrl, 16384, sums);
        if (r != UpdateResult::Ok) return r;
        if (!findChecksum(sums, info.zipName, expected)) return UpdateResult::BadRelease;
    }
    unsigned connections = 1;
    if (options && options->runParallel && options->makeTransport && info.zipSize)
        connections = (unsigned)std::min<uint64_t>(std::min<unsigned>(options->connections, BILI_UPDATE_MAX_CONNECTIONS),
                                                    info.zipSize / std::max<uint64_t>(options->minSegment, 1));
    bool parallel = connections >= 2;
    // Extra passes: the server refused our range (start over), or refused parallel Range requests.
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (parallel) {
            ParallelResult pr = downloadParallel(t, info, partPath, progress, *options, connections);
            if (pr == ParallelResult::Fallback) { removeSegments(partPath, 0); parallel = false; continue; }
            if (pr == ParallelResult::Cancelled) return UpdateResult::Cancelled;
            if (pr == ParallelResult::Storage) return UpdateResult::Storage;
            if (pr == ParallelResult::Network) return UpdateResult::Network;
        } else {
            removeSegments(partPath, 0);
            uint64_t have = 0;
            fileSize(partPath, have);
            if (info.zipSize && have > info.zipSize) { std::remove(partPath.c_str()); have = 0; }
            bool restart = false, aborted = false, openFailed = false;
            FILE* f = nullptr;
            UpdateProgress prog{have, info.zipSize};
            if (have == 0 || have != info.zipSize) {
                bool ok = t.get(info.zipUrl, have,
                    [&](int status, const std::string& cr) {
                        if (have == 0) {
                            if (status != 200) return false;
                        } else {
                            ResumeAction a = resumeAction(status, cr, have, info.zipSize);
                            if (a == ResumeAction::Fail) return false;
                            if (a == ResumeAction::Restart) { restart = true; return false; }
                        }
                        f = std::fopen(partPath.c_str(), have ? "ab" : "wb");
                        openFailed = f == nullptr;
                        return f != nullptr;
                    },
                    [&](const void* d, size_t n) {
                        if (info.zipSize && prog.done + n > info.zipSize) return false; // longer than advertised
                        if (std::fwrite(d, 1, n, f) != n) return false;
                        prog.done += n;
                        if (progress && !progress(prog)) { aborted = true; return false; }
                        return true;
                    });
                if (f) { std::fflush(f); std::fclose(f); }
                if (aborted) return UpdateResult::Cancelled;
                if (restart) { std::remove(partPath.c_str()); continue; }
                if (!ok) return openFailed ? UpdateResult::Storage : UpdateResult::Network; // .part kept for resume
            }
        }
        uint64_t size = 0;
        if (!fileSize(partPath, size) || (info.zipSize && size != info.zipSize)) {
            if (info.zipSize && size > info.zipSize) std::remove(partPath.c_str());
            return UpdateResult::Network; // short: keep for resume
        }
        FileSource src(partPath);
        std::string got = sha256Hex(src);
        if (got != expected) { std::remove(partPath.c_str()); return UpdateResult::ChecksumMismatch; }
        std::remove(zipPath.c_str());
        if (std::rename(partPath.c_str(), zipPath.c_str()) != 0) return UpdateResult::Storage;
        return UpdateResult::Ok;
    }
    return UpdateResult::Network;
}

UpdateResult stageRelease(const std::string& zipPath, const std::string& expectedSha256, const std::string& root) {
    FileSource src(zipPath);
    if (!src.f) return UpdateResult::Storage;
    if (sha256Hex(src) != expectedSha256) return UpdateResult::ChecksumMismatch;
    std::vector<ZipEntry> entries;
    if (listZip(src, entries) != ZipError::Ok) return UpdateResult::Package;
    bool hasPrx = false, hasEboot = false;
    for (auto& e : entries) {
        if (e.path == "WiliMini/EBOOT.PBP") hasEboot = true;
        if (e.path == "WiliMini/wili_mini.prx") hasPrx = true;
    }
    if (!hasEboot || !hasPrx) return UpdateResult::Package; // not a full package: never stage it
    FileWrite w;
    w.root = root;
    std::remove((root + "/pending/MANIFEST").c_str());
    for (auto& e : entries) {
        if (e.isDir) continue;
        ZipError z = extractEntry(src, e, w);
        if (z != ZipError::Ok) { removeTree(root, w.names); return z == ZipError::Sink ? UpdateResult::Storage : UpdateResult::Package; }
    }
    std::string manifest;
    for (auto& n : w.names) manifest += n + "\n";
    std::string mp = root + "/pending/MANIFEST";
    FILE* m = std::fopen(mp.c_str(), "wb");
    if (!m) { removeTree(root, w.names); return UpdateResult::Storage; }
    bool ok = std::fwrite(manifest.data(), 1, manifest.size(), m) == manifest.size();
    ok = std::fclose(m) == 0 && ok;
    if (!ok) { removeTree(root, w.names); return UpdateResult::Storage; }
    return UpdateResult::Ok;
}
}
