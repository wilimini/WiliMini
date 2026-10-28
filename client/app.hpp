#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <pspkernel.h>

#include "bilibili.h"
#include "navigation.hpp"
#include "preferences.hpp"
#include "services/auth_service.hpp"
#include "services/search_service.hpp"
#include "services/community_service.hpp"

namespace pspapp {

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
    void changeSetting();
    void updateStorage();
    bool utilityPage() const { return screen() >= Screen::Settings && screen() <= Screen::LicenseText; }
    const Preferences& preferences() const { return preferences_; }
    size_t settingSelection() const { return navigation_.get<SettingsRoute>().selection; }
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
        HistoryReport
    };

    bool backgroundJob() const {
        return job_ == Job::HistoryReport && queuedJob_ == Job::None && !coverActive_;
    }
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
