#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <pspkernel.h>

#include "bilibili.h"
#include "preferences.hpp"
#include "services/auth_service.hpp"
#include "services/search_service.hpp"
#include "services/community_service.hpp"

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

struct LocalVideo {
    std::string bvid;
    std::string title;
    std::string author;
    std::string picture;
    uint64_t aid = 0;
    uint64_t cid = 0;
    int duration = 0;
    uint64_t playedAt = 0;
    int64_t positionMs = 0;
};

class App {
public:
    App();
    ~App();

    void init();
    void poll();
    void shutdown();

    void switchTab(int direction);
    void setTab(Tab tab);
    Tab tab() const { return tab_; }
    Screen screen() const { return screen_; }
    FocusArea focusArea() const { return focusArea_; }
    int navigationSelection() const { return navigationSelection_; }
    void openSettings();
    void changeSetting();
    void updateStorage();
    bool utilityPage() const { return screen_ >= Screen::Settings && screen_ <= Screen::LicenseText; }
    const Preferences& preferences() const { return preferences_; }
    size_t settingSelection() const { return settingSelection_; }
    size_t licenseSelection() const { return licenseSelection_; }
    const std::string& licenseText() const { return licenseText_; }
    int documentScroll() const { return documentScroll_; }
    void setDocumentScrollLimit(int limit) { documentScrollLimit_ = limit; }
    size_t coverFiles() const { return coverFiles_; }
    uint64_t coverBytes() const { return coverBytes_; }
    bool clearingCovers() const { return clearingCovers_; }
    unsigned coverGeneration() const { return coverGeneration_; }
    const std::string &completedCoverKey() const { return completedCoverKey_; }
    std::string buildVersion() const;

    void openBrowse(BrowseRequest request);
    void browseMore();
    void browseRefresh();
    void browseActivate();
    void browseMove(int dx, int dy);
    void browseBack();
    void communityWrite(CommunityWrite request);
    void commentActions();
    void selectComment(int direction);
    void beginComment();
    void beginMessage();
    void submitCommunityInput(const std::string &text);
    bool communityInputPending() const { return communityInputPending_; }
    const std::string &communityDraft() const { return communityDraft_; }
    void cancelCommunityInput() { communityInputPending_ = false; }
    bool hasBrowse() const { return !browseStack_.empty(); }
    const BrowsePage &browsePage() const { return browseStack_.back().page; }
    const AppError &browseError() const { return browseStack_.back().error; }
    size_t browseSelection() const { return browseStack_.back().selection; }
    int browseScroll() const { return browseStack_.back().scroll; }
    size_t selectedComment() const { return selectedComment_; }
    void setBrowseScrollLimit(int limit) { browseScrollLimit_ = limit; }
    void communityShortcut();
    void openVideoActions();

    void refresh();
    void cancelRequest();
    void moveSelection(int dx, int dy);
    void activate();
    void back();
    void search(const std::string &keyword);
    bool searchInputSelected() const;
    void removeSearchHistory();
    SearchView searchView() const { return searchView_; }
    const std::vector<SearchTopic> &searchTopics() const { return searchTopics_; }
    const std::string &searchError() const { return searchError_; }
    void retry();
    void openLogin();
    void refreshLogin();
    void logout();
    const AuthView &auth() const { return authView_; }
    const AppError &authError() const { return authError_; }
    const AppError &error() const { return error_; }
    void setStatus(const std::string &status) { status_ = status; }
    void requestComments();
    void loadMoreComments();
    void scrollComments(int amount);
    void setCommentScrollLimit(int limit) { commentScrollLimit_ = limit; }
    void toggleLater();
    void requestPlayback(bool fromStart = false);
    int64_t detailStartMs() const { return selectedDetailCard().startMs; }
    void finishPlayback(int error, unsigned frames, int eof, int cancelled,
                        PspVideoStage stage = PSP_VIDEO_STAGE_NONE, int64_t positionMs = 0);
    int64_t playbackStartMs() const { return playbackCard_.startMs; }
    bool playbackFailed() const { return playbackFailed_; }

    bool busy() const { return busy_; }
    bool pollingLogin() const { return busy_ && job_ == Job::AuthPoll; }
    bool playbackReady() const { return playbackReady_; }
    BiliPlayback &playback() { return playback_; }
    const BiliPlayback &playback() const { return playback_; }
    const VideoDetailView &detail() const { return detail_; }
    const std::vector<VideoCard> &cards() const;
    const std::vector<CommentItem> &comments() const { return comments_; }
    const std::vector<LocalVideo> &history() const { return history_; }
    const std::vector<LocalVideo> &later() const { return later_; }
    const std::vector<std::string> &searchHistory() const { return searchHistory_; }
    const std::string &status() const { return status_; }
    const std::string &query() const { return query_; }
    size_t selection() const { return selection_; }
    size_t detailPart() const { return detailPart_; }
    int commentScroll() const { return commentScroll_; }
    bool commentsEnd() const { return commentsEnd_; }
    int page() const;
    bool hasMore() const;
    bool currentIsLater() const;

private:
    enum class Job {
        None,
        Recommend,
        Hots,
        Search,
        Detail,
        Comments,
        Play,
        SearchTopics,
        AuthGenerate,
        AuthPoll,
        AuthCheck,
        AuthLogout,
        Browse,
        CommunityWrite,
        CommunityPlay,
        HistoryReport
    };

    void startJob(Job job);
    void runJob();
    void commitJob();
    void joinJob();
    void updateCoverQueue();
    void startCover();
    void loadDefault();
    void loadCachedCards();
    void saveHistory();
    void saveLater();
    void saveSearchHistory();
    void rememberHistory(const VideoCard &card);
    void activateUtility();
    std::vector<VideoCard> &activeCards();
    const std::vector<VideoCard> &activeCards() const;
    void startRecommend();
    void startHots();
    void startSearch();
    void startDetail(const VideoCard &card);
    void startComments();
    void loadNextPage();
    void startPlay(const VideoCard &card, uint64_t cid, int duration);
    VideoCard selectedDetailCard() const;

    static int workerEntry(SceSize args, void *argp);
    static App *workerApp;

    Preferences preferences_;
    Screen settingsReturnScreen_ = Screen::Home;
    size_t settingSelection_ = 0, licenseSelection_ = 0;
    int documentScroll_ = 0, documentScrollLimit_ = 0;
    std::string licenseText_;
    size_t coverFiles_ = 0;
    uint64_t coverBytes_ = 0;
    bool clearingCovers_ = false;
    unsigned coverGeneration_ = 0;
    struct CoverRequest { std::string key, url; };
    std::vector<CoverRequest> coverQueue_;
    CoverRequest jobCover_;
    std::string completedCoverKey_;
    bool coverActive_ = false, coverDirty_ = true, coverEnabled_ = false;
    SearchView coverSearchView_ = SearchView::Landing;
    bool coverSucceeded_ = false;
    Screen coverScreen_ = Screen::Home;
    Tab coverTab_ = Tab::Recommend;
    size_t coverPage_ = 0, coverDepth_ = 0;

    struct BrowseFrame {
        BrowsePage page;
        BrowseItem context;
        size_t selection = 0;
        int scroll = 0;
        AppError error;
    };
    std::vector<BrowseFrame> browseStack_;
    Screen browseReturnScreen_ = Screen::Home, loginReturnScreen_ = Screen::Mine;
    Screen detailReturnScreen_ = Screen::Home;
    BrowseRequest jobBrowse_;
    BrowsePage stagedBrowse_;
    CommunityWrite jobWrite_;
    BrowseItem jobMedia_;
    VideoCard playbackCard_;
    uint64_t playbackEpisode_ = 0, playbackSeason_ = 0;
    ApiResponse stagedWrite_;
    BrowseItem activeComment_;
    size_t selectedComment_ = 0;
    bool browseAppending_ = false, communityInputPending_ = false, confirmingDelete_ = false;
    bool communityLoginPending_ = false, dismissLoginAfterCheck_ = false;
    int browseScrollLimit_ = 0;
    std::string communityDraft_;
    CommunityWrite inputWrite_;
    void clearAccountPages();
    void commitCommunityWrite();

    BilibiliApi api_;
    CommunityService communityService_{api_};
    SearchService searchService_{api_};
    SessionStore sessions_{CLIENT_ROOT "/session.json"};
    AuthService authService_{api_, sessions_};
    AuthView authView_, stagedAuth_;
    AppError error_, jobTypedError_, authError_;
    std::string jobAuthKey_;
    uint64_t authPollAt_ = 0;
    bool sessionCheckPending_ = false;
    bool leaveLogin_ = false;
    bool loginRefreshPending_ = false;
    uint64_t searchGeneration_ = 0, jobSearchGeneration_ = 0, stagedSearchGeneration_ = 0;

    Tab tab_ = Tab::Recommend;
    Screen screen_ = Screen::Home;
    FocusArea focusArea_ = FocusArea::Content;
    int navigationSelection_ = 0;
    size_t tabSelections_[5]{};
    size_t selection_ = 0;
    size_t detailPart_ = 0;
    int commentScroll_ = 0;
    int commentScrollLimit_ = 0;
    int recommendPage_ = 1;
    int hotsPage_ = 1;
    int searchPage_ = 1;
    SearchView searchView_ = SearchView::Landing;
    size_t searchLandingSelection_ = 0;
    std::string searchError_;
    std::vector<SearchTopic> searchTopics_;
    std::vector<SearchTopic> stagedSearchTopics_;
    bool recommendHasMore_ = true;
    bool hotsHasMore_ = true;
    bool searchHasMore_ = true;
    bool busy_ = false;
    bool smokeStageDone_ = false;
    std::string query_;
    bool playbackFailed_ = false;
    std::string status_ = "Wiliwili C++17 UI · 正在初始化";

    std::vector<VideoCard> recommend_;
    std::vector<VideoCard> hots_;
    std::vector<VideoCard> search_;
    std::vector<LocalVideo> history_;
    std::vector<LocalVideo> later_;
    std::vector<std::string> searchHistory_;
    VideoDetailView detail_;
    std::vector<CommentItem> comments_;
    std::vector<CommentItem> stagedComments_;
    int commentsNext_ = 0;
    bool commentsEnd_ = true;
    BiliPlayback playback_{};
    bool playbackReady_ = false;

    Job job_ = Job::None;
    Job queuedJob_ = Job::None;
    int jobPage_ = 1;
    int jobCommentNext_ = 0;
    int stagedCommentNext_ = 0;
    bool stagedCommentsEnd_ = true;
    bool stagedHasMore_ = false;
    enum class PendingAction { None, Comments, MoreComments };
    PendingAction pendingAction_ = PendingAction::None;
    std::string jobQuery_;
    VideoCard jobCard_{};
    uint64_t jobCid_ = 0;
    int jobDuration_ = 0;
    int jobResult_ = -1;
    std::string jobError_;
    bool jobFinished_ = false;
    SceUID worker_ = -1;
    bool workerStarted_ = false;

    std::vector<VideoCard> stagedCards_;
    VideoDetailView stagedDetail_;
    BiliPlayback stagedPlayback_{};
};

} // namespace pspapp
