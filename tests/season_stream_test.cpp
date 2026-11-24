#include "../client/bilibili.h"
#include "../client/services/community_service.hpp"
#include "../client/services/stream_json.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

/* Stub C symbols normally provided by bilibili.c / app_error.cpp for host tests. */
extern "C" {
volatile int transfer_cancel = 0;
int client_storage_index = 2;
int network_init(void) { return 0; }
int network_connect(void) { return 0; }
int network_ready(void) { return 1; }
int bili_http_credential_host(const char *) { return 1; }
int bili_http_get_response_spill(const char *, BiliHttpResponse *, size_t, size_t, volatile int *) { return -1; }
int bili_http_post_response(const char *, const char *, BiliHttpResponse *, size_t) { return -1; }
void bili_http_free(void *p) { std::free(p); }
}

namespace pspapp {
std::string errorMessage(const AppError &) { return "error"; }
bool mayRetry(const AppError &) { return false; }
} // namespace pspapp

static bool fileExists(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

static std::string writeSpillFile(const std::string &dir, const std::string &name,
                                  const std::string &content) {
    std::string path = dir + "/" + name;
    FILE *f = std::fopen(path.c_str(), "wb");
    assert(f != nullptr);
    assert(std::fwrite(content.data(), 1, content.size(), f) == content.size());
    assert(std::fclose(f) == 0);
    return path;
}

/* Build a Detective Conan-scale season JSON where `episodes` appears BEFORE
 * `evaluate`, `new_ep`, `section`, `styles`, `title`, and `user_status`
 * (matching the real key order of /pgc/view/pc/season?season_id=33378). */
static std::string makeSeasonJson(int episodeCount, uint64_t lastEpId, int lastTime) {
    std::string out;
    out.reserve(static_cast<size_t>(episodeCount) * 1150 + 4096);
    out += "{\"code\":0,\"message\":\"success\",\"result\":{";
    out += "\"activity\":{\"id\":1,\"title\":\"ignored\"},";
    out += "\"episodes\":[";
    for (int i = 1; i <= episodeCount; ++i) {
        if (i > 1) out += ",";
        const uint64_t epId = 320000ULL + static_cast<uint64_t>(i);
        const uint64_t aid = 710000000ULL + static_cast<uint64_t>(i);
        const uint64_t cid = 26800000000ULL + static_cast<uint64_t>(i);
        out += "{\"aid\":" + std::to_string(aid) +
               ",\"badge\":\"" + (i % 10 == 0 ? "会员" : "") + "\"" +
               ",\"badge_info\":{\"bg_color\":\"#999999\",\"text\":\"ignored\"}" +
               ",\"bvid\":\"BV1PQ4y1N" + std::to_string(1000 + (i % 9000)) + "\"" +
               ",\"cid\":" + std::to_string(cid) +
               ",\"cover\":\"http://i0.hdslb.com/bfs/archive/82d4523e2562748d050a8d8ec7ebc03fbe1a15a1.jpg\"" +
               ",\"dimension\":{\"height\":1080,\"rotate\":0,\"width\":1440}" +
               ",\"duration\":1493033" +
               ",\"ep_id\":" + std::to_string(epId) +
               ",\"from\":\"bangumi\"" +
               ",\"id\":" + std::to_string(epId) +
               ",\"link\":\"https://www.bilibili.com/bangumi/play/ep" + std::to_string(epId) + "\"" +
               ",\"long_title\":\"第" + std::to_string(i) + "话 案件 \\\"真相\\\"\"" +
               ",\"rights\":{\"allow_dm\":1,\"area_limit\":0,\"pay\":0}" +
               ",\"share_copy\":\"《名侦探柯南》第" + std::to_string(i) + "话\"" +
               ",\"share_url\":\"https://www.bilibili.com/bangumi/play/ep" + std::to_string(epId) + "\"" +
               ",\"skip\":{\"ed\":{\"end\":1490,\"start\":1410},\"op\":{\"end\":120,\"start\":30}}" +
               ",\"subtitle\":\"已观看 100万次\"" +
               ",\"title\":\"" + std::to_string(i) + "\"" +
               ",\"vid\":\"\"}";
    }
    out += "],";
    out += "\"evaluate\":\"主角工藤新一变小成江户川柯南。\",";
    out += "\"new_ep\":{\"desc\":\"每周六19:30更新\",\"id\":8014873},";
    out += "\"seasons\":[{\"season_id\":1,\"title\":\"ignored\"}],";
    out += "\"section\":[{\"id\":99,\"title\":\"PV\",\"episodes\":[{"
           "\"aid\":999,\"badge\":\"预告\",\"bvid\":\"BV1pv0000001\",\"cid\":888,"
           "\"cover\":\"http://i0.hdslb.com/bfs/archive/pv.jpg\",\"duration\":90000,"
           "\"id\":999999,\"long_title\":\"剧场版预告\",\"title\":\"PV1\"}]}],";
    out += "\"styles\":[\"漫画改\",\"推理\",\"悬疑\"],";
    out += "\"title\":\"名侦探柯南\",";
    out += "\"user_status\":{\"follow\":0,\"last_ep_id\":" + std::to_string(lastEpId) +
           ",\"last_time\":" + std::to_string(lastTime) + "}";
    out += "}}";
    return out;
}

int main() {
    using namespace pspapp;

    char dirBuf[] = "/tmp/wilimini_season_stream_XXXXXX";
    assert(mkdtemp(dirBuf) != nullptr);
    const std::string dir = dirBuf;

    /* 1. Large 1500-episode (~1.6 MiB) Conan-scale season: HTTP spills to temp file,
     *    Pull SAX parses only the requested 60-episode window into RAM, temp file auto-deleted. */
    {
        const std::string bigJson = makeSeasonJson(1500, 320500, 612);
        assert(bigJson.size() > 1024 * 1024);

        /* Window 1 (page=1): episodes 0..59. */
        {
            const std::string spill = writeSpillFile(dir, "conan_p1.tmp", bigJson);
            assert(fileExists(spill));
            HttpClient http([&](const std::string &, ErrorOperation) {
                return HttpResponse::fromSpillFile(spill, bigJson.size(), true);
            });
            BilibiliApi api(http);
            CommunityService service(api);
            auto res = service.load({BrowseKind::Season, "33378", {}, {}, {}, 1}, Session{});
            assert(res);
            assert(!fileExists(spill));
            const BrowsePage &page = res.value;
            assert(page.title == "名侦探柯南");
            assert(page.description == "主角工藤新一变小成江户川柯南。");
            assert(page.subtitle == "每周六19:30更新 · 漫画改 · 推理 · 悬疑");
            assert(page.episodeOffset == 0);
            assert(page.totalEpisodes == 1501);
            assert(page.items.size() == 1 + kSeasonWindowSize);
            assert(page.items[0].next.kind == BrowseKind::Text);
            assert(page.items[1].next.kind == BrowseKind::Episode);
            assert(page.items[1].title == "01 第1话 案件 \"真相\"");
            assert(page.items[1].aid == 710000001ULL);
            assert(page.items[1].cid == 26800000001ULL);
            assert(page.items[1].epId == 320001ULL);
            assert(page.items[1].seasonId == 33378ULL);
            assert(page.items[1].duration == 1493);
            assert(page.items[10].title == "10 第10话 案件 \"真相\"");
            assert(page.items[10].subtitle == "会员");
            assert(page.items[60].epId == 320060ULL);
        }

        /* Window 9 (page=9, offset=480): contains episode 500 with user_status applied. */
        {
            const std::string spill = writeSpillFile(dir, "conan_p9.tmp", bigJson);
            HttpClient http([&](const std::string &, ErrorOperation) {
                return HttpResponse::fromSpillFile(spill, bigJson.size(), true);
            });
            BilibiliApi api(http);
            CommunityService service(api);
            auto res = service.load({BrowseKind::Season, "33378", {}, {}, {}, 9}, Session{});
            assert(res);
            assert(!fileExists(spill));
            const BrowsePage &page = res.value;
            assert(page.episodeOffset == 480);
            assert(page.totalEpisodes == 1501);
            assert(page.items.size() == 1 + kSeasonWindowSize);
            assert(page.items[20].epId == 320500ULL);
            assert(page.items[20].startMs == 612000LL);
            assert(!page.items[20].completed);
        }

        /* Final window (page=26, offset=1500): contains section PV1 episode. */
        {
            const std::string spill = writeSpillFile(dir, "conan_p26.tmp", bigJson);
            HttpClient http([&](const std::string &, ErrorOperation) {
                return HttpResponse::fromSpillFile(spill, bigJson.size(), true);
            });
            BilibiliApi api(http);
            CommunityService service(api);
            auto res = service.load({BrowseKind::Season, "33378", {}, {}, {}, 26}, Session{});
            assert(res);
            assert(!fileExists(spill));
            const BrowsePage &page = res.value;
            assert(page.episodeOffset == 1500);
            assert(page.totalEpisodes == 1501);
            assert(page.items.size() == 2);
            assert(page.items[1].epId == 999999ULL);
            assert(page.items[1].title == "PV1 剧场版预告");
            assert(page.items[1].subtitle == "预告");
            assert(page.items[1].duration == 90);
        }
    }

    /* 2. Small in-RAM Season response (<= 512 KiB, no spill file) + completed status. */
    {
        const std::string smallJson = makeSeasonJson(3, 320002, -1);
        HttpClient http([&](const std::string &, ErrorOperation) {
            HttpResponse r;
            r.body = smallJson;
            r.bodyBytes = smallJson.size();
            return r;
        });
        BilibiliApi api(http);
        CommunityService service(api);
        auto res = service.load({BrowseKind::Season, "33378"}, Session{});
        assert(res);
        assert(res.value.totalEpisodes == 4);
        assert(res.value.items.size() == 5);
        assert(res.value.items[2].completed);
        assert(res.value.items[2].startMs == 0);
    }

    /* 3. API error codes & malformed JSON. */
    {
        HttpClient httpErr([](const std::string &, ErrorOperation) {
            HttpResponse r;
            r.body = "{\"code\":-101,\"message\":\"not logged in\"}";
            return r;
        });
        BilibiliApi apiErr(httpErr);
        CommunityService svcErr(apiErr);
        auto res = svcErr.load({BrowseKind::Season, "33378"}, Session{});
        assert(!res);
        assert(res.error.domain == ErrorDomain::Auth);
        assert(res.error.kind == ErrorKind::LoginRequired);
        assert(res.error.rawCode == -101);

        HttpClient httpBad([](const std::string &, ErrorOperation) {
            HttpResponse r;
            r.body = "{\"code\":0,\"result\":{truncated";
            return r;
        });
        BilibiliApi apiBad(httpBad);
        CommunityService svcBad(apiBad);
        auto bad = svcBad.load({BrowseKind::Season, "33378"}, Session{});
        assert(!bad);
        assert(bad.error.kind == ErrorKind::InvalidResponse);
    }

    /* 4. Generic BilibiliApi::get() fallback reading from a spilled temp file + RAII cleanup. */
    {
        const std::string genericJson = "{\"code\":0,\"data\":{\"hello\":\"world\",\"n\":42}}";
        const std::string spillPath = writeSpillFile(dir, "generic.tmp", genericJson);
        assert(fileExists(spillPath));
        HttpClient http([&](const std::string &, ErrorOperation) {
            return HttpResponse::fromSpillFile(spillPath, genericJson.size(), true);
        });
        BilibiliApi api(http);
        auto res = api.get("https://api.bilibili.com/test", {}, false, ErrorOperation::Request);
        assert(res);
        assert(!fileExists(spillPath));
        assert(res.value.data.at("hello") == "world");
        assert(res.value.data.at("n") == 42);
    }

    assert(rmdir(dir.c_str()) == 0);
    std::puts("season_stream_test ok");
    return 0;
}
