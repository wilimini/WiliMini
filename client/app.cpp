#include "app.hpp"
#include "playback.hpp"
#include "settings_pages.hpp"
#include "services/playback_error.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <utility>

#include <nlohmann/json.hpp>

#include "api/wiliwili/bilibili.h"
#include "api/wiliwili/bilibili/api.h"
#include "api/wiliwili/bilibili/result/home_hots_all_result.h"
#include "api/wiliwili/bilibili/result/home_result.h"
#include "api/wiliwili/bilibili/result/search_result.h"
#include "api/wiliwili/bilibili/result/video_detail_result.h"

namespace pspapp {

namespace {

constexpr size_t kGridColumns = 2;
constexpr size_t kPageSize = 6;

void appendUniqueCards(std::vector<VideoCard>& target, std::vector<VideoCard>& source) {
    for (auto& card : source) {
        if (card.bvid.empty()) continue;
        const bool exists = std::any_of(target.begin(), target.end(), [&card](const VideoCard& item) {
            return item.bvid == card.bvid;
        });
        if (!exists) target.push_back(std::move(card));
    }
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string stripTags(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool tag = false;
    for (char c : text) {
        if (c == '<') {
            tag = true;
        } else if (c == '>') {
            tag = false;
        } else if (!tag) {
            out.push_back(c);
        }
    }
    return out;
}

std::string decodeEntities(std::string text) {
    struct Entity { const char* from; const char* to; };
    static constexpr Entity entities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"},
    };
    for (const auto& entity : entities) {
        size_t position = 0;
        while ((position = text.find(entity.from, position)) != std::string::npos) {
            text.replace(position, std::strlen(entity.from), entity.to);
            position += std::strlen(entity.to);
        }
    }
    return text;
}

std::string cleanTitle(const std::string& title) {
    return trim(decodeEntities(stripTags(title)));
}

std::string normalizePicture(std::string picture, bool thumbnail) {
    if (picture.rfind("//", 0) == 0) picture = "https:" + picture;
    else if (picture.rfind("http://", 0) == 0) picture.replace(0, 7, "https://");
    if (!picture.empty() && picture.find('@') == std::string::npos && thumbnail)
        picture += "@288w_162h_1c.jpg";
    return picture;
}

int parseDurationText(const std::string& text) {
    int total = 0;
    int part = 0;
    bool found = false;
    for (char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            part = part * 10 + (c - '0');
            found = true;
        } else if (c == ':') {
            total = total * 60 + part;
            part = 0;
        }
    }
    if (!found) return 0;
    return total * 60 + part;
}

std::string cachePath(const std::string& bvid) {
    std::string safe;
    safe.reserve(bvid.size());
    for (char c : bvid) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') safe.push_back(c);
    }
    if (safe.empty()) safe = "cover";
    return std::string(CLIENT_ROOT) + "/cache/" + safe + ".jpg";
}

VideoCard fromRecommend(const bilibili::RecommendVideoResult& item) {
    VideoCard card;
    card.bvid = item.bvid;
    card.aid = item.id;
    card.cid = item.cid;
    card.title = cleanTitle(item.title);
    card.author = item.owner.name;
    card.picture = normalizePicture(item.pic, true);
    card.duration = item.duration;
    card.views = static_cast<uint64_t>(item.stat.view);
    return card;
}

VideoCard fromHots(const bilibili::HotsAllVideoResult& item) {
    VideoCard card;
    card.bvid = item.bvid;
    card.aid = item.aid;
    card.cid = item.cid;
    card.title = cleanTitle(item.title);
    card.author = item.owner.name;
    card.picture = normalizePicture(item.pic, true);
    card.duration = item.duration;
    card.views = static_cast<uint64_t>(item.stat.view);
    return card;
}

VideoCard fromSearch(const bilibili::VideoItemSearchResult& item) {
    VideoCard card;
    card.bvid = item.bvid;
    card.aid = item.aid;
    card.title = cleanTitle(item.title);
    card.author = item.subtitle;
    card.picture = normalizePicture(item.cover, true);
    card.duration = parseDurationText(item.rightBottomBadge);
    card.views = item.play;
    return card;
}

VideoCard fromLocal(const LocalVideo& item) {
    VideoCard card;
    card.bvid = item.bvid;
    card.aid = item.aid;
    card.cid = item.cid;
    card.title = item.title;
    card.author = item.author;
    card.picture = item.picture;
    card.duration = item.duration;
    card.startMs = item.positionMs;
    return card;
}

LocalVideo makeLocal(const VideoCard& card) {
    LocalVideo local;
    local.bvid = card.bvid;
    local.aid = card.aid;
    local.cid = card.cid;
    local.title = card.title;
    local.author = card.author;
    local.picture = card.picture;
    local.duration = card.duration;
    local.playedAt = static_cast<uint64_t>(std::time(nullptr));
    local.positionMs = card.startMs;
    return local;
}

std::string readFile(const char* path, size_t limit) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return {};
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    if (size <= 0 || static_cast<size_t>(size) > limit) {
        std::fclose(file);
        return {};
    }
    std::string text(static_cast<size_t>(size), '\0');
    const size_t read = std::fread(text.data(), 1, text.size(), file);
    std::fclose(file);
    text.resize(read);
    return text;
}

void writeFile(const char* path, const std::string& text) {
    const std::string temporary = std::string(path) + ".part";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) return;
    const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    const bool closed = std::fclose(file) == 0;
    if (ok && closed) {
        std::remove(path);
        std::rename(temporary.c_str(), path);
    } else {
        std::remove(temporary.c_str());
    }
}

std::vector<LocalVideo> loadLocal(const char* path) {
    std::vector<LocalVideo> entries;
    const std::string text = readFile(path, 96 * 1024);
    if (text.empty()) return entries;
    try {
        const auto root = nlohmann::json::parse(text);
        if (!root.is_array()) return entries;
        for (const auto& item : root) {
            LocalVideo entry;
            entry.bvid = item.value("bvid", "");
            entry.aid = item.value("aid", 0ULL);
            entry.cid = item.value("cid", 0ULL);
            entry.title = item.value("title", "");
            entry.author = item.value("author", "");
            entry.picture = item.value("picture", "");
            entry.duration = item.value("duration", 0);
            entry.playedAt = item.value("played_at", 0ULL);
            entry.positionMs = std::max<int64_t>(0, item.value("position_ms", int64_t{0}));
            if (!entry.bvid.empty()) entries.push_back(std::move(entry));
        }
    } catch (...) {
    }
    return entries;
}

void saveLocal(const char* path, const std::vector<LocalVideo>& entries) {
    nlohmann::json root = nlohmann::json::array();
    for (const auto& entry : entries) {
        root.push_back({{"bvid", entry.bvid},
                        {"aid", entry.aid},
                        {"cid", entry.cid},
                        {"title", entry.title},
                        {"author", entry.author},
                        {"picture", entry.picture},
                        {"duration", entry.duration},
                        {"played_at", entry.playedAt},
                        {"position_ms", entry.positionMs}});
    }
    writeFile(path, root.dump());
}

void saveSearchHistory(const std::vector<std::string>& entries) {
    nlohmann::json root = nlohmann::json::array();
    for (const auto& entry : entries) root.push_back(entry);
    writeFile(CLIENT_PATH("/search-history.json"), root.dump());
}

std::vector<std::string> loadSearchHistory() {
    std::vector<std::string> entries;
    const std::string text = readFile(CLIENT_PATH("/search-history.json"), 16 * 1024);
    try {
        const auto root = nlohmann::json::parse(text.empty() ? "[]" : text);
        if (root.is_array()) {
            for (const auto& item : root) {
                if (item.is_string()) entries.push_back(item.get<std::string>());
            }
        }
    } catch (...) {
    }
    return entries;
}

const char* jobLabel(int job) {
    switch (static_cast<int>(job)) {
        case 1: return "载入推荐";
        case 2: return "载入热门";
        case 3: return "搜索视频";
        case 4: return "载入详情";
        case 5: return "载入评论";
        case 6: return "准备在线播放";
        case 7: return "载入热搜";
        case 8:
            return "获取登录二维码";
        case 9:
            return "检查扫码状态";
        case 10:
            return "检查登录状态";
        case 11:
            return "退出本机账号";
        default: return "载入";
    }
}

}  // namespace

App* App::workerApp = nullptr;

App::App() = default;

App::~App() {
    shutdown();
}

int App::workerEntry(SceSize, void*) {
    if (workerApp) {
        try {
            if (workerApp->coverActive_) {
                const auto &cover = workerApp->jobCover_;
                workerApp->coverSucceeded_ = bili_http_download(
                    cover.url.c_str(), cachePath(cover.key).c_str(), 320 * 1024) == 0;
            } else workerApp->runJob();
        } catch (const std::bad_alloc &) {
            workerApp->jobTypedError_ = {ErrorDomain::Api, ErrorKind::OutOfMemory,
                                         ErrorOperation::Request};
            workerApp->jobResult_ = -1;
        } catch (const std::exception &) {
            workerApp->jobTypedError_ = {ErrorDomain::Api, ErrorKind::Unknown,
                                         ErrorOperation::Request};
            workerApp->jobResult_ = -1;
        }
    }
    __asm__ volatile("sync" ::: "memory");
    if (workerApp) workerApp->jobFinished_ = true;
    return 0;
}

void App::init() {
    const bool preferencesLoaded = loadPreferences(CLIENT_PATH("/settings.json"), preferences_);
    std::remove((std::string(CLIENT_ROOT) + "/history.json.part").c_str());
    history_ = loadLocal(CLIENT_PATH("/history.json"));
    later_ = loadLocal(CLIENT_PATH("/later.json"));
    searchHistory_ = loadSearchHistory();
    auto restored = authService_.restore();
    if (restored) {
        authView_ = std::move(restored.value);
        sessionCheckPending_ = authView_.state == AuthState::Restored;
    } else {
        authError_ = restored.error;
        logError(CLIENT_PATH("/last-error.txt"), authError_);
    }
    loadCachedCards();
    status_ = recommend_.empty() ? "推荐 · 按 □ 搜索，△ 刷新" : "缓存首页 · 联网后自动刷新";
    if (!preferencesLoaded) status_ = "设置文件无法读取 · 已使用默认设置";
}

void App::shutdown() {
    coverQueue_.clear();
    queuedJob_ = Job::None;
    if (workerStarted_ && worker_ >= 0) {
        transfer_cancel = 1;
        SceUInt timeout = 2000000;
        if (sceKernelWaitThreadEndCB(worker_, &timeout) < 0)
            sceKernelTerminateDeleteThread(worker_);
        else
            sceKernelDeleteThread(worker_);
        worker_ = -1;
        workerStarted_ = false;
    }
    coverActive_ = false;
    bili_finish_playback(&stagedPlayback_);
    stagedPlayback_ = BiliPlayback{};
}

void App::loadCachedCards() {
    BiliHome cached{};
    const std::string text = readFile(CLIENT_PATH("/cache/home.json"), 128 * 1024);
    if (text.empty() || bili_parse_home(text.c_str(), &cached) < 0) return;
    recommend_.clear();
    for (int i = 0; i < cached.count; ++i) {
        const BiliVideo& video = cached.videos[i];
        VideoCard card;
        card.bvid = video.bvid;
        card.cid = static_cast<uint64_t>(video.cid);
        card.title = video.title;
        card.author = video.author;
        card.picture = video.picture;
        card.duration = video.duration;
        card.views = static_cast<uint64_t>(video.views);
        recommend_.push_back(std::move(card));
    }
}

void App::loadDefault() {
    if (root().tab == Tab::Recommend) startRecommend();
    else if (root().tab == Tab::Hots) startHots();
}

void App::refresh() {
    if (screen() == Screen::Browse) { browseRefresh(); return; }
    if (root().tab == Tab::Search && !root().searchError.empty()) {
        startSearch();
        return;
    }
    if (root().tab == Tab::Recommend) {
        recommendPage_ = 1;
        startRecommend();
    } else if (root().tab == Tab::Hots) {
        hotsPage_ = 1;
        startHots();
    } else if (root().tab == Tab::Search) {
        if (root().searchView == SearchView::Results && !root().query.empty()) {
            searchPage_ = 1;
            startSearch();
        } else if (root().searchView == SearchView::Landing) {
            startJob(Job::SearchTopics);
        }
    } else {
        status_ = root().tab == Tab::Search
            ? "按 " + std::string(preferences_.confirmButton()) + " 输入搜索关键词"
            : "我的 · 历史与稍后观看";
    }
}

void App::retry() {
    if (screen() == Screen::Detail && !detailState().loaded) {
        jobCard_ = detailState().detail.card;
        startJob(Job::Detail);
        return;
    }
    if (root().tab == Tab::Search && !root().query.empty()) startSearch();
    else refresh();
}

void App::setTab(Tab nextTab) {
    if (busy() || !atTabRoot()) return;
    if (root().tab == Tab::Discover) {
        root().tabSelections[static_cast<int>(Tab::Discover)] = browseState().selection;
        navigation_.pop();
    } else {
        root().tabSelections[static_cast<int>(root().tab)] = root().selection;
    }
    root().tab = nextTab;
    root().selection = root().tabSelections[static_cast<int>(nextTab)];
    navigation_.current().focus = FocusArea::Content;
    root().navigationSelection = nextTab == Tab::Mine ? 2 : 0;
    if (nextTab == Tab::Discover) {
        BrowseRoute page;
        page.page.request = {BrowseKind::Discover};
        page.page.title = browseTitle(BrowseKind::Discover);
        page.selection = root().tabSelections[static_cast<int>(Tab::Discover)];
        pushBrowse(std::move(page));
        browseRefresh();
        return;
    }
    if (nextTab == Tab::Mine) {
        const size_t total = 1 + history_.size() + later_.size();
        root().selection = total ? std::min(root().selection, total - 1) : 0;

        if (history_.empty() && later_.empty()) status_ = "我的 · 暂无本地记录";
        else status_ = "我的 · 历史与稍后观看";
        return;
    }

    if (nextTab == Tab::Search && root().searchView != SearchView::Results) {
        const size_t count = root().searchView == SearchView::History ? searchHistory_.size() : 2 + root().searchTopics.size();
        root().selection = count ? std::min(root().selection, count - 1) : 0;
        status_ = "搜索 · □ 输入关键词";
        if (root().searchView == SearchView::Landing && root().searchTopics.empty()) startJob(Job::SearchTopics);
        return;
    }
    const auto& list = activeCards();
    root().selection = list.empty() ? 0 : std::min(root().selection, list.size() - 1);
    if (list.empty()) {
        if (nextTab == Tab::Search)
            status_ = "搜索 · 按 " + std::string(preferences_.confirmButton()) + " 打开系统键盘";
        else
            loadDefault();
    } else {
        status_ = nextTab == Tab::Search ? "搜索结果" : nextTab == Tab::Hots ? "热门" : "推荐";
    }
}

bool App::atTabRoot() const {
    return navigation_.size() == 1 ||
           (navigation_.size() == 2 && root().tab == Tab::Discover &&
            hasBrowse() && browseState().page.request.kind == BrowseKind::Discover);
}

void App::shoulder(int direction) {
    if (atTabRoot()) switchTab(direction);
    else if (screen() == Screen::Browse) {
        if (direction > 0) browseMore();
    } else if (screen() == Screen::Detail) {
        if (direction > 0) requestComments();
        else if (!busy()) openVideoActions();
    } else if (screen() == Screen::Comments && direction > 0) loadMoreComments();
}

void App::switchTab(int direction) {
    if (!atTabRoot()) return;
    static const Tab order[] = {Tab::Recommend, Tab::Hots, Tab::Search, Tab::Discover, Tab::Mine};
    int index = 0;
    while (index < 5 && order[index] != root().tab) ++index;
    setTab(order[(index + (direction > 0 ? 1 : 4)) % 5]);
}

std::vector<VideoCard>& App::activeCards() {
    if (root().tab == Tab::Hots) return hots_;
    if (root().tab == Tab::Search) return search_;
    return recommend_;
}

const std::vector<VideoCard>& App::activeCards() const {
    if (root().tab == Tab::Hots) return hots_;
    if (root().tab == Tab::Search) return search_;
    return recommend_;
}

const std::vector<VideoCard>& App::cards() const { return activeCards(); }

int App::page() const {
    if (screen() != Screen::Home) return 1;
    return static_cast<int>(root().selection / kPageSize) + 1;
}

bool App::hasMore() const {
    if (root().tab == Tab::Hots) return hotsHasMore_;
    if (root().tab == Tab::Search) return searchHasMore_;
    return recommendHasMore_;
}

void App::moveSelection(int dx, int dy) {
    if (screen() == Screen::Browse) { browseMove(dx, dy); return; }
    if (utilityPage()) {
        if (screen() == Screen::Settings) {
            auto &selected = navigation_.get<SettingsRoute>().selection;
            if (dy) selected = static_cast<size_t>(std::max(0, std::min(
                static_cast<int>(SettingItem::Count) - 1, static_cast<int>(selected) + dy)));
            else if (dx && selected < static_cast<size_t>(SettingItem::Wifi)) changeSetting();
        } else if (screen() == Screen::Licenses && dy) {
            auto &selected = navigation_.get<LicensesRoute>().selection;
            selected = static_cast<size_t>(std::max(0, std::min(static_cast<int>(settingsLicenseCount) - 1,
                                                             static_cast<int>(selected) + dy)));
        } else if (screen() == Screen::LicenseText && dy) {
            auto &page = navigation_.get<LicenseTextRoute>();
            page.scroll = std::max(0, std::min(page.scrollLimit, page.scroll + dy * (preferences_.bodySize + 4)));
        }
        return;
    }
    if (screen() == Screen::Home || screen() == Screen::Mine) {
        if (navigation_.current().focus == FocusArea::Navigation) {
            if (dy) root().navigationSelection = (root().navigationSelection + (dy > 0 ? 1 : 4)) % 5;
            if (dx > 0) navigation_.current().focus = FocusArea::Content;
            return;
        }
        if (navigation_.current().focus == FocusArea::Categories) {
            if (dy > 0) navigation_.current().focus = FocusArea::Content;
            else if (dx < 0 && root().tab == Tab::Recommend) navigation_.current().focus = FocusArea::Navigation;
            else if (dx) {
                int category = (static_cast<int>(root().tab) + dx + 4) % 4;
                if (category == 3) { setTab(Tab::Discover); return; }
                setTab(static_cast<Tab>(category));
                navigation_.current().focus = root().tab == Tab::Search ? FocusArea::Content : FocusArea::Categories;
            }
            return;
        }
        if (dx < 0 && (screen() == Screen::Mine || root().tab == Tab::Search || root().selection % kGridColumns == 0)) {
            navigation_.current().focus = FocusArea::Navigation;
            root().navigationSelection = root().tab == Tab::Mine ? 2 : 0;
            return;
        }
        if (screen() == Screen::Home && root().tab != Tab::Search && dy < 0 && root().selection < kGridColumns) {
            navigation_.current().focus = FocusArea::Categories;
            return;
        }
    }
    if (screen() == Screen::Home && root().tab == Tab::Search) {
        const size_t total = root().searchView == SearchView::Landing ? 2 + root().searchTopics.size() :
                             root().searchView == SearchView::History ? searchHistory_.size() : search_.size();
        if (!total) return;
        int value = static_cast<int>(root().selection) + dy;
        value = std::max(0, std::min(value, static_cast<int>(total) - 1));
        root().selection = static_cast<size_t>(value);
        if (root().searchView == SearchView::Results && dy > 0 && root().selection + 1 == total && hasMore()) loadNextPage();
        return;
    }
    if (screen() == Screen::Comments) {
        scrollComments((dy ? dy : dx) * 18);
        return;
    }
    if (screen() == Screen::Detail) {
        if (detailState().detail.pages.empty()) return;
        int value = static_cast<int>(detailState().part) + dx + dy;
        if (value < 0) value = 0;
        if (value >= static_cast<int>(detailState().detail.pages.size())) value = static_cast<int>(detailState().detail.pages.size()) - 1;
        detailState().part = static_cast<size_t>(value);
        return;
    }
    if (screen() == Screen::Mine) {
        size_t total = 1 + history_.size() + later_.size();
        if (!total) return;
        int value = static_cast<int>(root().selection) + dx + dy;
        if (value < 0) value = 0;
        if (value >= static_cast<int>(total)) value = static_cast<int>(total) - 1;
        root().selection = static_cast<size_t>(value);
        return;
    }
    const auto& list = activeCards();
    if (list.empty()) return;
    int value = static_cast<int>(root().selection);
    if (dx < 0 && value % static_cast<int>(kGridColumns) != 0) --value;
    if (dx > 0 && value % static_cast<int>(kGridColumns) != static_cast<int>(kGridColumns) - 1 && value + 1 < static_cast<int>(list.size())) ++value;
    if (dy < 0 && value >= static_cast<int>(kGridColumns)) value -= static_cast<int>(kGridColumns);
    if (dy > 0 && value + static_cast<int>(kGridColumns) < static_cast<int>(list.size())) value += static_cast<int>(kGridColumns);
    root().selection = static_cast<size_t>(value);
    if (dy > 0 && value + static_cast<int>(kGridColumns) >= static_cast<int>(list.size()) && hasMore())
        loadNextPage();
}

void App::activate() {
    if (busy() || playbackReady_) return;
    if (screen() == Screen::Browse) { browseActivate(); return; }
    if (navigation_.current().focus == FocusArea::Navigation) {
        if (root().navigationSelection == 1) openBrowse({BrowseKind::DynamicFeed});
        else if (root().navigationSelection == 3) openBrowse({BrowseKind::Inbox});
        else if (root().navigationSelection == 4) openSettings();
        else setTab(root().navigationSelection == 2 ? Tab::Mine : Tab::Recommend);
        return;
    }
    if (utilityPage()) {
        activateUtility();
        return;
    }
    if (navigation_.current().focus == FocusArea::Categories) {
        navigation_.current().focus = FocusArea::Content;
        return;
    }
    if (screen() == Screen::Detail) {
        requestPlayback();
        return;
    }
    if (screen() == Screen::Mine) {
        if (root().selection == 0) {
            openLogin();
            return;
        }
        const size_t localSelection = root().selection - 1;
        if (localSelection < history_.size()) {
            const VideoCard card = fromLocal(history_[localSelection]);
            startDetail(card);
        } else if (localSelection - history_.size() < later_.size()) {
            const VideoCard card = fromLocal(later_[localSelection - history_.size()]);
            startDetail(card);
        }
        return;
    }
    if (screen() == Screen::Home && root().tab == Tab::Search && root().searchView != SearchView::Results) {
        if (root().searchView == SearchView::History) {
            if (root().selection < searchHistory_.size()) search(searchHistory_[root().selection]);
        } else if (root().selection == 1) {
            root().searchLandingSelection = root().selection;
            root().searchView = SearchView::History;
            root().selection = 0;
            status_ = "搜索历史 · " + std::string(preferences_.confirmButton()) + " 搜索 · △ 删除当前项";
        } else if (root().selection >= 2 && root().selection - 2 < root().searchTopics.size()) {
            root().searchLandingSelection = root().selection;
            search(root().searchTopics[root().selection - 2].keyword);
        }
        return;
    }
    const auto& list = activeCards();
    if (root().selection < list.size()) {
        startDetail(list[root().selection]);
    } else if (root().tab == Tab::Search) {
        status_ = "按 " + std::string(preferences_.confirmButton()) + " 打开系统键盘输入关键词";
    }
}

void App::openLogin() {
    if (busy())
        return;
    navigation_.push<LoginRoute>();
    navigation_.current().focus = FocusArea::Content;
    authError_ = {};
    if (authView_.state != AuthState::SignedIn && authView_.state != AuthState::Restored)
        refreshLogin();
    else {
        sessionCheckPending_ = false;
        startJob(Job::AuthCheck);
    }
}
void App::refreshLogin() {
    if (busy()) {
        loginRefreshPending_ = true;
        transfer_cancel = 1;
        return;
    }
    sessionCheckPending_ = false;
    authError_ = {};
    authPollAt_ = 0;
    if (authView_.state == AuthState::SignedIn || authView_.state == AuthState::Restored)
        startJob(Job::AuthCheck);
    else {
        authView_.key.clear();
        authView_.qr.clear();
        authView_.qrSize = 0;
        startJob(Job::AuthGenerate);
    }
}
void App::logout() {
    if (busy())
        return;
    sessionCheckPending_ = false;
    startJob(Job::AuthLogout);
}
void App::cancelRequest() {
    transfer_cancel = 1;
    if (coverActive_ && queuedJob_ != Job::None) {
        queuedJob_ = Job::None;
        busy_ = false;
        coverDirty_ = true;
        status_ = "已取消";
    }
}

void App::back() {
    if (busy()) {
        // Cancel on the worker first; release this route only after joinJob().
        pendingBack_ = true;
        pendingAction_ = PendingAction::None;
        loginRefreshPending_ = false;
        transfer_cancel = 1;
        if (queuedJob_ != Job::None) {
            queuedJob_ = Job::None;
            if (coverActive_) {
                busy_ = false;
                pendingBack_ = false;
                back();
            }
        }
        return;
    }
    if (screen() == Screen::Browse && browseState().confirmingDelete) {
        browseState().confirmingDelete = false;
        status_ = "已取消删除";
        return;
    }
    if (screen() == Screen::Storage && navigation_.get<StorageRoute>().clearing) {
        navigation_.get<StorageRoute>().clearing = false;
        return;
    }
    if (screen() == Screen::Login) {
        authView_.key.clear();
        authView_.qr.clear();
        authView_.qrSize = 0;
        if (authView_.state == AuthState::WaitingScan ||
            authView_.state == AuthState::WaitingConfirm || authView_.state == AuthState::QrExpired)
            authView_.state =
                sessions_.current().valid() ? AuthState::Restored : AuthState::SignedOut;
        if (sessions_.current().valid() && authView_.state != AuthState::SignedIn) {
            authView_ = {};
            authView_.state = AuthState::Restored;
            authView_.mid = sessions_.current().mid;
            sessionCheckPending_ = true;
        } else if (!sessions_.current().valid() && (authView_.state == AuthState::SignedIn ||
                                                    authView_.state == AuthState::Restored))
            authView_ = {};
        navigation_.pop();
        status_ = "已返回";
        if (communityLoginPending_) {
            communityLoginPending_ = false;
            if (hasBrowse()) browseRefresh();
        }
        return;
    }
    if (navigation_.current().focus != FocusArea::Content) {
        navigation_.current().focus = FocusArea::Content;
        return;
    }
    communityInputPending_ = false;
    if (atTabRoot() && root().tab == Tab::Discover) {
        setTab(Tab::Recommend);
        return;
    }
    if (navigation_.pop()) {
        coverDirty_ = true;
        status_ = "已返回";
    } else if (screen() == Screen::Mine) {
        setTab(Tab::Recommend);
    } else if (root().tab == Tab::Search && root().searchView != SearchView::Landing) {
        root().searchView = SearchView::Landing;
        root().selection = std::min(root().searchLandingSelection, root().searchTopics.size() + 1);
        status_ = "搜索 · □ 输入关键词";
    } else if (root().tab == Tab::Search) {
        setTab(Tab::Recommend);
    }
}

void App::search(const std::string& keyword) {
    if (busy()) return;
    const std::string nextQuery = trim(keyword);
    if (nextQuery.empty()) return;
    if (root().tab == Tab::Search && root().searchView == SearchView::Landing) root().searchLandingSelection = root().selection;
    if (root().tab != Tab::Search) root().tabSelections[static_cast<int>(root().tab)] = root().selection;
    if (root().query != nextQuery) search_.clear();
    ++searchGeneration_;
    root().query = nextQuery;
    root().searchView = SearchView::Results;
    navigation_.current().focus = FocusArea::Content;
    searchHistory_.erase(std::remove(searchHistory_.begin(), searchHistory_.end(), root().query), searchHistory_.end());
    searchHistory_.insert(searchHistory_.begin(), root().query);
    if (searchHistory_.size() > 8) searchHistory_.resize(8);
    saveSearchHistory();
    root().tab = Tab::Search;

    root().selection = 0;
    searchPage_ = 1;
    startSearch();
}

bool App::searchInputSelected() const {
    return root().tab == Tab::Search && screen() == Screen::Home && navigation_.current().focus == FocusArea::Content &&
           root().searchView == SearchView::Landing && root().selection == 0;
}

void App::removeSearchHistory() {
    if (busy() || root().tab != Tab::Search || root().searchView != SearchView::History || root().selection >= searchHistory_.size()) return;
    searchHistory_.erase(searchHistory_.begin() + root().selection);
    root().selection = searchHistory_.empty() ? 0 : std::min(root().selection, searchHistory_.size() - 1);
    saveSearchHistory();
    status_ = "已删除这条搜索历史";
}

void App::requestComments() {
    if (!navigation_.find<DetailRoute>() || !detailState().loaded || detailState().detail.card.aid == 0) return;
    if (busy()) {
        if (screen() == Screen::Detail && job_ != Job::Comments) {
            pendingAction_ = PendingAction::Comments;
            status_ = "评论已排队 · 当前任务结束后加载";
        }
        return;
    }
    if (screen() != Screen::Comments) navigation_.push<CommentsRoute>();
    jobCommentNext_ = 0;
    commentsState().items.clear();
    commentsState().next = 0;
    commentsState().end = false;
    commentsState().scroll = 0;
    commentsState().selected = 0;
    startComments();
}

void App::loadMoreComments() {
    if (commentsState().end || commentsState().next <= 0 || detailState().detail.card.aid == 0) return;
    if (busy()) {
        if (screen() == Screen::Comments && job_ != Job::Comments) {
            pendingAction_ = PendingAction::MoreComments;
            status_ = "更多评论已排队";
        }
        return;
    }
    jobCommentNext_ = commentsState().next;
    startComments();
}

void App::scrollComments(int amount) {
    const int maxScroll = commentsState().scrollLimit;
    commentsState().scroll += amount;
    if (commentsState().scroll < 0) commentsState().scroll = 0;
    if (commentsState().scroll > maxScroll) commentsState().scroll = maxScroll;
}

void App::toggleLater() {
    if (detailState().detail.card.bvid.empty()) return;
    auto found = std::find_if(later_.begin(), later_.end(), [this](const LocalVideo& item) {
        return item.bvid == detailState().detail.card.bvid;
    });
    if (found == later_.end()) {
        later_.insert(later_.begin(), makeLocal(detailState().detail.card));
        if (later_.size() > 20) later_.resize(20);
        status_ = "已加入稍后观看";
    } else {
        later_.erase(found);
        status_ = "已从稍后观看移除";
    }
    saveLater();
}

bool App::currentIsLater() const {
    return std::any_of(later_.begin(), later_.end(), [this](const LocalVideo& item) {
        return item.bvid == detailState().detail.card.bvid;
    });
}

VideoCard App::selectedDetailCard() const {
    const auto &page = detailState();
    VideoCard card = page.detail.card;
    if (page.part < page.detail.pages.size()) {
        const auto &part = page.detail.pages[page.part];
        if (card.cid != part.cid) card.startMs = 0;
        card.cid = part.cid;
        card.duration = part.duration;
        if (!part.title.empty()) card.title = part.title;
    }
    return card;
}

void App::requestPlayback(bool fromStart) {
    if (busy() || !detailState().loaded || detailState().detail.card.bvid.empty()) return;
    playbackPaused_ = false;
    VideoCard card = selectedDetailCard();
    if (fromStart || (card.duration > 0 && card.startMs >= card.duration * 1000LL)) card.startMs = 0;
    startPlay(card, card.cid, card.duration);
}

void App::rememberHistory(const VideoCard& card) {
    auto found = std::find_if(history_.begin(), history_.end(), [&card](const LocalVideo& item) {
        return item.bvid == card.bvid && item.cid == card.cid;
    });
    if (found != history_.end()) history_.erase(found);
    history_.insert(history_.begin(), makeLocal(card));
    if (history_.size() > 20) history_.resize(20);
    saveHistory();
}

void App::saveHistory() { saveLocal(CLIENT_PATH("/history.json"), history_); }
void App::saveLater() { saveLocal(CLIENT_PATH("/later.json"), later_); }
void App::saveSearchHistory() { pspapp::saveSearchHistory(searchHistory_); }

void App::startRecommend() {
    jobPage_ = recommendPage_;
    startJob(Job::Recommend);
}

void App::startHots() {
    jobPage_ = hotsPage_;
    startJob(Job::Hots);
}

void App::startSearch() {
    root().searchError.clear();
    jobSearchGeneration_ = searchGeneration_;
    jobPage_ = searchPage_;
    jobQuery_ = root().query;
    startJob(Job::Search);
}

void App::loadNextPage() {
    if (busy() || !hasMore()) return;
    if (root().tab == Tab::Search) {
        if (root().query.empty()) return;
        startSearch();
    } else if (root().tab == Tab::Hots) {
        startHots();
    } else if (root().tab == Tab::Recommend) {
        startRecommend();
    }
}

void App::startDetail(const VideoCard& card) {
    jobCard_ = card;
    auto &page = navigation_.push<DetailRoute>();
    page.detail.card = card;
    startJob(Job::Detail);
}

void App::startComments() {
    startJob(Job::Comments);
}

void App::startPlay(const VideoCard& card, uint64_t cid, int duration) {
    jobCard_ = card;
    jobCid_ = cid;
    jobDuration_ = duration;
    startJob(Job::Play);
}

void App::updateCoverQueue() {
    if (busy_ && !coverActive_) return;
    const size_t selected = hasBrowse()
                                ? browseState().selection : root().selection;
    const bool posters = hasBrowse() &&
                         isPgcPosterPage(browseState().page.request.kind);
    const bool grid = screen() == Screen::Home && root().tab != Tab::Search;
    // Include the preceding row while the selected row scrolls into view.
    const size_t first = posters ? selected / 4 * 4 : grid ? (selected >= 2 ? (selected / 2 - 1) * 2 : 0)
                                                          : (selected > 2 ? selected - 2 : 0);
    if (!coverDirty_ && coverScreen_ == screen() && coverTab_ == root().tab &&
        coverPage_ == first && coverDepth_ == navigation_.size() &&
        coverSearchView_ == root().searchView &&
        coverEnabled_ == preferences_.showCovers) return;
    coverDirty_ = false;
    coverScreen_ = screen();
    coverTab_ = root().tab;
    coverPage_ = first;
    coverDepth_ = navigation_.size();
    coverSearchView_ = root().searchView;
    coverEnabled_ = preferences_.showCovers;
    coverQueue_.clear();
    if (coverActive_) transfer_cancel = 1;
    if (!preferences_.showCovers || playbackReady_ || queuedJob_ != Job::None) return;
    auto enqueue = [&](const std::string &key, std::string url) {
        if (key.empty() || url.empty() || url.size() > 4096 || coverQueue_.size() >= kPageSize) return;
        if (std::any_of(coverQueue_.begin(), coverQueue_.end(), [&](const CoverRequest &c) {
                return c.key == key;
            })) return;
        coverQueue_.push_back({key, std::move(url)});
    };
    if (screen() == Screen::Detail) {
        enqueue(detailState().detail.card.bvid, normalizePicture(detailState().detail.card.picture, true));
    } else if (screen() == Screen::Home && root().tab != Tab::Mine &&
               (root().tab != Tab::Search || root().searchView == SearchView::Results)) {
        const auto &list = cards();
        for (size_t i = first; i < list.size() && i < first + kPageSize; ++i)
            enqueue(list[i].bvid, normalizePicture(list[i].picture, true));
    } else if (screen() == Screen::Mine) {
        const size_t total = history_.size() + later_.size();
        for (size_t i = first; i < total && i < first + 2; ++i) {
            const auto &card = i < history_.size() ? history_[i] : later_[i - history_.size()];
            enqueue(card.bvid, normalizePicture(card.picture, true));
        }
    } else if (hasBrowse()) {
        const auto &items = browseState().page.items;
        for (size_t i = first; i < items.size() && i < first + (posters ? 4 : kPageSize); ++i) {
            const auto &item = items[i];
            if (item.picture.empty()) continue;
            auto url = normalizePicture(item.picture, false);
            const auto suffix = url.find('@');
            if (suffix != std::string::npos) url.resize(suffix);
            url += item.next.kind == BrowseKind::Season ? "@204w_272h_1c.jpg" : "@196w_110h_1c.jpg";
            enqueue(pgcCoverKey(item), std::move(url));
        }
    }
}

void App::startCover() {
    if (coverActive_ || workerStarted_ || !preferences_.showCovers || playbackReady_ ||
        !network_ready() || queuedJob_ != Job::None) return;
    while (!coverQueue_.empty()) {
        jobCover_ = std::move(coverQueue_.front());
        coverQueue_.erase(coverQueue_.begin());
        if (FILE *file = std::fopen(cachePath(jobCover_.key).c_str(), "rb")) {
            std::fseek(file, 0, SEEK_END);
            const long size = std::ftell(file);
            std::fclose(file);
            if (size > 512) continue;
        }
        coverSucceeded_ = false;
        jobFinished_ = false;
        transfer_cancel = 0;
        workerApp = this;
        worker_ = sceKernelCreateThread("wilimini_cover", workerEntry, 0x24, 224 * 1024,
                                        PSP_THREAD_ATTR_USER, nullptr);
        if (worker_ < 0) return;
        coverActive_ = true;
        workerStarted_ = true;
        if (sceKernelStartThread(worker_, 0, nullptr) < 0) {
            sceKernelDeleteThread(worker_);
            worker_ = -1;
            coverActive_ = workerStarted_ = false;
        }
        return;
    }
}

void App::startJob(Job job) {
    if (busy_) {
        if (backgroundJob()) {
            queuedJob_ = job;
            transfer_cancel = 1;
            status_ = std::string(jobLabel(static_cast<int>(job))) + "…";
        }
        return;
    }
    coverQueue_.clear();
    coverDirty_ = true;
    if (coverActive_) {
        // Keep curl and the shared resolver on one worker. Foreground work
        // starts only after the cancelled cover has closed its file/socket.
        queuedJob_ = job;
        transfer_cancel = 1;
        network_progress_reset();
        busy_ = true;
        status_ = std::string(jobLabel(static_cast<int>(job))) + "…";
        return;
    }
    navigation_.current().playbackFailed = false;
    job_ = job;
    jobRouteId_ = navigation_.current().id;
    jobResult_ = -1;
    jobError_.clear();
    jobTypedError_ = {};
    navigation_.current().error = {};
    jobFinished_ = false;
    transfer_cancel = 0;
    network_progress_reset();
    network_error[0] = 0;
    stagedCards_.clear();
    stagedSearchTopics_.clear();
    stagedComments_.clear();
    stagedCommentNext_ = 0;
    stagedCommentsEnd_ = true;
    stagedHasMore_ = false;
    stagedDetail_ = VideoDetailView{};
    stagedPlayback_ = BiliPlayback{};
    stagedDanmaku_.clear();
    stagedSubtitles_.clear();
    busy_ = true;
    if (job != Job::HistoryReport)
        status_ = std::string(jobLabel(static_cast<int>(job))) + "…";
    workerApp = this;
    worker_ = sceKernelCreateThread("wiliwili_job", workerEntry, 0x22, 224 * 1024, PSP_THREAD_ATTR_USER, nullptr);
    if (worker_ < 0) {
        busy_ = false;
        jobResult_ = -1;
        jobError_ = "无法启动网络线程";
        status_ = jobError_;
        return;
    }
    workerStarted_ = true;
    if (sceKernelStartThread(worker_, 0, nullptr) < 0) {
        sceKernelDeleteThread(worker_);
        worker_ = -1;
        workerStarted_ = false;
        busy_ = false;
        jobError_ = "无法启动网络线程";
        status_ = jobError_;
    }
}

void App::poll() {
    updateCoverQueue();
    if (coverActive_) {
        if (!jobFinished_) return;
        joinJob();
        coverActive_ = false;
        // Invalidate only this cache key, even if navigation changed while the
        // completed file was being published. No card or page state is changed.
        if (coverSucceeded_) {
            completedCoverKey_ = jobCover_.key;
            ++coverGeneration_;
        }
        transfer_cancel = 0;
        if (queuedJob_ != Job::None) {
            const Job next = queuedJob_;
            queuedJob_ = Job::None;
            busy_ = false;
            startJob(next);
            return;
        }
    }
    if (!busy_) {
        if (sessionCheckPending_) {
            sessionCheckPending_ = false;
            startJob(Job::AuthCheck);
        } else if (screen() == Screen::Login && !authError_ &&
                   (authView_.state == AuthState::WaitingScan ||
                    authView_.state == AuthState::WaitingConfirm) &&
                   static_cast<uint64_t>(sceKernelGetSystemTimeWide()) >= authPollAt_) {
            jobAuthKey_ = authView_.key;
            startJob(Job::AuthPoll);
        } else if (!playbackReady_) startCover();
        return;
    }
    if (!jobFinished_)
        return;
    joinJob();
    busy_ = false;
    if (job_ == Job::HistoryReport) {
        if (jobResult_ != 0 && !transfer_cancel && jobTypedError_)
            logError(CLIENT_PATH("/last-error.txt"), jobTypedError_);
        job_ = Job::None;
        transfer_cancel = 0;
        const Job next = queuedJob_;
        queuedJob_ = Job::None;
        if (pendingBack_) {
            pendingBack_ = false;
            back();
        } else if (next != Job::None) startJob(next);
        return;
    }
    if (jobResult_ == 0 && !transfer_cancel && navigation_.contains(jobRouteId_)) {
        commitJob();
        coverDirty_ = true;
    } else {
        if (job_ == Job::Play || job_ == Job::CommunityPlay) {
            bili_finish_playback(&stagedPlayback_);
            stagedPlayback_ = BiliPlayback{};
        }
        navigation_.current().error = jobTypedError_;
        if ((job_ == Job::Browse || job_ == Job::CommunityWrite || job_ == Job::CommunityPlay) && hasBrowse()) browseState().error = navigation_.current().error;
        status_ = transfer_cancel
                      ? "已取消"
                      : (navigation_.current().error ? errorMessage(navigation_.current().error)
                                : (jobError_.empty()
                                       ? (network_error[0] ? network_error : "载入失败，△ 重试")
                                       : jobError_));
        if (navigation_.current().error)
            logError(CLIENT_PATH("/last-error.txt"), navigation_.current().error);
        if (job_ == Job::Search)
            root().searchError = transfer_cancel ? "" : status_;
        if (job_ == Job::AuthGenerate || job_ == Job::AuthPoll || job_ == Job::AuthCheck ||
            job_ == Job::AuthLogout) {
            authError_ = navigation_.current().error;
            if (transfer_cancel && !pendingBack_)
                authError_ = {ErrorDomain::Auth, ErrorKind::Cancelled, ErrorOperation::PollLogin};
        }
    }
    if (pendingBack_) {
        pendingBack_ = false;
        loginRefreshPending_ = false;
        transfer_cancel = 0;
        back();
        return;
    }
    if (loginRefreshPending_) {
        loginRefreshPending_ = false;
        transfer_cancel = 0;
        if (sessions_.current().valid()) {
            authView_ = {};
            authView_.state = AuthState::Restored;
            authView_.mid = sessions_.current().mid;
        }
        refreshLogin();
    }
    if (pendingAction_ != PendingAction::None) {
        const PendingAction action = pendingAction_;
        pendingAction_ = PendingAction::None;
        if (action == PendingAction::Comments) requestComments();
        else loadMoreComments();
    }
}

void App::joinJob() {
    if (workerStarted_ && worker_ >= 0) {
        sceKernelWaitThreadEnd(worker_, nullptr);
        sceKernelDeleteThread(worker_);
        worker_ = -1;
        workerStarted_ = false;
    }
}

void App::commitJob() {
    if (job_ == Job::Browse) {
        if (hasBrowse()) {
            auto &frame = browseState();
            if (browseAppending_) {
                for (auto &item : stagedBrowse_.items) {
                    if (item.key.empty() || std::none_of(frame.page.items.begin(), frame.page.items.end(),
                        [&](const BrowseItem &old) { return old.key == item.key; })) frame.page.items.push_back(std::move(item));
                }
                frame.page.request = stagedBrowse_.request;
                frame.page.hasMore = stagedBrowse_.hasMore;
                frame.page.nextCursor = stagedBrowse_.nextCursor;
                frame.page.nextAux = stagedBrowse_.nextAux;
            } else {
                const bool openingSeason = frame.page.items.empty() && stagedBrowse_.request.kind == BrowseKind::Season;
                frame.page = std::move(stagedBrowse_);
                frame.selection = frame.page.items.empty() ? 0 : std::min(frame.selection, frame.page.items.size() - 1);
                if (openingSeason)
                    for (size_t i = 0; i < frame.page.items.size(); ++i)
                        if (frame.page.items[i].next.kind == BrowseKind::Episode) { frame.selection = i; break; }
            }
            frame.error = {};
            status_ = frame.page.items.empty() ? "暂无内容" : frame.page.title;
        }
    } else if (job_ == Job::CommunityWrite) {
        job_ = Job::None;
        commitCommunityWrite();
        return;
    } else if (job_ == Job::AuthGenerate || job_ == Job::AuthCheck || job_ == Job::AuthLogout) {
        const bool clearAccount = stagedAuth_.state == AuthState::SignedOut || stagedAuth_.state == AuthState::SessionExpired;
        if (clearAccount) {
            clearAccountPages();
        }
        authView_ = std::move(stagedAuth_);
        authError_ = {};
        authPollAt_ = sceKernelGetSystemTimeWide() + 2000000;
        status_ = authStateMessage(authView_.state, preferences_.confirmWithCircle);
        if (job_ == Job::AuthCheck && dismissLoginAfterCheck_ && authView_.state == AuthState::SignedIn && screen() == Screen::Login) {
            dismissLoginAfterCheck_ = false; job_ = Job::None; back(); return;
        }
    } else if (job_ == Job::AuthPoll) {
        const auto next = stagedAuth_.state;
        if (next == AuthState::SignedIn) {
            authView_ = std::move(stagedAuth_);
            sessionCheckPending_ = true;
            dismissLoginAfterCheck_ = true;
        } else
            authView_.state = next;
        authError_ = {};
        authPollAt_ = sceKernelGetSystemTimeWide() + 2000000;
        status_ = authStateMessage(authView_.state, preferences_.confirmWithCircle);
    } else if (job_ == Job::SearchTopics) {
        root().searchTopics = std::move(stagedSearchTopics_);
        status_ = root().searchTopics.empty() ? "暂无热搜 · □ 输入关键词" : "热搜已更新";
    } else if (job_ == Job::Recommend) {
        const size_t oldSize = recommend_.size();
        if (jobPage_ > 1) appendUniqueCards(recommend_, stagedCards_);
        else recommend_ = std::move(stagedCards_);
        root().selection = jobPage_ > 1 && oldSize ? oldSize : 0;
        recommendPage_ = jobPage_ + 1;
        recommendHasMore_ = stagedHasMore_;
        status_ = "推荐 · 第 " + std::to_string(jobPage_) + " 页 · " +
                  (recommendHasMore_ ? "↓ 到底继续加载" : "已加载完");
    } else if (job_ == Job::Hots) {
        const size_t oldSize = hots_.size();
        if (jobPage_ > 1) appendUniqueCards(hots_, stagedCards_);
        else hots_ = std::move(stagedCards_);
        root().selection = jobPage_ > 1 && oldSize ? oldSize : 0;
        hotsPage_ = jobPage_ + 1;
        hotsHasMore_ = stagedHasMore_;
        status_ = "热门 · 第 " + std::to_string(jobPage_) + " 页 · " +
                  (hotsHasMore_ ? "↓ 到底继续加载" : "已加载完");
    } else if (job_ == Job::Search) {
        if (stagedSearchGeneration_ != searchGeneration_) {
            job_ = Job::None;
            return;
        }
        const size_t oldSize = search_.size();
        if (jobPage_ > 1) appendUniqueCards(search_, stagedCards_);
        else search_ = std::move(stagedCards_);
        root().selection = jobPage_ > 1 && oldSize ? oldSize : 0;
        searchPage_ = jobPage_ + 1;
        searchHasMore_ = stagedHasMore_;

        status_ = search_.empty() ? "没有找到相关视频" :
                  "搜索：" + root().query + " · 已载入 " + std::to_string(search_.size()) + " 条" +
                      (searchHasMore_ ? " · ↓ 继续" : "");
    } else if (job_ == Job::Detail) {
        detailState().detail = std::move(stagedDetail_);
        detailState().loaded = true;
        detailState().part = 0;
        for (size_t i = 0; i < detailState().detail.pages.size(); ++i)
            if (detailState().detail.pages[i].cid == detailState().detail.card.cid) detailState().part = i;
        status_ = "视频详情 · " + std::string(preferences_.confirmButton()) + " 播放 · R 评论 · □ 稍后观看";
    } else if (job_ == Job::Comments) {
        if (jobCommentNext_ == 0) commentsState().items = std::move(stagedComments_);
        else commentsState().items.insert(commentsState().items.end(), stagedComments_.begin(), stagedComments_.end());
        commentsState().next = stagedCommentNext_;
        commentsState().end = stagedCommentsEnd_;
        status_ = commentsState().items.empty() ? "暂无评论" :
                  "评论 · " + std::to_string(commentsState().items.size()) + " 条" +
                      (commentsState().end ? " · 已加载完" : " · R 加载更多");
    } else if (job_ == Job::Play || job_ == Job::CommunityPlay) {
        if (job_ == Job::Play) {
            playbackCard_ = jobCard_;
            playbackCard_.cid = jobCid_;
            playbackCard_.duration = jobDuration_;
            playbackEpisode_ = playbackSeason_ = 0;
        } else {
            playbackCard_ = {};
            playbackCard_.bvid = jobMedia_.bvid;
            playbackCard_.aid = jobMedia_.aid;
            playbackCard_.cid = jobMedia_.cid;
            playbackCard_.title = jobMedia_.title;
            playbackCard_.duration = jobMedia_.duration;
            playbackCard_.startMs = jobMedia_.startMs;
            playbackEpisode_ = jobMedia_.epId;
            playbackSeason_ = jobMedia_.seasonId;
        }
        if (playbackDanmakuCid_ != playbackCard_.cid) playerSettings_.subtitle_track = 0;
        playbackSubtitles_ = std::move(stagedSubtitles_);
        playbackDanmaku_ = std::move(stagedDanmaku_);
        playbackDanmakuCid_ = playbackCard_.cid;
        playback_ = stagedPlayback_;
        stagedPlayback_ = BiliPlayback{};
        playbackReady_ = true;
        status_ = playback_.streaming ? "在线缓冲 · 正在启动播放器" : "正在启动解码器";
    }
    job_ = Job::None;
}

void App::runJob() {
    if (job_ == Job::Play || job_ == Job::CommunityPlay) {
        const uint64_t cid = job_ == Job::Play ? (jobCid_ ? jobCid_ : jobCard_.cid) : jobMedia_.cid;
        stagedDanmaku_ = cid == playbackDanmakuCid_ ? playbackDanmaku_ : loadDanmaku(cid);
        if (transfer_cancel) { jobResult_ = -1; return; }
        stagedSubtitles_ = cid == playbackDanmakuCid_ ? playbackSubtitles_ : loadSubtitles(
            job_ == Job::Play ? jobCard_.bvid : jobMedia_.bvid, cid,
            job_ == Job::Play ? jobCard_.aid : jobMedia_.aid);
        if (transfer_cancel) { jobResult_ = -1; return; }
    }
    if (job_ == Job::CommunityPlay) {
        if (jobMedia_.next.kind == BrowseKind::LiveStream) {
            jobResult_ = bili_prepare_live_playback(jobMedia_.text.c_str(), &stagedPlayback_);
        } else {
            BiliVideo video{};
            std::snprintf(video.bvid, sizeof(video.bvid), "%s", jobMedia_.bvid.c_str());
            std::snprintf(video.title, sizeof(video.title), "%s", jobMedia_.title.c_str());
            video.cid = static_cast<long long>(jobMedia_.cid); video.duration = jobMedia_.duration;
            jobResult_ = prepareEpisodePlayback(video,jobMedia_.epId,stagedPlayback_,jobError_,playbackQuality_);
        }
    } else if (job_ == Job::Browse) {
        auto result = communityService_.load(jobBrowse_, sessions_.current());
        if (!result) { jobTypedError_ = result.error; jobResult_ = -1; }
        else {
            stagedBrowse_ = std::move(result.value);
            if (jobBrowse_.kind == BrowseKind::Conversation &&
                !stagedBrowse_.nextCursor.empty() &&
                stagedBrowse_.nextCursor != jobBrowse_.cursor) {
                CommunityWrite ack;
                ack.action = CommunityAction::Acknowledge;
                ack.id = jobBrowse_.id;
                ack.type = jobBrowse_.aux.empty() ? 1 : std::stoi(jobBrowse_.aux);
                ack.text = stagedBrowse_.nextCursor;
                // Wiliwili acknowledges the returned max_seqno without blocking
                // display on the acknowledgment response.
                communityService_.write(ack, sessions_.current());
            }
            for(const auto &item:stagedBrowse_.items) {
                if(transfer_cancel)break;
                if(item.next.kind==BrowseKind::Image && !item.picture.empty()) {
                    std::string url=normalizePicture(item.picture,false);
                    const auto suffix=url.find('@'); if(suffix!=std::string::npos)url.resize(suffix);
                    url+="@480w_480h_0e_1l.jpg";
                    const auto path=cachePath("community_image_"+item.next.id);
                    bili_http_download(url.c_str(),path.c_str(),320*1024);
                }
            }
            jobResult_ = 0;
        }
    } else if (job_ == Job::CommunityWrite || job_ == Job::HistoryReport) {
        auto result = communityService_.write(
            job_ == Job::HistoryReport ? jobHistoryWrite_ : jobWrite_, sessions_.current());
        if (!result) { jobTypedError_ = result.error; jobResult_ = -1; }
        else { stagedWrite_ = std::move(result.value); jobResult_ = 0; }
    } else if (job_ == Job::AuthGenerate || job_ == Job::AuthPoll || job_ == Job::AuthCheck ||
        job_ == Job::AuthLogout) {
        Result<AuthView> result;
        if (job_ == Job::AuthGenerate)
            result = authService_.generate();
        else if (job_ == Job::AuthPoll)
            result = authService_.poll(jobAuthKey_);
        else if (job_ == Job::AuthCheck)
            result = authService_.check();
        else
            result = authService_.logout();
        if (!result) {
            jobTypedError_ = result.error;
            jobResult_ = -1;
        } else {
            stagedAuth_ = std::move(result.value);
            jobResult_ = 0;
        }
    } else if (job_ == Job::SearchTopics) {
        auto result = searchService_.topics();
        if (!result) {
            jobTypedError_ = result.error;
            jobResult_ = -1;
            return;
        }
        for (auto &topic : result.value) {
            topic.title = cleanTitle(topic.title);
            stagedSearchTopics_.push_back(std::move(topic));
        }
        jobResult_ = transfer_cancel ? -1 : 0;
    } else if (job_ == Job::Recommend) {
        bool responded = false;
        bilibili::BilibiliClient::get_recommend(
            jobPage_, static_cast<int>(kPageSize), jobPage_ == 1 ? 3 : 4, "V1", 3, 2,
            [&](bilibili::RecommendVideoListResultWrapper result) {
                for (const auto& item : result.item) {
                    VideoCard card = fromRecommend(item);
                    if (!card.bvid.empty()) stagedCards_.push_back(std::move(card));
                }
                stagedHasMore_ = stagedCards_.size() >= kPageSize;
                responded = true;
            },
            [&](const std::string& message, int code) {
                jobError_ = message + " (" + std::to_string(code) + ")";
            });
        if (!responded || (jobPage_ == 1 && stagedCards_.empty())) {
            stagedCards_.clear();
            BiliHome fallback{};
            if (bili_home(jobPage_, &fallback) == 0) {
                for (int i = 0; i < fallback.count; ++i) {
                    VideoCard card;
                    card.bvid = fallback.videos[i].bvid;
                    card.cid = static_cast<uint64_t>(fallback.videos[i].cid);
                    card.title = fallback.videos[i].title;
                    card.author = fallback.videos[i].author;
                    card.picture = fallback.videos[i].picture;
                    card.duration = fallback.videos[i].duration;
                    card.views = static_cast<uint64_t>(fallback.videos[i].views);
                    stagedCards_.push_back(std::move(card));
                }
                stagedHasMore_ = fallback.count >= static_cast<int>(kPageSize);
                responded = !stagedCards_.empty();
                if (responded) jobError_.clear();
            }
        }
        if (!responded) {
            jobResult_ = -1;
            return;
        }
        jobResult_ = transfer_cancel ? -1 : 0;
    } else if (job_ == Job::Hots) {
        bool responded = false;
        bilibili::BilibiliClient::get_hots_all(
            jobPage_, static_cast<int>(kPageSize),
            [&](bilibili::HotsAllVideoListResult result, bool noMore) {
                for (const auto& item : result) {
                    VideoCard card = fromHots(item);
                    if (!card.bvid.empty()) stagedCards_.push_back(std::move(card));
                }
                stagedHasMore_ = !noMore && !result.empty();
                responded = true;
            },
            [&](const std::string& message, int code) {
                jobError_ = message + " (" + std::to_string(code) + ")";
            });
        if (!responded || (jobPage_ == 1 && stagedCards_.empty())) {
            stagedCards_.clear();
            BiliHome fallback{};
            if (bili_home(jobPage_, &fallback) == 0) {
                for (int i = 0; i < fallback.count; ++i) {
                    VideoCard card;
                    card.bvid = fallback.videos[i].bvid;
                    card.cid = static_cast<uint64_t>(fallback.videos[i].cid);
                    card.title = fallback.videos[i].title;
                    card.author = fallback.videos[i].author;
                    card.picture = fallback.videos[i].picture;
                    card.duration = fallback.videos[i].duration;
                    card.views = static_cast<uint64_t>(fallback.videos[i].views);
                    stagedCards_.push_back(std::move(card));
                }
                stagedHasMore_ = fallback.count >= static_cast<int>(kPageSize);
                responded = !stagedCards_.empty();
                if (responded) jobError_.clear();
            }
        }
        if (!responded) {
            jobResult_ = -1;
            return;
        }
        jobResult_ = transfer_cancel ? -1 : 0;
    } else if (job_ == Job::Search) {
        auto result = searchService_.search(
            {jobQuery_, static_cast<unsigned>(jobPage_), jobSearchGeneration_});
        if (!result) {
            jobTypedError_ = result.error;
            jobResult_ = -1;
            return;
        }
        stagedSearchGeneration_ = result.value.request.generation;
        stagedHasMore_ = result.value.hasMore;
        for (const auto &video : result.value.videos)
            stagedCards_.push_back(fromSearch(video));
        jobResult_ = transfer_cancel ? -1 : 0;
    } else if (job_ == Job::Detail) {
        bool ok = false;
        bilibili::BilibiliClient::get_video_detail(
            jobCard_.bvid,
            [&](bilibili::VideoDetailResult result) {
                stagedDetail_.card.bvid = result.bvid.empty() ? jobCard_.bvid : result.bvid;
                stagedDetail_.card.aid = result.aid;
                stagedDetail_.card.mid = std::to_string(result.owner.mid);
                stagedDetail_.card.cid = result.pages.empty() ? jobCard_.cid : result.pages[0].cid;
                stagedDetail_.card.title = cleanTitle(result.title.empty() ? jobCard_.title : result.title);
                stagedDetail_.card.author = result.owner.name.empty() ? jobCard_.author : result.owner.name;
                stagedDetail_.card.picture = normalizePicture(result.pic.empty() ? jobCard_.picture : result.pic, true);
                stagedDetail_.card.duration = result.duration ? result.duration : jobCard_.duration;
                stagedDetail_.card.views = static_cast<uint64_t>(result.stat.view);
                stagedDetail_.danmaku = static_cast<uint64_t>(std::max(0, result.stat.danmaku));
                stagedDetail_.like = static_cast<uint64_t>(std::max(0, result.stat.like));
                stagedDetail_.coin = static_cast<uint64_t>(std::max(0, result.stat.coin));
                stagedDetail_.favorite = static_cast<uint64_t>(std::max(0, result.stat.favorite));
                stagedDetail_.description = result.desc;
                for (const auto& page : result.pages) {
                    stagedDetail_.pages.push_back({page.cid, page.part, page.duration});
                }
                if (stagedDetail_.pages.empty() && jobCard_.cid) {
                    stagedDetail_.pages.push_back({jobCard_.cid, jobCard_.title, jobCard_.duration});
                }
                ok = true;
            },
            [&](const std::string& message, int code) {
                jobError_ = message + " (" + std::to_string(code) + ")";
            });
        if (!ok) {
            jobResult_ = -1;
            return;
        }
        for (const auto &part : stagedDetail_.pages) {
            if (part.cid == jobCard_.cid) {
                stagedDetail_.card.cid = part.cid;
                stagedDetail_.card.startMs = jobCard_.startMs;
                break;
            }
        }
        if (sessions_.current().valid()) {
            auto progress = communityService_.videoProgress(stagedDetail_.card.bvid, stagedDetail_.card.cid);
            // Match Wiliwili: a different last_play_cid must not silently
            // switch the part the user opened.
            if (progress && progress.value.cid == stagedDetail_.card.cid)
                stagedDetail_.card.startMs = progress.value.positionMs;
        }
        jobResult_ = transfer_cancel ? -1 : 0;
    } else if (job_ == Job::Comments) {
        bool ok = false;
        bilibili::BilibiliClient::get_comment(
            std::to_string(detailState().detail.card.aid), jobCommentNext_, 3, 1,
            [&](bilibili::VideoCommentResultWrapper result) {
                auto append = [&](const bilibili::VideoCommentResult& reply, bool top) {
                    CommentItem item;
                    item.id = reply.rpid;
                    item.mid = reply.member.mid;
                    item.root = std::to_string(reply.root);
                    item.parent = std::to_string(reply.parent);
                    item.replies = static_cast<int>(reply.rcount);
                    item.action = static_cast<int>(reply.action);
                    item.author = reply.member.uname;
                    item.message = decodeEntities(reply.content.message);
                    item.time = reply.reply_control.time_desc;
                    if (item.time.empty() && reply.ctime) {
                        const std::time_t stamp = static_cast<std::time_t>(reply.ctime);
                        char buffer[32]{};
                        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", std::localtime(&stamp));
                        item.time = buffer;
                    }
                    item.like = static_cast<int>(reply.like);
                    item.top = top;
                    item.uploader = reply.member.is_uploader;
                    item.message = trim(item.message);
                    if (item.author.empty() || item.message.empty()) return;
                    const auto duplicate = [&item](const CommentItem& existing) {
                        if (item.id && existing.id) return item.id == existing.id;
                        return item.author == existing.author && item.message == existing.message &&
                               item.time == existing.time;
                    };
                    if (std::any_of(commentsState().items.begin(), commentsState().items.end(), duplicate) ||
                        std::any_of(stagedComments_.begin(), stagedComments_.end(), duplicate))
                        return;
                    stagedComments_.push_back(std::move(item));
                };
                if (jobCommentNext_ == 0)
                    for (const auto& reply : result.top_replies) append(reply, true);
                for (const auto& reply : result.replies) append(reply, false);
                stagedCommentNext_ = result.cursor.next;
                stagedCommentsEnd_ = result.cursor.is_end;
                if (stagedCommentNext_ <= 0 && !stagedCommentsEnd_) stagedCommentsEnd_ = true;
                ok = true;
            },
            [&](const std::string& message, int code) {
                jobError_ = message + " (" + std::to_string(code) + ")";
            });
        jobResult_ = ok && !transfer_cancel ? 0 : -1;
    } else if (job_ == Job::Play || job_ == Job::CommunityPlay) {
        BiliVideo video{};
        std::snprintf(video.bvid, sizeof(video.bvid), "%s", jobCard_.bvid.c_str());
        std::snprintf(video.title, sizeof(video.title), "%s", jobCard_.title.c_str());
        std::snprintf(video.author, sizeof(video.author), "%s", jobCard_.author.c_str());
        std::snprintf(video.picture, sizeof(video.picture), "%s", jobCard_.picture.c_str());
        video.cid = static_cast<long long>(jobCid_ ? jobCid_ : jobCard_.cid);
        video.duration = jobDuration_;
        std::string playbackError;
        const int result = preparePlayback(video, stagedPlayback_, playbackError, playbackQuality_);
        if (result < 0) {
            jobError_ = playbackError.empty() ? "无法准备视频流" : playbackError;
            jobResult_ = result;
            return;
        }
        jobResult_ = 0;
    }
}

void App::seekPlayback(int64_t positionMs, bool paused, int quality) {
    if (quality) playbackQuality_ = quality;
    // Decoder and all source readers have joined. Cancelled range sources
    // cannot be reused; prepare fresh ones through the existing worker.
    bili_finish_playback(&playback_);
    playback_ = BiliPlayback{};
    playbackReady_ = false;
    playbackPaused_ = paused;
    playbackCard_.startMs = std::max<int64_t>(0, positionMs);
    if (playbackEpisode_) {
        jobMedia_.startMs = playbackCard_.startMs;
        startJob(Job::CommunityPlay);
    } else {
        startPlay(playbackCard_, playbackCard_.cid, playbackCard_.duration);
    }
}

void App::finishPlayback(int error, unsigned frames, int eof, int cancelled, PspVideoStage stage, int64_t positionMs) {
    playbackPaused_ = false;
    playbackDanmaku_.clear(); playbackDanmakuCid_ = 0;
    playbackSubtitles_.clear();
    const int cacheResult = bili_finish_playback(&playback_);
    playbackReady_ = false;
    playback_ = BiliPlayback{};
    if (cacheResult < 0 && error == 0 && !cancelled) {
        error = cacheResult;
        stage = PSP_VIDEO_STAGE_CLEANUP;
    }
    navigation_.current().playbackFailed = error < 0 && !cancelled;
    if (navigation_.current().playbackFailed) {
        navigation_.current().error = pspapp::playbackError(error, stage);
        status_ = errorMessage(navigation_.current().error);
        logError(CLIENT_PATH("/last-error.txt"), navigation_.current().error);
        if (FILE *out = std::fopen(CLIENT_PATH("/last-playback-error.txt"), "w")) {
            std::fprintf(out, "stage=%s\nerror=%d\ncleanup_error=%d\nframes=%u\nmessage=%s\n",
                         psp_video_stage_name(stage), error, cacheResult, frames, status_.c_str());
            std::fclose(out);
        }
    } else {
        status_ = std::string(cancelled ? "播放已取消" : "播放完成") + " · " + std::to_string(frames) + " 帧" +
                  (eof ? " · EOF" : "");
    }
    if (frames && playbackCard_.aid && playbackCard_.cid) {
        playbackCard_.startMs = eof ? 0 : std::max<int64_t>(0, positionMs);
        rememberHistory(playbackCard_);
        for (auto &entry : navigation_.entries()) {
            if (auto *page = std::get_if<DetailRoute>(&entry.state)) {
                const uint64_t selectedCid = page->part < page->detail.pages.size()
                    ? page->detail.pages[page->part].cid : page->detail.card.cid;
                if (page->detail.card.aid == playbackCard_.aid && selectedCid == playbackCard_.cid) {
                    page->detail.card.cid = playbackCard_.cid;
                    page->detail.card.startMs = playbackCard_.startMs;
                }
            } else if (auto *page = std::get_if<BrowseRoute>(&entry.state)) {
                for (auto &item : page->page.items)
                    if (item.aid == playbackCard_.aid && item.cid == playbackCard_.cid)
                        item.startMs = playbackCard_.startMs;
            }
        }
        if (!navigation_.current().playbackFailed && sessions_.current().valid()) {
            jobHistoryWrite_ = {};
            jobHistoryWrite_.action = CommunityAction::ReportHistory;
            jobHistoryWrite_.aid = playbackCard_.aid;
            jobHistoryWrite_.cid = playbackCard_.cid;
            jobHistoryWrite_.progress = eof ? playbackCard_.duration : static_cast<int>(positionMs / 1000);
            jobHistoryWrite_.duration = playbackCard_.duration;
            jobHistoryWrite_.episodeId = playbackEpisode_;
            jobHistoryWrite_.seasonId = playbackSeason_;
            startJob(Job::HistoryReport);
        }
    }
    (void)error;
}

}  // namespace pspapp
