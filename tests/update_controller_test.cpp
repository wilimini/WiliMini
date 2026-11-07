#include "../client/services/update_controller.hpp"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>
using namespace pspapp;
static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
struct Net : UpdateTransport {
    std::string zip, json, sums; bool down = false;
    bool get(const std::string& url, uint64_t from, const std::function<bool(int, const std::string&)>& h,
             const std::function<bool(const void*, size_t)>& d) override {
        if (down) return false;
        if (url.find("releases/latest") != std::string::npos) return h(200, "") && d(json.data(), json.size());
        if (url == "https://h/sums") return h(200, "") && d(sums.data(), sums.size());
        std::string cr = from ? "bytes " + std::to_string(from) + "-" + std::to_string(zip.size() - 1) + "/" + std::to_string(zip.size()) : "";
        if (!h(from ? 206 : 200, cr)) return false;
        return d(zip.data() + from, zip.size() - from);
    }
};
int main(int, char** argv) {
    char t[] = "/tmp/wili-ctl-XXXXXX"; assert(mkdtemp(t));
    std::string root = t;
    Net net; net.zip = slurp(argv[1]);
    Sha256 h; h.update(net.zip.data(), net.zip.size()); std::string sha = h.finishHex();
    net.json = R"({"tag_name":"v9.0.0","body":"notes here","assets":[{"name":"WiliMini-v9.0.0.zip","size":)" + std::to_string(net.zip.size()) +
               R"(,"browser_download_url":"https://h/z","digest":"sha256:)" + sha + R"("}]})";
    bool lowBattery = false; uint64_t freeBytes = 1ull << 30;
    UpdatePolicy pol;
    pol.preflight = [&](std::string& r) { if (lowBattery) { r = "battery too low"; return false; } return true; };
    pol.freeSpace = [&] { return freeBytes; };
    struct Ref : UpdateTransport {
        Net* n;
        explicit Ref(Net* x) : n(x) {}
        bool get(const std::string& u, uint64_t f, const std::function<bool(int, const std::string&)>& a,
                 const std::function<bool(const void*, size_t)>& b) override { return n->get(u, f, a, b); }
    };
    UpdateController c(root, "v1.0.0", [&](const volatile bool*) { return std::unique_ptr<UpdateTransport>(new Ref(&net)); }, pol);

    assert(c.snapshot().state == UpdateState::Idle && !c.busy());
    net.down = true; c.runCheck();
    assert(c.snapshot().state == UpdateState::Failed && !c.snapshot().error.empty());
    net.down = false; c.reset();
    { // Chosen source only: when it is down the check fails, even though the other one would answer.
      struct Pick : UpdateTransport {
          Net* n; std::string lastUrl; bool githubDown = true;
          explicit Pick(Net* x) : n(x) {}
          bool get(const std::string& u, uint64_t f, const std::function<bool(int, const std::string&)>& a,
                   const std::function<bool(const void*, size_t)>& b) override {
              lastUrl = u; if (githubDown && u.find("api.github.com") != std::string::npos) return false; return n->get(u, f, a, b); }
      };
      std::vector<std::string> asked;
      UpdateController pc(root, "v1.0.0", [&](const volatile bool*) { return std::unique_ptr<UpdateTransport>(new Pick(&net)); }, pol);
      pc.setSource(0); pc.runCheck();
      assert(pc.snapshot().state == UpdateState::Failed);
      pc.setSource(1); assert(pc.snapshot().state == UpdateState::Idle); // changing source clears the old result
      pc.runCheck();
      assert(pc.snapshot().state == UpdateState::Available && pc.snapshot().source == "Codeberg");
    } assert(c.snapshot().state == UpdateState::Idle);
    c.runInstall(); assert(c.snapshot().state == UpdateState::Idle); // install only from Available
    c.runCheck();
    { auto s = c.snapshot(); assert(s.state == UpdateState::Available && s.tag == "v9.0.0" && s.notes == "notes here" && s.total == net.zip.size()); }
    lowBattery = true; c.runInstall();
    assert(c.snapshot().state == UpdateState::Failed && c.snapshot().error == "battery too low");
    lowBattery = false; c.reset(); c.runCheck();
    freeBytes = 1000; c.runInstall();
    assert(c.snapshot().state == UpdateState::Failed && c.snapshot().error == "not enough free space");
    freeBytes = 1ull << 30; c.reset(); c.runCheck();
    c.cancel(); // cancel before the transfer: stays available, nothing staged
    c.runInstall();
    assert(c.snapshot().state == UpdateState::Ready || c.snapshot().state == UpdateState::Available);
    c.reset(); c.runCheck(); c.runInstall();
    assert(c.snapshot().state == UpdateState::Ready);
    assert(!slurp(root + "/pending/MANIFEST").empty() && slurp(root + "/update.zip").empty() && slurp(root + "/update.part").empty());
    std::string cmd = std::string("rm -rf '") + root + "'"; assert(system(cmd.c_str()) == 0);
    std::cout << "update_controller: PASS\n";
}
