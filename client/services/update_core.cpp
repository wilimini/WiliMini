#include "update_core.hpp"
extern "C" {
#include "../json.h"
}
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
namespace pspapp {

bool parseSemVer(std::string_view t, SemVer& out) {
    if (!t.empty() && (t[0] == 'v' || t[0] == 'V')) t.remove_prefix(1);
    int vals[3] = {0, 0, 0};
    size_t i = 0;
    int n = 0;
    for (; n < 3; ++n) {
        size_t s = i;
        long v = 0;
        while (i < t.size() && std::isdigit((unsigned char)t[i])) {
            v = v * 10 + (t[i] - '0');
            if (v > 1000000) return false;
            ++i;
        }
        if (i == s) return false;
        vals[n] = (int)v;
        if (i < t.size() && t[i] == '.' && n < 2) { ++i; continue; }
        ++n;
        break;
    }
    bool pre = false;
    if (i < t.size()) {
        if (t[i] != '-' && t[i] != '+') return false;
        pre = t[i] == '-';
    }
    out = {vals[0], vals[1], vals[2], pre};
    return true;
}

int compareSemVer(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.prerelease != b.prerelease) return a.prerelease ? -1 : 1;
    return 0;
}

bool isUpdateAvailable(std::string_view running, std::string_view candidate) {
    SemVer r, c;
    if (!parseSemVer(candidate, c) || c.prerelease) return false;
    // An unparseable running version (e.g. a bare commit hash) is treated as
    // 0.0.0 so a stable release is offered, never a downgrade.
    if (!parseSemVer(running, r)) r = {};
    return compareSemVer(c, r) > 0;
}

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static bool isHex64(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); });
}

// Uses the project's allocation-free token reader (client/json.c) instead of nlohmann:
// the templates cost about 70 KB of the PSP's scarce RAM for one small payload.
namespace {
constexpr int kMaxReleaseTokens = 4096; // GitHub payloads need a few hundred
bool tokenIs(const Json& j, int n, const char* word) {
    if (n < 0) return false;
    const JsonToken& t = j.tokens[n];
    return (size_t)(t.end - t.start) == std::strlen(word) && !std::strncmp(j.text + t.start, word, t.end - t.start);
}
// String member of `object`, "" if missing or not a string.
std::string strMember(const Json& j, int object, const char* key, size_t cap) {
    int n = json_key(&j, object, key);
    if (n < 0) return {};
    std::vector<char> buf(cap + 1);
    int len = json_string(&j, n, buf.data(), buf.size());
    return len < 0 ? std::string() : std::string(buf.data(), (size_t)len);
}
}

bool parseRelease(std::string_view text, ReleaseInfo& out) {
    if (text.size() > kMaxReleaseJson) return false;
    std::string copy(text); // the reader needs a NUL-terminated buffer
    std::vector<JsonToken> tokens(kMaxReleaseTokens);
    Json j;
    if (json_parse(&j, copy.c_str(), tokens.data(), kMaxReleaseTokens) < 0) return false;
    if (j.text[j.tokens[0].start] != '{') return false;
    if (tokenIs(j, json_key(&j, 0, "draft"), "true") || tokenIs(j, json_key(&j, 0, "prerelease"), "true")) return false;
    ReleaseInfo r;
    r.tag = strMember(j, 0, "tag_name", 64);
    SemVer v;
    if (r.tag.empty() || !parseSemVer(r.tag, v) || v.prerelease) return false;
    r.notes = strMember(j, 0, "body", kMaxReleaseNotes); // json_string truncates at the buffer size
    const int assets = json_key(&j, 0, "assets");
    if (assets < 0 || j.text[j.tokens[assets].start] != '[') return false;
    const std::string want = "WiliMini-" + r.tag + ".zip";
    for (int n = assets + 1; n < j.tokens[assets].next; n = j.tokens[n].next) {
        if (j.text[j.tokens[n].start] != '{') continue;
        const std::string name = strMember(j, n, "name", 128);
        const std::string url = strMember(j, n, "browser_download_url", 512);
        if (name == want && !url.empty()) {
            r.zipName = name;
            r.zipUrl = url;
            const int sz = json_key(&j, n, "size");
            if (sz >= 0 && j.text[j.tokens[sz].start] >= '0' && j.text[j.tokens[sz].start] <= '9')
                r.zipSize = (uint64_t)json_number(&j, sz);
            const std::string d = strMember(j, n, "digest", 96); // "sha256:<hex>" on GitHub
            if (d.rfind("sha256:", 0) == 0 && isHex64(d.substr(7))) r.sha256 = lower(d.substr(7));
        } else if (name == "SHA256SUMS" && !url.empty()) {
            r.sumsUrl = url;
        }
    }
    if (r.zipUrl.empty() || r.zipUrl.rfind("https://", 0) != 0) return false;
    if (r.sha256.empty() && r.sumsUrl.empty()) return false; // nothing to verify against
    out = std::move(r);
    return true;
}

bool findChecksum(std::string_view sums, std::string_view name, std::string& hexOut) {
    size_t pos = 0;
    while (pos < sums.size()) {
        size_t eol = sums.find('\n', pos);
        std::string_view line = sums.substr(pos, eol == std::string_view::npos ? sums.size() - pos : eol - pos);
        pos = eol == std::string_view::npos ? sums.size() : eol + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() < 66) continue;
        std::string hex(line.substr(0, 64));
        if (!isHex64(hex)) continue;
        std::string_view rest = line.substr(64);
        size_t k = 0;
        while (k < rest.size() && rest[k] == ' ') ++k;
        if (k == 0) continue;
        if (k < rest.size() && rest[k] == '*') ++k; // binary-mode marker
        if (rest.substr(k) == name) { hexOut = lower(hex); return true; }
    }
    return false;
}

bool isSafeArchivePath(std::string_view p) {
    if (p.empty() || p.size() > 255 || p[0] == '/') return false;
    if (p.find('\\') != std::string_view::npos || p.find(':') != std::string_view::npos) return false;
    if (p.find('\0') != std::string_view::npos) return false;
    size_t i = 0;
    while (i <= p.size()) {
        size_t e = p.find('/', i);
        std::string_view seg = p.substr(i, e == std::string_view::npos ? std::string_view::npos : e - i);
        if (seg == "..") return false;
        if (e == std::string_view::npos) break;
        i = e + 1;
    }
    return true;
}

SwapJournal planSwap(const std::vector<std::string>& manifest) {
    std::vector<std::string> files;
    for (const auto& f : manifest)
        if (isSafeArchivePath(f) && f != "EBOOT.PBP") files.push_back(f);
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    bool hasEboot = std::find(manifest.begin(), manifest.end(), "EBOOT.PBP") != manifest.end();
    if (hasEboot) files.push_back("EBOOT.PBP"); // loader cannot replace itself: rename last
    SwapJournal j;
    for (const auto& f : files) j.steps.push_back({SwapOp::BackupOld, f, false});
    for (const auto& f : files) j.steps.push_back({SwapOp::InstallNew, f, false});
    for (const auto& f : files) j.steps.push_back({SwapOp::Cleanup, f, false});
    return j;
}

std::string serializeJournal(const SwapJournal& j) {
    static const char* names[] = {"BACKUP", "INSTALL", "CLEANUP"};
    std::string s = "WILI-SWAP 1\n";
    for (const auto& st : j.steps)
        s += std::string(st.done ? "D " : "P ") + names[(int)st.op] + " " + st.path + "\n";
    s += "END\n"; // a missing END marks a torn write
    return s;
}

bool parseJournal(std::string_view text, SwapJournal& out) {
    SwapJournal j;
    size_t pos = text.find('\n');
    if (pos == std::string_view::npos || text.substr(0, pos) != "WILI-SWAP 1") return false;
    ++pos;
    bool ended = false;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) return false;
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line == "END") { ended = pos == text.size(); break; }
        if (line.size() < 4 || line[1] != ' ' || (line[0] != 'D' && line[0] != 'P')) return false;
        const bool done = line[0] == 'D';
        line.remove_prefix(2);
        size_t sp = line.find(' ');
        if (sp == std::string_view::npos) return false;
        std::string_view op = line.substr(0, sp), path = line.substr(sp + 1);
        SwapStep st;
        if (op == "BACKUP") st.op = SwapOp::BackupOld;
        else if (op == "INSTALL") st.op = SwapOp::InstallNew;
        else if (op == "CLEANUP") st.op = SwapOp::Cleanup;
        else return false;
        if (!isSafeArchivePath(path)) return false;
        st.path = std::string(path);
        st.done = done;
        j.steps.push_back(std::move(st));
    }
    if (!ended) return false;
    out = std::move(j);
    return true;
}

size_t nextStep(const SwapJournal& j) {
    for (size_t i = 0; i < j.steps.size(); ++i)
        if (!j.steps[i].done) return i;
    return j.steps.size();
}
}
