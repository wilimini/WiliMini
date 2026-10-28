#pragma once

#include <cassert>
#include <cstdint>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "services/community_service.hpp"
#include "services/search_service.hpp"

namespace pspapp {

enum class Tab { Recommend, Hots, Search, Mine, Discover };
enum class Screen { Home, Detail, Comments, Mine, Login, Settings, Storage, Help, About, Licenses, LicenseText, Browse };
enum class FocusArea { Content, Navigation, Categories };
enum class SearchView { Landing, History, Results };

struct VideoCard {
    std::string bvid;
    std::string title;
    std::string author;
    std::string picture;
    std::string mid;
    uint64_t aid = 0;
    uint64_t cid = 0;
    uint64_t views = 0;
    int duration = 0;
    int64_t startMs = 0;
};

struct VideoPart {
    uint64_t cid = 0;
    std::string title;
    int duration = 0;
};

struct VideoDetailView {
    VideoCard card;
    std::string description;
    std::vector<VideoPart> pages;
    uint64_t danmaku = 0;
    uint64_t like = 0;
    uint64_t coin = 0;
    uint64_t favorite = 0;
};

struct CommentItem {
    uint64_t id = 0;
    std::string author;
    std::string message;
    std::string time;
    int like = 0;
    bool top = false;
    bool uploader = false;
    std::string mid, root, parent;
    int replies = 0, action = 0;
};

// Only the root changes tabs. Every child is an independent page instance.
struct RootRoute {
    Tab tab = Tab::Recommend;
    size_t selection = 0, tabSelections[5]{};
    int navigationSelection = 0;
    SearchView searchView = SearchView::Landing;
    size_t searchLandingSelection = 0;
    std::string query, searchError;
    std::vector<SearchTopic> searchTopics;
};
struct DetailRoute {
    static constexpr Screen kind = Screen::Detail;
    VideoDetailView detail;
    size_t part = 0;
    bool loaded = false;
};
struct CommentsRoute {
    static constexpr Screen kind = Screen::Comments;
    std::vector<CommentItem> items;
    size_t selected = 0;
    int scroll = 0, scrollLimit = 0, next = 0;
    bool end = true;
};
struct BrowseRoute {
    static constexpr Screen kind = Screen::Browse;
    BrowsePage page;
    BrowseItem context;
    size_t selection = 0;
    int scroll = 0, scrollLimit = 0;
    AppError error;
    bool confirmingDelete = false;
};
struct LoginRoute { static constexpr Screen kind = Screen::Login; };
struct SettingsRoute {
    static constexpr Screen kind = Screen::Settings;
    size_t selection = 0;
};
struct StorageRoute {
    static constexpr Screen kind = Screen::Storage;
    bool clearing = false;
};
struct HelpRoute { static constexpr Screen kind = Screen::Help; };
struct AboutRoute { static constexpr Screen kind = Screen::About; };
struct LicensesRoute {
    static constexpr Screen kind = Screen::Licenses;
    size_t selection = 0;
};
struct LicenseTextRoute {
    static constexpr Screen kind = Screen::LicenseText;
    std::string text;
    int scroll = 0, scrollLimit = 0;
};
using RouteState = std::variant<RootRoute, DetailRoute, CommentsRoute, BrowseRoute,
                                LoginRoute, SettingsRoute, StorageRoute, HelpRoute,
                                AboutRoute, LicensesRoute, LicenseTextRoute>;
struct RouteEntry {
    uint64_t id;
    RouteState state;
    FocusArea focus = FocusArea::Content;
    AppError error;
    bool playbackFailed = false;
    Screen screen() const {
        return std::visit([](const auto &page) {
            if constexpr (std::is_same_v<std::decay_t<decltype(page)>, RootRoute>)
                return page.tab == Tab::Mine ? Screen::Mine : Screen::Home;
            else return page.kind;
        }, state);
    }
};

class NavigationStack {
public:
    NavigationStack() { push(RootRoute{}); }
    const RouteEntry &current() const { return entries_.back(); }
    RouteEntry &current() { return entries_.back(); }
    size_t size() const { return entries_.size(); }
    template<class T> T &push(T page = {}) {
        entries_.push_back({nextId_++, RouteState{std::move(page)}, FocusArea::Content, {}, false});
        return std::get<T>(entries_.back().state);
    }
    bool pop() {
        if (entries_.size() == 1) return false;
        entries_.pop_back();
        return true;
    }
    void clearChildren() { entries_.erase(entries_.begin() + 1, entries_.end()); }
    bool contains(uint64_t id) const {
        for (const auto &entry : entries_) if (entry.id == id) return true;
        return false;
    }
    // Lookup includes ancestors (e.g. the detail page below a comments page).
    // Do not retain references across push/pop/clearChildren.
    template<class T> T *find() {
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
            if (auto *page = std::get_if<T>(&it->state)) return page;
        return nullptr;
    }
    template<class T> const T *find() const {
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
            if (const auto *page = std::get_if<T>(&it->state)) return page;
        return nullptr;
    }
    template<class T> T &get() { auto *page = find<T>(); assert(page); return *page; }
    template<class T> const T &get() const { const auto *page = find<T>(); assert(page); return *page; }
    std::vector<RouteEntry> &entries() { return entries_; }
private:
    std::vector<RouteEntry> entries_;
    uint64_t nextId_ = 1;
};

} // namespace pspapp
