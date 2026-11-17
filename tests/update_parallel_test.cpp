// Multi-connection download (BILI_UPDATE_CONNECTIONS): segment layout, resume, fallbacks.
// Built by tools/test-update-package.sh once per connection count.
#include "../client/services/update_install.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <random>
#include <thread>
#include <unistd.h>
using namespace pspapp;

static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static bool exists(const std::string& p) { return access(p.c_str(), F_OK) == 0; }
static void spit(const std::string& p, const std::string& d) { std::ofstream(p, std::ios::binary) << d; }

struct Server {
    std::string zip;
    bool ignoreRange = false;
    uint64_t cutFrom = UINT64_MAX; size_t cutAfter = 0; // drop the connection of the request starting at cutFrom
    std::mutex m;
    std::vector<std::pair<uint64_t, uint64_t>> ranges; // requested [from, to]
};
struct Client : UpdateTransport {
    Server* s;
    explicit Client(Server* srv) : s(srv) {}
    bool get(const std::string& url, uint64_t from, const std::function<bool(int, const std::string&)>& h,
             const std::function<bool(const void*, size_t)>& d) override { return getRange(url, from, UINT64_MAX, h, d); }
    bool getRange(const std::string&, uint64_t from, uint64_t to, const std::function<bool(int, const std::string&)>& h,
                  const std::function<bool(const void*, size_t)>& d) override {
        uint64_t size = s->zip.size();
        uint64_t end = std::min<uint64_t>(to, size - 1);
        bool cut = false;
        {
            std::lock_guard<std::mutex> l(s->m);
            s->ranges.push_back({from, end});
            if (from == s->cutFrom) { cut = true; s->cutFrom = UINT64_MAX; }
        }
        uint64_t start = s->ignoreRange ? 0 : from;
        if (s->ignoreRange || (from == 0 && to == UINT64_MAX)) { if (!h(200, "")) return false; end = size - 1; } // not a Range request
        else if (!h(206, "bytes " + std::to_string(from) + "-" + std::to_string(end) + "/" + std::to_string(size))) return false;
        uint64_t sent = 0;
        for (uint64_t o = start; o <= end; o += 700) {
            size_t n = (size_t)std::min<uint64_t>(700, end - o + 1);
            if (cut && sent + n > s->cutAfter) { n = s->cutAfter - sent; if (n) d(s->zip.data() + o, n); return false; }
            if (!d(s->zip.data() + o, n)) return false;
            sent += n;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        return true;
    }
};
// Only get(): exercises the default getRange().
struct PlainClient : UpdateTransport {
    std::string zip;
    bool get(const std::string&, uint64_t from, const std::function<bool(int, const std::string&)>& h,
             const std::function<bool(const void*, size_t)>& d) override {
        if (!h(206, "bytes " + std::to_string(from) + "-" + std::to_string(zip.size() - 1) + "/" + std::to_string(zip.size()))) return false;
        for (uint64_t o = from; o < zip.size(); o += 300)
            if (!d(zip.data() + o, (size_t)std::min<uint64_t>(300, zip.size() - o))) return false;
        return true;
    }
};

static DownloadOptions makeOptions(Server* srv, uint64_t minSegment) {
    DownloadOptions o;
    o.makeTransport = [srv] { return std::unique_ptr<UpdateTransport>(new Client(srv)); };
    o.runParallel = [](std::vector<std::function<void()>>& tasks) {
        std::vector<std::thread> th;
        for (size_t i = 1; i < tasks.size(); ++i) th.emplace_back(tasks[i]);
        tasks[0]();
        for (auto& t : th) t.join();
    };
    o.minSegment = minSegment;
    return o;
}
static std::string segFile(const std::string& part, uint64_t size, unsigned n, unsigned i) {
    uint64_t seg = (size + n - 1) / n;
    unsigned eff = (unsigned)((size + seg - 1) / seg);
    return part + "." + std::to_string(eff) + "." + std::to_string(i);
}
static unsigned noSegmentsLeft(const std::string& part) {
    unsigned left = 0;
    for (unsigned n = 2; n <= 8; ++n) for (unsigned i = 1; i < n; ++i) left += exists(part + "." + std::to_string(n) + "." + std::to_string(i));
    return left;
}

int main() {
    const unsigned N = BILI_UPDATE_CONNECTIONS;
    char dirT[] = "/tmp/wili-par-XXXXXX"; assert(mkdtemp(dirT));
    std::string dir = dirT, part = dir + "/u.part", zip = dir + "/u.zip";
    std::mt19937 rng(7);
    Server srv; for (int i = 0; i < 10007; ++i) srv.zip.push_back((char)rng()); // not divisible by 2..8
    const uint64_t size = srv.zip.size();
    Sha256 h; h.update(srv.zip.data(), srv.zip.size());
    ReleaseInfo info; info.zipUrl = "z"; info.zipSize = size; info.sha256 = h.finishHex();
    Client base(&srv);
    DownloadOptions opt = makeOptions(&srv, 1000);
    opt.connections = N;
    auto reset = [&] { std::remove(part.c_str()); std::remove(zip.c_str()); for (unsigned n = 2; n <= 8; ++n) for (unsigned i = 1; i < n; ++i) std::remove((part + "." + std::to_string(n) + "." + std::to_string(i)).c_str()); srv.ranges.clear(); srv.ignoreRange = false; srv.cutFrom = UINT64_MAX; };
    auto coverage = [&] { // the requested ranges must be disjoint and cover [0, size) exactly
        auto r = srv.ranges; std::sort(r.begin(), r.end());
        uint64_t next = 0;
        for (auto& x : r) { assert(x.first == next); next = x.second + 1; }
        assert(next == size);
    };
    // Default getRange() on a transport that only has get().
    { PlainClient p; p.zip = srv.zip; std::string got;
      p.getRange("u", 100, 399, [](int, const std::string&) { return true; }, [&](const void* d, size_t n) { got.append((const char*)d, n); return true; });
      assert(got == srv.zip.substr(100, 300)); }

    // 1. Fresh download: N disjoint ranges, progress ends at the full size, nothing left over.
    { reset(); std::vector<uint64_t> prog;
      assert(downloadRelease(base, info, part, zip, [&](const UpdateProgress& p) { prog.push_back(p.done); return true; }, &opt) == UpdateResult::Ok);
      assert(slurp(zip) == srv.zip && !exists(part) && noSegmentsLeft(part) == 0);
      assert(srv.ranges.size() == N); coverage();
      assert(!prog.empty() && prog.back() == size && std::is_sorted(prog.begin(), prog.end())); }
    // 2. Segments never smaller than minSegment: 10007 / 4000 -> at most 2 connections.
    { reset(); DownloadOptions o2 = opt; o2.minSegment = 4000;
      assert(downloadRelease(base, info, part, zip, nullptr, &o2) == UpdateResult::Ok && slurp(zip) == srv.zip);
      assert(srv.ranges.size() == std::min<size_t>(N, 2)); coverage(); }
    // 3. Unknown size or no runner: single connection, one open-ended request.
    { reset(); ReleaseInfo noSize = info; noSize.zipSize = 0;
      assert(downloadRelease(base, noSize, part, zip, nullptr, &opt) == UpdateResult::Ok && srv.ranges.size() == 1);
      reset(); DownloadOptions none; assert(downloadRelease(base, info, part, zip, nullptr, &none) == UpdateResult::Ok && srv.ranges.size() == 1); }

    if (N >= 2) {
        const uint64_t seg = (size + N - 1) / N;
        // 4. One connection drops: the others finish, the partial segment resumes alone.
        { reset(); const size_t cut = (size_t)((size - seg * (N - 1)) / 2); // half of the last segment
          srv.cutFrom = seg * (N - 1); srv.cutAfter = cut;
          assert(downloadRelease(base, info, part, zip, nullptr, &opt) == UpdateResult::Network);
          assert(!exists(zip) && exists(part) && slurp(segFile(part, size, N, N - 1)).size() == cut);
          srv.ranges.clear();
          assert(downloadRelease(base, info, part, zip, nullptr, &opt) == UpdateResult::Ok && slurp(zip) == srv.zip);
          assert(srv.ranges.size() == 1 && srv.ranges[0].first == seg * (N - 1) + cut && noSegmentsLeft(part) == 0); }
        // 5. Every segment partial (as after a cancel): each resumes from its own length.
        { reset(); spit(part, srv.zip.substr(0, 1200));
          for (unsigned i = 1; i < N; ++i) spit(segFile(part, size, N, i), srv.zip.substr(i * seg, 400 + 10 * i));
          assert(downloadRelease(base, info, part, zip, nullptr, &opt) == UpdateResult::Ok && slurp(zip) == srv.zip);
          assert(srv.ranges.size() == N);
          std::sort(srv.ranges.begin(), srv.ranges.end());
          assert(srv.ranges[0].first == 1200);
          for (unsigned i = 1; i < N; ++i) assert(srv.ranges[i].first == i * seg + 400 + 10 * i); }
        // 6. A completed segment is not requested again.
        { reset(); spit(part, srv.zip.substr(0, seg));
          assert(downloadRelease(base, info, part, zip, nullptr, &opt) == UpdateResult::Ok && slurp(zip) == srv.zip);
          assert(srv.ranges.size() == N - 1 && std::none_of(srv.ranges.begin(), srv.ranges.end(), [&](auto& r) { return r.first < seg; })); }
        // 7. The server ignores Range: fall back to one plain download, no leftovers.
        { reset(); srv.ignoreRange = true;
          assert(downloadRelease(base, info, part, zip, nullptr, &opt) == UpdateResult::Ok && slurp(zip) == srv.zip);
          assert(noSegmentsLeft(part) == 0); }
        // 8. Cancel stops every connection and keeps the data; the next run completes.
        { reset();
          assert(downloadRelease(base, info, part, zip, [](const UpdateProgress& p) { return p.done < 3000; }, &opt) == UpdateResult::Cancelled);
          assert(!exists(zip)); srv.ranges.clear();
          assert(downloadRelease(base, info, part, zip, nullptr, &opt) == UpdateResult::Ok && slurp(zip) == srv.zip); }
        // 9. A prefix longer than segment 0 (single-connection resume, interrupted merge) is continued alone.
        { reset(); size_t have = (size_t)(seg + seg / 2); spit(part, srv.zip.substr(0, have));
          spit(segFile(part, size, N, 1), srv.zip.substr(seg, 300)); // stale: already inside the prefix
          assert(downloadRelease(base, info, part, zip, nullptr, &opt) == UpdateResult::Ok && slurp(zip) == srv.zip);
          assert(srv.ranges.size() == 1 && srv.ranges[0].first == have && noSegmentsLeft(part) == 0); }
        // 10. Segment files of a different connection count are removed.
        { reset(); unsigned other = N == 2 ? 3 : 2; spit(part + "." + std::to_string(other) + ".1", "stale");
          assert(downloadRelease(base, info, part, zip, nullptr, &opt) == UpdateResult::Ok && noSegmentsLeft(part) == 0); }
        // 11. Checksum mismatch deletes everything.
        { reset(); Server bad; bad.zip = srv.zip; bad.zip[5000] ^= 1;
          DownloadOptions ob = makeOptions(&bad, 1000); ob.connections = N; Client bc(&bad);
          assert(downloadRelease(bc, info, part, zip, nullptr, &ob) == UpdateResult::ChecksumMismatch);
          assert(!exists(part) && !exists(zip) && noSegmentsLeft(part) == 0); }
        // 12. A segment longer than its range (misbehaving server) is an error, not a corrupt file.
        { reset(); ReleaseInfo big = info; big.zipSize = size - 100; // server file is longer than advertised
          assert(downloadRelease(base, big, part, zip, nullptr, &opt) != UpdateResult::Ok); assert(!exists(zip)); }
    }
    std::string cmd = "rm -rf '" + dir + "'"; assert(system(cmd.c_str()) == 0);
    std::cout << "update_parallel (N=" << N << "): PASS\n";
}
