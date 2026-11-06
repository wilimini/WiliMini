#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <pspkernel.h>

#include "bilibili.h"
#include "navigation.hpp"
#include "preferences.hpp"
#include "tile_layout.hpp"
#include "services/auth_service.hpp"
#include "server_discovery.h"
#include "playback.hpp"
#include "services/search_service.hpp"
#include "services/community_service.hpp"

namespace pspapp {

// Applies the saved log level and per-channel overrides to the process-wide logger.
void applyLogPreferences(const Preferences& preferences);


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
    void shoulder(int direction);
    bool atTabRoot() const;
    void setTab(Tab tab);
    Tab tab() const { return root().tab; }
    Screen screen() const { return navigation_.current().screen(); }
    FocusArea focusArea() const { return navigation_.current().focus; }
    int navigationSelection() const { return root().navigationSelection; }
    void openSettings();
    // Tile home screen. The hub is the root view while the tile theme is on and no tab has
    // been entered; child pages opened from it return to it when popped.
    bool tileHubVisible() const { return preferences_.tileHome() && hubOpen_ && navigation_.size() == 1; }
    int tileSelection() const { return hubSelection_; }
    // Changes whenever the visible page changes (route, tab or search view); drives page transitions.
    uint64_t pageKey() const {
        return navigation_.current().id * 64 + static_cast<uint64_t>(root().tab) * 8 +
               static_cast<uint64_t>(root().searchView);
    }
    void tileMove(int dx, int dy);
    void tileActivate();
    // Confirm starts a short launch animation; the destination opens when it ends (see poll()).
    bool tileLaunching() const { return hubLaunchAt_ != 0; }
    int tileLaunchMs() const;
    void tileFinishLaunch();
    bool tileLive(TileId tile, std::string& big, std::string& small) const;
    std::string tileSubtitle(TileId tile) const;
    void changeSetting();
    // Stream-server rows that need the text keyboard; main.cpp runs it, then submits.
    enum class SettingsInput { None, Address, Token };
    SettingsInput settingsInput() const { return settingsInput_; }
    std::string settingsInputTitle() const;
    std::string settingsInputInitial() const;
    void submitSettingsInput(const std::string& text);
    void cancelSettingsInput() { settingsInput_ = SettingsInput::None; }
    // Message for a StreamServer playback error, in the interface language.
    std::string streamErrorMessage(const AppError& error) const;
    void updateStorage();
    bool utilityPage() const { return screen() >= Screen::Settings && screen() <= Screen::LicenseText; }
    const Preferences& preferences() const { return preferences_; }
    // ME_BACKEND_* loaded by the bootstrap; -1 when native ME is not in use.
    int activeMeBackend() const { return activeMeBackend_; }
    void setActiveMeBackend(int backend) { activeMeBackend_ = backend; }
    size_t settingSelection() const { return navigation_.get<SettingsRoute>().selection; }
    size_t settingsScroll() const { return navigation_.get<SettingsRoute>().scroll; }
    size_t licenseSelection() const { return navigation_.get<LicensesRoute>().selection; }
    const std::string& licenseText() const { return navigation_.get<LicenseTextRoute>().text; }
    int documentScroll() const { return navigation_.get<LicenseTextRoute>().scroll; }
    void setDocumentScrollLimit(int limit) { navigation_.get<LicenseTextRoute>().scrollLimit = limit; }
    size_t coverFiles() const { return coverFiles_; }
    uint64_t coverBytes() const { return coverBytes_; }
    bool clearingCovers() const { return screen() == Screen::Storage && navigation_.get<StorageRoute>().clearing; }
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
    // Comment lists: video comments and reply threads share one design and one set of controls.
    enum class CommentTile { Reply, Like, Replies, More };
    bool commentPage() const;
    int commentSelectedRow() const;
    size_t commentRowCount() const;
    bool commentExpanded() const;
    int commentInner() const;
    void commentSetInner(int line);
    bool commentMenuOpen() const;
    int commentMenuIndex() const;
    std::vector<CommentTile> commentMenuTiles() const;
    void commentMoveSelection(int dy);
    void commentToggle();
    void commentOpenMenu();
    void commentMenuMove(int dx);
    void commentMenuClose();
    void commentMenuActivate();
    void commentQuickLike();
    void commentQuickReplies();
    void beginMessage();
    void submitCommunityInput(const std::string &text);
    bool communityInputPending() const { return communityInputPending_; }
    const std::string &communityDraft() const { return communityDraft_; }
    void cancelCommunityInput() { communityInputPending_ = false; }
    bool hasBrowse() const { return screen() == Screen::Browse; }
    const BrowsePage &browsePage() const { return browseState().page; }
    const AppError &browseError() const { return browseState().error; }
    size_t browseSelection() const { return browseState().selection; }
    int browseScroll() const { return browseState().scroll; }
    size_t selectedComment() const { return commentsState().selected; }
    void setBrowseScrollLimit(int limit) { browseState().scrollLimit = limit; }
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
    SearchView searchView() const { return root().searchView; }
    const std::vector<SearchTopic> &searchTopics() const { return root().searchTopics; }
    const std::string &searchError() const { return root().searchError; }
    void retry();
    void openLogin();
    void refreshLogin();
    void logout();
    const AuthView &auth() const { return authView_; }
    const AppError &authError() const { return authError_; }
    const AppError &error() const { return navigation_.current().error; }
    void setStatus(const std::string &status) { status_ = status; }
    void requestComments();
    void loadMoreComments();
    void scrollComments(int amount);
    void setCommentScrollLimit(int limit) { commentsState().scrollLimit = limit; }
    void toggleLater();
    void requestPlayback(bool fromStart = false);
    bool detailReady() const { return detailState().loaded; }
    int64_t detailStartMs() const { return selectedDetailCard().startMs; }
    void finishPlayback(int error, unsigned frames, int eof, int cancelled,
                        PspVideoStage stage = PSP_VIDEO_STAGE_NONE, int64_t positionMs = 0);
    void seekPlayback(int64_t positionMs, bool paused, int quality = 0);
    // Prepares the current video's wili-server stream again at positionMs, for a seek that keeps the
    // player running. Returns false when it is not a server stream or the server refuses.
    bool reopenServerPlayback(int64_t positionMs, BiliPlayback& out);
    const VideoCard& playbackCard() const { return playbackCard_; }
    PspVideoSettings playerSettings() const { return playerSettings_; }
    void setPlayerSettings(const PspVideoSettings& settings) { playerSettings_ = settings; }
    const std::string& playbackTitle() const { return playbackCard_.title; }
    bool playbackPaused() const { return playbackPaused_; }
    int64_t playbackStartMs() const { return playbackCard_.startMs; }
    bool playbackFailed() const { return navigation_.current().playbackFailed; }

    bool busy() const { return busy_ && (!backgroundJob() || pendingBack_); }
    bool pollingLogin() const { return busy_ && job_ == Job::AuthPoll; }
    bool playbackReady() const { return playbackReady_; }
    BiliPlayback &playback() { return playback_; }
    const BiliPlayback &playback() const { return playback_; }
    const VideoDetailView &detail() const { return detailState().detail; }
    const std::vector<VideoCard> &cards() const;
    const std::vector<CommentItem> &comments() const { return commentsState().items; }
    const std::vector<LocalVideo> &history() const { return history_; }
    const std::vector<LocalVideo> &later() const { return later_; }
    const std::vector<std::string> &searchHistory() const { return searchHistory_; }
    const std::string &status() const { return status_; }
    const std::string &query() const { return root().query; }
    size_t selection() const { return root().selection; }
    size_t detailPart() const { return detailState().part; }
    int commentScroll() const { return commentsState().scroll; }
    bool commentsEnd() const { return commentsState().end; }
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
        HistoryReport,
        ServerFind,
        ServerTest
    };

    // Jobs that never block input. A new foreground job cancels them and takes over.
    bool backgroundJob() const {
        return ((job_ == Job::HistoryReport) || (prefetch_ && !prefetchPromoted_)) &&
               queuedJob_ == Job::None && !coverActive_;
    }
    // Next-page prefetch for video lists: runs between cover downloads, appends silently
    // without moving the selection, and is promoted to a normal load if the user reaches
    // the end first.
    bool startPrefetchIfWanted();
    void finishPrefetch();
    void commitPrefetch(bool promoted);
    void startJob(Job job);
    void startServerFind();
    void startServerTest();
    void commitServerFind();
    void runServerTest();
    void applyServerConfig();
    bool serverStreaming() const { return preferences_.streamServer && preferences_.serverReady(); }
    bool streamTarget(StreamTarget& target, long long startMs) const;
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
    bool hubOpen_ = false;
    int hubSelection_ = 0;
    uint64_t hubLaunchAt_ = 0;  // system time (us) when the launch animation began; 0 = idle
    void openHub();
    int activeMeBackend_ = -1;
    size_t coverFiles_ = 0;
    uint64_t coverBytes_ = 0;
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

    BrowseRequest jobBrowse_;
    // Prefetch requests are private to the worker so foreground actions can keep editing job*_.
    bool prefetch_ = false, prefetchPromoted_ = false;
    int pfPage_ = 1;
    std::string pfQuery_;
    uint64_t pfGeneration_ = 0;
    BrowseRequest pfBrowse_;
    uint64_t prefetchRetryAt_ = 0;
    BrowsePage stagedBrowse_;
    CommunityWrite jobWrite_, jobHistoryWrite_;
    uint64_t jobRouteId_ = 0;
    BrowseItem jobMedia_;
    VideoCard playbackCard_;
    uint64_t playbackEpisode_ = 0, playbackSeason_ = 0;
    ApiResponse stagedWrite_;
    BrowseItem activeComment_;
    bool browseAppending_ = false, communityInputPending_ = false;
    bool communityLoginPending_ = false, dismissLoginAfterCheck_ = false;
    std::string communityDraft_;
    CommunityWrite inputWrite_;
    NavigationStack navigation_;
    RootRoute &root() { return navigation_.get<RootRoute>(); }
    const RootRoute &root() const { return navigation_.get<RootRoute>(); }
    DetailRoute &detailState() { return navigation_.get<DetailRoute>(); }
    const DetailRoute &detailState() const { return navigation_.get<DetailRoute>(); }
    CommentsRoute &commentsState() { return navigation_.get<CommentsRoute>(); }
    const CommentsRoute &commentsState() const { return navigation_.get<CommentsRoute>(); }
    BrowseRoute &browseState() { return navigation_.get<BrowseRoute>(); }
    const BrowseRoute &browseState() const { return navigation_.get<BrowseRoute>(); }
    CommentUi* commentUi();
    const CommentUi* commentUi() const;
    bool selectedCommentData(CommunityComment& comment, BrowseItem& item) const;
    void commentReply(const CommunityComment& comment, const BrowseRequest& request);
    void commentVote(const CommunityComment& comment, const BrowseRequest& request);
    void commentOpenReplies(const CommunityComment& comment, const BrowseRequest& request);
#ifdef BILI_UI_FIXTURE
    void loadUiFixture();
#endif
    void pushBrowse(BrowseRoute page);
    void pushCommentActions(const BrowseItem &item);
    void clearAccountPages();
    void commitCommunityWrite();

    BilibiliApi api_;
    CommunityService communityService_{api_};
    SearchService searchService_{api_};
    SessionStore sessions_{CLIENT_PATH("/session.json")};
    AuthService authService_{api_, sessions_};
    AuthView authView_, stagedAuth_;
    SettingsInput settingsInput_ = SettingsInput::None;
    WiliServer jobServer_{};
    std::vector<WiliFoundServer> stagedFound_;
    std::string stagedServerMessage_;
    AppError jobTypedError_, authError_;
    std::string jobAuthKey_;
    uint64_t authPollAt_ = 0;
    bool sessionCheckPending_ = false;
    bool pendingBack_ = false;
    bool loginRefreshPending_ = false;
    uint64_t searchGeneration_ = 0, jobSearchGeneration_ = 0, stagedSearchGeneration_ = 0;

    int recommendPage_ = 1;
    int hotsPage_ = 1;
    int searchPage_ = 1;
    std::vector<SearchTopic> stagedSearchTopics_;
    bool recommendHasMore_ = true;
    bool hotsHasMore_ = true;
    bool searchHasMore_ = true;
    bool busy_ = false;
    std::string status_ = "Wiliwili C++17 UI · 正在初始化";

    std::vector<VideoCard> recommend_;
    std::vector<VideoCard> hots_;
    std::vector<VideoCard> search_;
    std::vector<LocalVideo> history_;
    std::vector<LocalVideo> later_;
    std::vector<std::string> searchHistory_;
    std::vector<CommentItem> stagedComments_;
    BiliPlayback playback_{};
    bool playbackReady_ = false;
    bool playbackPaused_ = false;
    int playbackQuality_ = 16;
    uint64_t playbackOverlayCid_ = 0;
    PspVideoSettings playerSettings_{16, 1, {16, 0, 0}, 1, 1, 75, 0, 0};

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
