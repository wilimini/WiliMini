#include "../client/services/update_install.hpp"
#include "wili_update_swap_posix.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unistd.h>
using namespace pspapp;
static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }

struct Fake : UpdateTransport {
    std::string zip, json, sums;
    int zipStatus = 200; bool ignoreRange = false; size_t cutAfter = 0; // cut: connection drops after N body bytes
    int zipRequests = 0; uint64_t lastRange = 0;
    bool get(const std::string& url, uint64_t from,
             const std::function<bool(int, const std::string&)>& hdr,
             const std::function<bool(const void*, size_t)>& data) override {
        if (url == "api") { if (!hdr(200, "")) return false; return data(json.data(), json.size()); }
        if (url == "sums") { if (!hdr(200, "")) return false; return data(sums.data(), sums.size()); }
        ++zipRequests; lastRange = from;
        uint64_t start = ignoreRange ? 0 : from;
        std::string cr = (from && !ignoreRange) ? "bytes " + std::to_string(start) + "-" + std::to_string(zip.size() - 1) + "/" + std::to_string(zip.size()) : "";
        if (!hdr(from && !ignoreRange ? 206 : zipStatus, cr)) return false;
        size_t n = zip.size() - start;
        if (cutAfter && cutAfter < n) { data(zip.data() + start, cutAfter); return false; }
        for (size_t o = 0; o < n; o += 1000) if (!data(zip.data() + start + o, std::min<size_t>(1000, n - o))) return false;
        return true;
    }
};

int main(int argc, char** argv) {
    assert(argc == 2);
    char dirT[] = "/tmp/wili-inst-XXXXXX"; assert(mkdtemp(dirT));
    std::string dir = dirT, part = dir + "/u.part", zip = dir + "/u.zip";
    Fake f; f.zip = slurp(argv[1]);
    Sha256 h; h.update(f.zip.data(), f.zip.size()); std::string sha = h.finishHex();
    f.json = R"({"tag_name":"v2.0.0","body":"n","assets":[{"name":"WiliMini-v2.0.0.zip","size":)" + std::to_string(f.zip.size()) +
             R"(,"browser_download_url":"https://h/z","digest":"sha256:)" + sha + R"("}]})";
    ReleaseInfo info;
    assert(checkForUpdate(f, "api", "v1.0.0", info) == UpdateResult::Ok && info.sha256 == sha);
    assert(checkForUpdate(f, "api", "v2.0.0", info) == UpdateResult::NoUpdate);
    assert(checkForUpdate(f, "api", "v3.0.0", info) == UpdateResult::NoUpdate); // no downgrade
    f.json = "{nope"; assert(checkForUpdate(f, "api", "v1.0.0", info) == UpdateResult::BadRelease);
    f.json = std::string(kMaxReleaseJson + 10, 'x'); assert(checkForUpdate(f, "api", "v1.0.0", info) == UpdateResult::BadRelease);
    f.json = R"({"tag_name":"v2.0.0","assets":[{"name":"WiliMini-v2.0.0.zip","size":)" + std::to_string(f.zip.size()) +
             R"(,"browser_download_url":"https://h/z","digest":"sha256:)" + sha + R"("}]})";
    assert(checkForUpdate(f, "api", "v1.0.0", info) == UpdateResult::Ok);
    info.zipUrl = "z";
    assert(std::string(kUpdateSources[0].apiUrl).rfind("https://", 0) == 0 && std::string(kUpdateSources[1].apiUrl).rfind("https://", 0) == 0);
    // Connection drops mid-download: .part kept; next try resumes with a Range request.
    f.cutAfter = 3000; assert(f.zip.size() > 10000);
    { auto r0 = downloadRelease(f, info, part, zip, nullptr); if (r0 != UpdateResult::Network) std::cerr << updateResultName(r0) << "\n"; assert(r0 == UpdateResult::Network); }
    assert(slurp(part).size() == 3000);
    f.cutAfter = 0; f.zipRequests = 0;
    assert(downloadRelease(f, info, part, zip, nullptr) == UpdateResult::Ok);
    assert(f.lastRange == 3000 && slurp(zip) == f.zip);
    // Server ignores Range (200): start over cleanly.
    std::remove(zip.c_str());
    { std::ofstream o(part, std::ios::binary); o << f.zip.substr(0, 2000); }
    f.ignoreRange = true;
    assert(downloadRelease(f, info, part, zip, nullptr) == UpdateResult::Ok && slurp(zip) == f.zip);
    f.ignoreRange = false;
    // Cancel keeps the partial file.
    std::remove(zip.c_str());
    assert(downloadRelease(f, info, part, zip, [](const UpdateProgress& p) { return p.done < 2500; }) == UpdateResult::Cancelled);
    assert(slurp(part).size() >= 2500);
    std::remove(part.c_str());
    // Corrupt download: checksum mismatch deletes the .part.
    { Fake bad = f; bad.zip[100] ^= 1; assert(downloadRelease(bad, info, part, zip, nullptr) == UpdateResult::ChecksumMismatch);
      assert(slurp(part).empty() && slurp(zip).empty()); }
    // Server sends more than advertised.
    { ReleaseInfo small = info; small.zipSize = 500; assert(downloadRelease(f, small, part, zip, nullptr) != UpdateResult::Ok); std::remove(part.c_str()); }
    // Checksum from SHA256SUMS instead of the API digest.
    { ReleaseInfo s2 = info; s2.sha256.clear(); s2.sumsUrl = "sums"; s2.zipName = "WiliMini-v2.0.0.zip";
      f.sums = sha + "  WiliMini-v2.0.0.zip\n";
      assert(downloadRelease(f, s2, part, zip, nullptr) == UpdateResult::Ok);
      std::remove(zip.c_str());
      f.sums = std::string(64, '0') + "  WiliMini-v2.0.0.zip\n";
      assert(downloadRelease(f, s2, part, zip, nullptr) == UpdateResult::ChecksumMismatch);
      f.sums = "garbage"; assert(downloadRelease(f, s2, part, zip, nullptr) == UpdateResult::BadRelease); }

    // Stage the real zip, then let the loader swap it in.
    std::string root = dir + "/root";
    mkdir(root.c_str(), 0777);
    { std::ofstream(root + "/EBOOT.PBP") << "old"; std::ofstream(root + "/wili_mini.prx") << "old"; std::ofstream(root + "/settings.json") << "user"; }
    assert(downloadRelease(f, info, part, zip, nullptr) == UpdateResult::Ok);
    assert(stageRelease(zip, std::string(64, '1'), root) == UpdateResult::ChecksumMismatch);
    assert(stageRelease(zip, sha, root) == UpdateResult::Ok);
    assert(slurp(root + "/pending/MANIFEST").find("EBOOT.PBP") != std::string::npos);
    assert(wili_swap_run(&wili_swap_posix_fs, root.c_str()) == WILI_SWAP_APPLIED);
    assert(slurp(root + "/EBOOT.PBP") == "new-eboot" && slurp(root + "/wili_mini.prx") == "new-prx");
    assert(slurp(root + "/settings.json") == "user" && slurp(root + "/EBOOT.PBP.bak") == "old");
    assert(wili_swap_commit(&wili_swap_posix_fs, root.c_str()) == 0);
    std::string cmd = "rm -rf '" + dir + "'"; assert(system(cmd.c_str()) == 0);
    std::cout << "update_install: PASS\n";
}
