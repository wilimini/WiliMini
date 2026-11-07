#include "update_install.hpp"
#include <cstdio>
#include <cstring>
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

const UpdateSource kUpdateSources[2] = {
    {"GitHub", "https://api.github.com/repos/wilimini/WiliMini/releases/latest"},
    {"Codeberg", "https://codeberg.org/api/v1/repos/wilimini/WiliMini/releases/latest"},
};

UpdateResult checkForUpdateAny(UpdateTransport& t, const UpdateSource* sources, size_t count,
                               const std::string& running, ReleaseInfo& out, const char** usedSource) {
    UpdateResult last = UpdateResult::Network;
    for (size_t i = 0; i < count; ++i) {
        last = checkForUpdate(t, sources[i].apiUrl, running, out);
        if (usedSource) *usedSource = sources[i].name;
        if (last != UpdateResult::Network) return last;
    }
    return last;
}

UpdateResult downloadRelease(UpdateTransport& t, const ReleaseInfo& info, const std::string& partPath,
                             const std::string& zipPath, const ProgressFn& progress) {
    std::string expected = info.sha256;
    if (expected.empty()) {
        std::string sums;
        UpdateResult r = fetchText(t, info.sumsUrl, 16384, sums);
        if (r != UpdateResult::Ok) return r;
        if (!findChecksum(sums, info.zipName, expected)) return UpdateResult::BadRelease;
    }
    for (int attempt = 0; attempt < 2; ++attempt) { // second pass: server refused our range, start over
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
