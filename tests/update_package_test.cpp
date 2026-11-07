#include "../client/services/update_package.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
using namespace pspapp;
struct Mem : ByteSource {
    std::string d;
    uint64_t size() const override { return d.size(); }
    size_t read(uint64_t off, void* dst, size_t n) override {
        if (off >= d.size()) return 0;
        n = std::min<size_t>(n, d.size() - off);
        memcpy(dst, d.data() + off, n);
        return n;
    }
};
struct Collect : FileSink {
    std::map<std::string, std::string> files; std::string cur; bool failWrite = false; int discarded = 0;
    bool begin(const std::string& p, uint64_t) override { cur = p; files[p].clear(); return true; }
    bool write(const void* x, size_t n) override { if (failWrite) return false; files[cur].append((const char*)x, n); return true; }
    bool end(bool ok) override { if (!ok) { files.erase(cur); ++discarded; } return true; }
};
static std::string slurp(const char* p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
int main(int argc, char** argv) {
    assert(argc == 2); // path to python-generated fixture zip
    { Mem m; m.d = ""; assert(sha256Hex(m) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"); }
    { Mem m; m.d = "abc"; assert(sha256Hex(m) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"); }
    { Mem m; m.d = std::string(1000000, 'a'); assert(sha256Hex(m) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"); }

    Mem z; z.d = slurp(argv[1]);
    std::vector<ZipEntry> es;
    assert(listZip(z, es) == ZipError::Ok);
    Collect c;
    size_t files = 0;
    for (auto& e : es) { if (e.isDir) continue; ++files; assert(extractEntry(z, e, c) == ZipError::Ok); }
    assert(files == 3);
    assert(c.files["WiliMini/EBOOT.PBP"] == "eboot");
    assert(c.files["WiliMini/empty"].empty());
    assert(c.files["WiliMini/big.bin"].size() == 300000 && c.files["WiliMini/big.bin"][299999] == 'x');

    // Corrupt a data byte of the stored entry -> CRC error, partial file discarded.
    { Mem bad = z; size_t at = bad.d.find("eboot"); assert(at != std::string::npos); bad.d[at] ^= 1;
      std::vector<ZipEntry> e2; assert(listZip(bad, e2) == ZipError::Ok);
      Collect cc; bool sawBad = false;
      for (auto& e : e2) if (!e.isDir && extractEntry(bad, e, cc) == ZipError::BadCrc) sawBad = true;
      assert(sawBad && cc.discarded == 1 && !cc.files.count("WiliMini/EBOOT.PBP")); }
    // Truncation: end-of-central-directory gone, and every prefix is rejected or fails cleanly.
    for (size_t cut : {size_t(0), size_t(10), z.d.size() / 2, z.d.size() - 1}) {
        Mem t; t.d = z.d.substr(0, cut); std::vector<ZipEntry> e3;
        assert(listZip(t, e3) != ZipError::Ok);
    }
    // Sink failure surfaces as an error and discards.
    { Collect f; f.failWrite = true; for (auto& e : es) if (!e.isDir && e.size) { assert(extractEntry(z, e, f) == ZipError::Sink); break; } }
    // Limits.
    { ZipLimits l; l.maxFileSize = 100; std::vector<ZipEntry> e4; assert(listZip(z, e4, l) == ZipError::TooLarge); }
    { ZipLimits l; l.maxEntries = 1; std::vector<ZipEntry> e4; assert(listZip(z, e4, l) == ZipError::TooLarge); }
    // Garbage never crashes.
    srand(1);
    for (int i = 0; i < 300; ++i) {
        Mem g = z; for (int k = 0; k < 4; ++k) g.d[rand() % g.d.size()] = (char)rand();
        std::vector<ZipEntry> eg; Collect cg;
        if (listZip(g, eg) == ZipError::Ok) for (auto& e : eg) if (!e.isDir) extractEntry(g, e, cg);
    }
    // Traversal fixture.
    { Mem t; t.d = slurp((std::string(argv[1]) + ".evil").c_str()); std::vector<ZipEntry> e5; assert(listZip(t, e5) == ZipError::BadPath); }

    assert(rangeHeader(0) == "bytes=0-" && rangeHeader(1234) == "bytes=1234-");
    assert(resumeAction(206, "bytes 100-999/1000", 100, 1000) == ResumeAction::Append);
    assert(resumeAction(206, "bytes 100-999/1000", 100, 0) == ResumeAction::Append);
    assert(resumeAction(206, "bytes 50-999/1000", 100, 1000) == ResumeAction::Restart); // wrong offset
    assert(resumeAction(206, "bytes 100-999/2000", 100, 1000) == ResumeAction::Restart); // changed asset
    assert(resumeAction(206, "", 100, 1000) == ResumeAction::Restart);
    assert(resumeAction(206, "bytes 100-9/1000", 100, 1000) == ResumeAction::Restart);
    assert(resumeAction(200, "", 100, 1000) == ResumeAction::Restart);
    assert(resumeAction(416, "", 100, 1000) == ResumeAction::Restart);
    assert(resumeAction(404, "", 0, 0) == ResumeAction::Fail && resumeAction(503, "", 0, 0) == ResumeAction::Fail);
    std::cout << "update_package: PASS\n";
}
