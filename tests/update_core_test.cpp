#include "../client/services/update_core.hpp"
#include <cassert>
#include <iostream>
using namespace pspapp;
int main() {
    SemVer v;
    assert(parseSemVer("v1.2.3", v) && v.major == 1 && v.minor == 2 && v.patch == 3 && !v.prerelease);
    assert(parseSemVer("1.2", v) && v.patch == 0);
    assert(parseSemVer("1.2.3-4-gabc", v) && v.prerelease);
    assert(!parseSemVer("", v) && !parseSemVer("abc", v) && !parseSemVer("1..2", v) && !parseSemVer("1.2.3x", v));
    assert(isUpdateAvailable("v1.2.3", "v1.2.4"));
    assert(isUpdateAvailable("1.9.0", "v1.10.0")); // numeric, not lexical
    assert(!isUpdateAvailable("v1.2.3", "v1.2.3"));
    assert(!isUpdateAvailable("v2.0.0", "v1.9.9")); // never downgrade
    assert(!isUpdateAvailable("1.0.0", "v1.1.0-rc1")); // no prereleases
    assert(isUpdateAvailable("1.1.0-rc1", "v1.1.0"));
    assert(isUpdateAvailable("deadbeef", "v0.1.0"));

    const std::string hex(64, 'A');
    std::string good = R"({"tag_name":"v1.2.0","body":"notes","draft":false,"prerelease":false,"assets":[
      {"name":"SHA256SUMS","browser_download_url":"https://x/SHA256SUMS"},
      {"name":"WiliMini-v1.2.0.zip","size":123,"browser_download_url":"https://x/z.zip","digest":"sha256:)" + hex + R"("}]})";
    ReleaseInfo r;
    assert(parseRelease(good, r) && r.tag == "v1.2.0" && r.zipSize == 123 && r.sha256 == std::string(64, 'a'));
    assert(r.sumsUrl == "https://x/SHA256SUMS" && r.notes == "notes");
    assert(!parseRelease("garbage", r) && !parseRelease("[]", r) && !parseRelease("{}", r));
    assert(!parseRelease(R"({"tag_name":"v1.2.0","prerelease":true,"assets":[]})", r));
    assert(!parseRelease(R"({"tag_name":"v1.2.0","draft":true,"assets":[]})", r));
    assert(!parseRelease(R"({"tag_name":"v1.2.0","assets":[{"name":"WiliMini-v1.2.0.zip","browser_download_url":"http://x/z"}]})", r)); // http
    assert(!parseRelease(R"({"tag_name":"v1.2.0","assets":[{"name":"WiliMini-v1.2.0.zip","browser_download_url":"https://x/z"}]})", r)); // no checksum
    assert(!parseRelease(R"({"tag_name":"v1.2.0","assets":[{"name":"other.zip","browser_download_url":"https://x/z"}]})", r));
    assert(!parseRelease(R"({"tag_name":"v1.2.0","assets":null})", r));
    assert(!parseRelease(std::string(kMaxReleaseJson + 1, ' '), r));
    std::string big = R"({"tag_name":"v1.2.0","body":")" + std::string(100000, 'x') +
        R"(","assets":[{"name":"WiliMini-v1.2.0.zip","browser_download_url":"https://x/z","digest":"sha256:)" + hex + R"("}]})";
    assert(parseRelease(big, r) && r.notes.size() == kMaxReleaseNotes);

    std::string sums = hex + "  WiliMini-v1.2.0.zip\r\n" + std::string(64, 'b') + " *other.zip\n";
    std::string h;
    assert(findChecksum(sums, "WiliMini-v1.2.0.zip", h) && h == std::string(64, 'a'));
    assert(findChecksum(sums, "other.zip", h) && h == std::string(64, 'b'));
    assert(!findChecksum(sums, "missing.zip", h) && !findChecksum("", "x", h));

    assert(isSafeArchivePath("WiliMini/a/b.bin"));
    assert(!isSafeArchivePath("../x") && !isSafeArchivePath("a/../x") && !isSafeArchivePath("/abs"));
    assert(!isSafeArchivePath("a\\b") && !isSafeArchivePath("C:x") && !isSafeArchivePath(""));

    SwapJournal j = planSwap({"EBOOT.PBP", "wili_mini.prx", "../evil", "a/b", "a/b"});
    assert(j.steps.size() == 9); // 3 unique files x 3 phases... minus evil
    assert(j.steps[1].path == "wili_mini.prx" && j.steps[2].path == "EBOOT.PBP"); // EBOOT last
    assert(j.steps[0].op == SwapOp::BackupOld && j.steps[8].op == SwapOp::Cleanup);
    assert(nextStep(j) == 0);
    // Crash injected after every step: serialize, reload, resume.
    for (size_t k = 0; k <= j.steps.size(); ++k) {
        SwapJournal c = j;
        for (size_t i = 0; i < k; ++i) c.steps[i].done = true;
        SwapJournal back;
        assert(parseJournal(serializeJournal(c), back));
        assert(nextStep(back) == k && back.steps.size() == j.steps.size());
        assert(back.steps[0].path == c.steps[0].path);
    }
    std::string text = serializeJournal(j);
    SwapJournal b;
    assert(!parseJournal(text.substr(0, text.size() - 4), b)); // torn write
    assert(!parseJournal("bad", b) && !parseJournal("WILI-SWAP 1\nP BACKUP ../x\nEND\n", b));
    std::cout << "update_core: PASS\n";
}
