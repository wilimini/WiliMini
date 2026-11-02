#include "app.hpp"
#include <algorithm>
#include <cstdio>

namespace pspapp {
namespace {
BrowseItem destination(BrowseKind kind, const std::string &title,
                       const std::string &id = {},
                       const std::string &aux = {}) {
  BrowseItem item;
  item.title = title;
  item.next = {kind, id, aux, {}, title};
  return item;
}
} // namespace
void App::pushBrowse(BrowseRoute page) {
  navigation_.push(std::move(page));
  coverDirty_ = true;
}
void App::openBrowse(BrowseRequest request) {
  if (busy())
    return;
  if (request.kind == BrowseKind::Discover && atTabRoot()) {
    setTab(Tab::Discover);
    return;
  }
  BrowseRoute frame;
  frame.page.request = std::move(request);
  frame.page.title = frame.page.request.title.empty()
                         ? browseTitle(frame.page.request.kind)
                         : frame.page.request.title;
  pushBrowse(std::move(frame));
  browseRefresh();
}
void App::browseRefresh() {
  if (busy() || !hasBrowse())
    return;
  auto &frame = browseState();
  if (frame.page.request.kind == BrowseKind::Text ||
      frame.page.request.kind == BrowseKind::Image ||
      frame.page.request.kind == BrowseKind::CommentActions)
    return;
  jobBrowse_ = frame.page.request;
  jobBrowse_.page = 1;
  jobBrowse_.cursor.clear();
  // aux identifies the root/area/session type; pagination must not overwrite it
  // in the stored route.
  browseAppending_ = false;
  frame.error = {};
  startJob(Job::Browse);
}
void App::browseMore() {
  if (busy() || !hasBrowse() || !browseState().page.hasMore)
    return;
  auto &frame = browseState();
  // The prefetch for this page is still in flight: show the normal loading state and wait.
  if (prefetch_ && !prefetchPromoted_ && job_ == Job::Browse &&
      jobRouteId_ == navigation_.current().id) {
    prefetchPromoted_ = true;
    return;
  }
  jobBrowse_ = frame.page.request;
  ++jobBrowse_.page;
  jobBrowse_.cursor = frame.page.nextCursor;
  if (jobBrowse_.kind == BrowseKind::Notifications ||
      jobBrowse_.kind == BrowseKind::History)
    jobBrowse_.aux = frame.page.nextAux;
  browseAppending_ = true;
  frame.error = {};
  startJob(Job::Browse);
}
void App::browseMove(int dx, int dy) {
  if (!hasBrowse())
    return;
  auto &frame = browseState();
  if (frame.page.request.kind == BrowseKind::Text) {
    frame.scroll =
        std::max(0, std::min(browseState().scrollLimit,
                             frame.scroll +
                                 (dy ? dy : dx) * (preferences_.bodySize + 4)));
    return;
  }
  if (frame.page.request.kind == BrowseKind::Season && dx) {
    for (size_t i = 0; i < frame.page.items.size(); ++i)
      if ((dx > 0 && frame.page.items[i].next.kind == BrowseKind::Text) ||
          (dx < 0 && frame.page.items[i].next.kind == BrowseKind::Episode)) {
        frame.selection = i;
        return;
      }
  }
  const bool videoGrid =
      preferences_.homeStyle == HomeStyle::Tiles && isVideoListPage(frame.page);
  const int movement =
      isPgcPosterPage(frame.page.request.kind) ? dx + dy * 4 : videoGrid ? dx + dy * 2 : dy;
  if (!movement)
    return;
  const size_t count = frame.page.items.size();
  // The final row is an explicit pagination/retry target.
  const size_t total = count + (frame.page.hasMore ? 1 : 0);
  if (total)
    frame.selection = static_cast<size_t>(
        std::max(0, std::min(static_cast<int>(total) - 1,
                             static_cast<int>(frame.selection) + movement)));
  browseState().confirmingDelete = false;
}
void App::browseBack() {
  back();
}
void App::browseActivate() {
  if (busy() || !hasBrowse())
    return;
  auto &frame = browseState();
  if (frame.error.kind == ErrorKind::LoginRequired ||
      frame.error.kind == ErrorKind::SessionExpired) {
    communityLoginPending_ = true;
    openLogin();
    return;
  }
  if (frame.selection >= frame.page.items.size()) {
    browseMore();
    return;
  }
  const BrowseItem item = frame.page.items[frame.selection];
  if (frame.page.request.kind == BrowseKind::ChooseCollection) {
    CommunityWrite w;
    w.action = CommunityAction::Collect;
    w.id = item.next.id;
    w.oid = frame.page.request.id;
    w.undo = item.duration != 0;
    communityWrite(w);
    return;
  }
  if (frame.page.request.kind == BrowseKind::VideoActions) {
    if (item.next.id == "restart") {
      browseBack();
      requestPlayback(true);
      return;
    }
    if (item.next.id == "local-later") {
      toggleLater();
      return;
    }
    if (item.next.id == "cloud-later") {
      CommunityWrite w;
      w.action = frame.page.followed ? CommunityAction::WatchLaterDelete
                                     : CommunityAction::WatchLaterAdd;
      w.id = frame.page.request.id;
      communityWrite(w);
      return;
    }
  }
  if (frame.page.request.kind == BrowseKind::CommentActions)
    activeComment_ = frame.context;
  if (frame.page.request.kind == BrowseKind::CommentActions) {
    if (item.next.id == "read") {
      BrowseRoute text;
      text.page.request = item.next;
      text.page.title = item.title;
      text.page.description = item.text;
      pushBrowse(std::move(text));
      return;
    }
    const auto &c = activeComment_.comment;
    if (frame.selection == 0) {
      auto r = BrowseRequest{BrowseKind::Replies,
                             frame.page.request.aux,
                             c.root.empty() || c.root == "0" ? c.id : c.root,
                             {},
                             "评论回复"};
      r.type = frame.page.request.type;
      openBrowse(r);
    } else if (frame.selection == 1) {
      inputWrite_ = {};
      inputWrite_.action = CommunityAction::AddComment;
      inputWrite_.oid = frame.page.request.aux;
      inputWrite_.type = frame.page.request.type;
      inputWrite_.parent = c.id;
      inputWrite_.root = c.root.empty() || c.root == "0" ? c.id : c.root;
      if (!sessions_.current().valid()) {
        communityLoginPending_ = true;
        openLogin();
        return;
      }
      communityInputPending_ = true;
    } else if (frame.selection == 2 || frame.selection == 3) {
      CommunityWrite w;
      w.action = frame.selection == 2 ? CommunityAction::LikeComment
                                      : CommunityAction::HateComment;
      w.oid = frame.page.request.aux;
      w.id = c.id;
      w.type = frame.page.request.type;
      w.undo = c.action == (frame.selection == 2 ? 1 : 2);
      communityWrite(w);
    } else if (frame.selection == 4)
      openBrowse(item.next);
    else {
      if (!browseState().confirmingDelete) {
        browseState().confirmingDelete = true;
        status_ = "再次按 " + std::string(preferences_.confirmButton()) + " 删除这条评论，" +
                  preferences_.backButton() + " 取消";
        return;
      }
      CommunityWrite w;
      w.action = CommunityAction::DeleteComment;
      w.oid = frame.page.request.aux;
      w.id = c.id;
      w.type = frame.page.request.type;
      communityWrite(w);
    }
    return;
  }
  if (item.next.kind == BrowseKind::Video) {
    VideoCard card;
    card.bvid = item.bvid;
    card.aid = item.aid;
    card.cid = item.cid;
    card.title = item.title;
    card.author = item.subtitle;
    card.picture = item.picture;
    card.duration = item.duration;
    card.startMs = item.startMs;
    // Notifications can identify a video by aid. Resolve that through the
    // detail API too.
    if (card.bvid.empty() && card.aid)
      card.bvid = "av" + std::to_string(card.aid);
    if (!card.bvid.empty())
      startDetail(card);
    return;
  }
  if (item.next.kind == BrowseKind::CommentActions) {
    pushCommentActions(item);
    return;
  }
  if (item.next.kind == BrowseKind::Image) {
    BrowseRoute image;
    image.page.request = item.next;
    image.page.title = item.title;
    image.context = item;
    image.page.description = "community_image_" + item.next.id;
    pushBrowse(std::move(image));
    return;
  }
  if (item.next.kind == BrowseKind::Text) {
    BrowseRoute text;
    text.page.request = item.next;
    text.page.title = item.title;
    text.page.description = item.text;
    pushBrowse(std::move(text));
    return;
  }
  if (item.next.kind == BrowseKind::Episode ||
      item.next.kind == BrowseKind::LiveStream) {
    jobMedia_ = item;
    playbackPaused_ = false;
    startJob(Job::CommunityPlay);
    return;
  }
  openBrowse(item.next);
}
void App::pushCommentActions(const BrowseItem &item) {
  activeComment_ = item;
  BrowseRoute actions;
  actions.page.request = item.next;
  actions.page.title = "评论操作";
  actions.context = item;
  actions.page.description =
      item.comment.author + "：" + item.comment.message;
  actions.page.items = {
      destination(BrowseKind::Replies, "查看回复"),
      destination(BrowseKind::Text, "回复评论"),
      destination(BrowseKind::Text,
                  item.comment.action == 1 ? "取消点赞" : "点赞"),
      destination(BrowseKind::Text,
                  item.comment.action == 2 ? "取消点踩" : "点踩"),
      destination(BrowseKind::Space, "作者空间", item.comment.mid)};
  if (item.comment.mid == sessions_.current().mid)
    actions.page.items.push_back(
        destination(BrowseKind::Text, "删除我的评论"));
  auto read = destination(BrowseKind::Text, "阅读评论全文", "read");
  read.text = item.comment.message;
  actions.page.items.push_back(std::move(read));
  pushBrowse(std::move(actions));
}
void App::commentActions() {
  if (busy() || commentsState().items.empty())
    return;
  commentsState().selected = std::min(commentsState().selected, commentsState().items.size() - 1);
  const auto &c = commentsState().items[commentsState().selected];
  BrowseItem item;
  item.comment = {std::to_string(c.id),
                  c.root,
                  c.parent,
                  c.mid,
                  c.author,
                  c.message,
                  c.like,
                  c.replies,
                  c.action};
  item.next = {BrowseKind::CommentActions,
               std::to_string(c.id),
               std::to_string(detailState().detail.card.aid),
               {},
               "评论操作"};
  pushCommentActions(item);
}
void App::selectComment(int direction) {
  if (commentsState().items.empty())
    return;
  commentsState().selected = static_cast<size_t>(
      std::max(0, std::min(static_cast<int>(commentsState().items.size()) - 1,
                           static_cast<int>(commentsState().selected) + direction)));
}
void App::beginComment() {
  if (busy())
    return;
  inputWrite_ = {};
  inputWrite_.action = CommunityAction::AddComment;
  if (screen() == Screen::Comments)
    inputWrite_.oid = std::to_string(detailState().detail.card.aid);
  else if (hasBrowse()) {
    const auto &r = browseState().page.request;
    if (r.kind != BrowseKind::Comments && r.kind != BrowseKind::Replies)
      return;
    inputWrite_.oid = r.id;
    inputWrite_.type = r.type;
    if (r.kind == BrowseKind::Replies) {
      inputWrite_.root = r.aux;
      inputWrite_.parent = r.aux;
    }
  } else
    return;
  if (!sessions_.current().valid()) {
    communityLoginPending_ = true;
    openLogin();
    return;
  }
  communityInputPending_ = true;
}
void App::beginMessage() {
  if (busy() || !hasBrowse() ||
      browseState().page.request.kind != BrowseKind::Conversation)
    return;
  inputWrite_ = {};
  inputWrite_.action = CommunityAction::SendMessage;
  inputWrite_.id = browseState().page.request.id;
  if (!sessions_.current().valid()) {
    communityLoginPending_ = true;
    openLogin();
    return;
  }
  communityInputPending_ = true;
}
void App::submitCommunityInput(const std::string &text) {
  communityInputPending_ = false;
  if (text.empty())
    return;
  communityDraft_ = text;
  inputWrite_.text = text;
  communityWrite(inputWrite_);
}
void App::communityWrite(CommunityWrite request) {
  if (busy())
    return;
  if (!sessions_.current().valid()) {
    communityLoginPending_ = true;
    openLogin();
    return;
  }
  jobWrite_ = std::move(request);
  startJob(Job::CommunityWrite);
}
void App::openVideoActions() {
  if (screen() == Screen::Detail && detailState().detail.card.aid)
    openBrowse({BrowseKind::VideoActions, std::to_string(detailState().detail.card.aid)});
}
void App::communityShortcut() {
  if (screen() == Screen::Mine) {
    openBrowse({BrowseKind::Personal});
    return;
  }
  if (hasBrowse()) {
    const auto kind = browseState().page.request.kind;
    if (kind == BrowseKind::DynamicDetail) {
      CommunityWrite w;
      w.action = CommunityAction::LikeDynamic;
      w.id = browseState().page.request.id;
      w.undo = browseState().page.followed;
      communityWrite(w);
    } else if (kind == BrowseKind::Conversation)
      beginMessage();
    else if (kind == BrowseKind::Comments || kind == BrowseKind::Replies)
      beginComment();
    else if (kind == BrowseKind::Space || kind == BrowseKind::Season) {
      CommunityWrite w;
      w.action = kind == BrowseKind::Space ? CommunityAction::Follow
                                           : CommunityAction::FollowSeason;
      w.id = browseState().page.request.id;
      w.undo = browseState().page.followed;
      communityWrite(w);
    } else if (kind == BrowseKind::WatchLater &&
               browseState().selection <
                   browseState().page.items.size()) {
      CommunityWrite w;
      w.action = CommunityAction::WatchLaterDelete;
      w.id = std::to_string(
          browseState().page.items[browseState().selection].aid);
      communityWrite(w);
    }
  }
}
void App::clearAccountPages() {
  // Drop private page instances as well as their navigation history.
  const bool login = screen() == Screen::Login;
  const bool hadBrowse = navigation_.find<BrowseRoute>() != nullptr;
  navigation_.clearChildren();
  if (hadBrowse) {
    root().tab = Tab::Mine;
    root().selection = 0;
  }
  navigation_.current().focus = FocusArea::Content;
  if (login) navigation_.push<LoginRoute>();
  communityDraft_.clear();
  communityInputPending_ = false;
  communityLoginPending_ = false;
  activeComment_ = {};
  jobWrite_ = {};
  inputWrite_ = {};
}
void App::commitCommunityWrite() {
  communityDraft_.clear();
  if (hasBrowse()) browseState().confirmingDelete = false;
  auto update = [&](CommunityComment &c) {
    if (c.id != jobWrite_.id)
      return;
    if (jobWrite_.action == CommunityAction::LikeComment ||
        jobWrite_.action == CommunityAction::HateComment)
      updateCommentVote(c.likes, c.action,
                        jobWrite_.action == CommunityAction::LikeComment,
                        jobWrite_.undo);
  };
  update(activeComment_.comment);
  for (auto &entry : navigation_.entries()) {
    auto *page = std::get_if<BrowseRoute>(&entry.state);
    if (!page) continue;
    auto &frame = *page;
    update(frame.context.comment);
    for (auto &item : frame.page.items) {
      update(item.comment);
      if (!item.comment.id.empty())
        item.subtitle = std::to_string(item.comment.likes) + " 赞 · " +
                        std::to_string(item.comment.replies) + " 回复";
    }
    if (jobWrite_.action == CommunityAction::DeleteComment)
      frame.page.items.erase(
          std::remove_if(frame.page.items.begin(), frame.page.items.end(),
                         [&](const BrowseItem &item) {
                           return item.comment.id == jobWrite_.id;
                         }),
          frame.page.items.end());
  }
  for (auto &entry : navigation_.entries()) {
    auto *page = std::get_if<CommentsRoute>(&entry.state);
    if (!page) continue;
    for (auto &c : page->items)
      if (std::to_string(c.id) == jobWrite_.id &&
          (jobWrite_.action == CommunityAction::LikeComment ||
           jobWrite_.action == CommunityAction::HateComment))
        updateCommentVote(c.like, c.action,
                          jobWrite_.action == CommunityAction::LikeComment,
                          jobWrite_.undo);
    if (jobWrite_.action == CommunityAction::DeleteComment)
      page->items.erase(std::remove_if(page->items.begin(), page->items.end(),
                         [&](const CommentItem &c) {
                           return std::to_string(c.id) == jobWrite_.id;
                         }), page->items.end());
  }
  if (jobWrite_.action == CommunityAction::DeleteComment) {
    if (screen() == Screen::Browse)
      browseBack();
  } else if (jobWrite_.action == CommunityAction::AddComment ||
             jobWrite_.action == CommunityAction::SendMessage) {
    if (screen() == Screen::Comments)
      requestComments();
    else if (screen() == Screen::Browse) {
      if (browseState().page.request.kind == BrowseKind::CommentActions)
        browseBack();
      if (screen() == Screen::Browse)
        browseRefresh();
      else if (screen() == Screen::Comments)
        requestComments();
    }
  } else if (hasBrowse() &&
             browseState().page.request.kind ==
                 BrowseKind::CommentActions) {
    browseState().context = activeComment_;
    auto &items = browseState().page.items;
    items[2].title = activeComment_.comment.action == 1 ? "取消点赞" : "点赞";
    items[3].title = activeComment_.comment.action == 2 ? "取消点踩" : "点踩";
  }
  if (jobWrite_.action == CommunityAction::LikeDynamic ||
      jobWrite_.action == CommunityAction::Follow ||
      jobWrite_.action == CommunityAction::FollowSeason ||
      jobWrite_.action == CommunityAction::Collect ||
      jobWrite_.action == CommunityAction::WatchLaterAdd ||
      jobWrite_.action == CommunityAction::WatchLaterDelete) {
    if (screen() == Screen::Browse)
      browseRefresh();
  }
  status_ = "操作成功";
}

// ---- Comment lists ---------------------------------------------------------------------------
// Video comments and reply threads: up/down move between comments, confirm expands the selected
// one in place, the menu is a card over the list, and left/right are quick like / reply list.
CommentUi *App::commentUi() {
  if (screen() == Screen::Comments)
    return &commentsState().ui;
  if (hasBrowse()) {
    const auto kind = browseState().page.request.kind;
    if (kind == BrowseKind::Comments || kind == BrowseKind::Replies)
      return &browseState().ui;
  }
  return nullptr;
}
const CommentUi *App::commentUi() const {
  return const_cast<App *>(this)->commentUi();
}
bool App::commentPage() const { return commentUi() != nullptr; }
int App::commentSelectedRow() const {
  if (screen() == Screen::Comments)
    return static_cast<int>(commentsState().selected);
  return static_cast<int>(browseState().selection);
}
size_t App::commentRowCount() const {
  if (screen() == Screen::Comments)
    return commentsState().items.size();
  return browseState().page.items.size() + (browseState().page.hasMore ? 1 : 0);
}
bool App::commentExpanded() const {
  const auto *ui = commentUi();
  return ui && ui->expanded;
}
int App::commentInner() const {
  const auto *ui = commentUi();
  return ui ? ui->inner : 0;
}
void App::commentSetInner(int line) {
  if (auto *ui = commentUi())
    ui->inner = std::max(0, line);
}
bool App::commentMenuOpen() const {
  const auto *ui = commentUi();
  return ui && ui->menuOpen;
}
int App::commentMenuIndex() const {
  const auto *ui = commentUi();
  return ui ? ui->menuIndex : 0;
}
bool App::selectedCommentData(CommunityComment &comment, BrowseItem &item) const {
  if (screen() == Screen::Comments) {
    const auto &items = commentsState().items;
    if (items.empty())
      return false;
    const auto &c = items[std::min(commentsState().selected, items.size() - 1)];
    comment = {std::to_string(c.id), c.root, c.parent, c.mid, c.author,
               c.message, c.like, c.replies, c.action};
    item = {};
    item.comment = comment;
    item.next = {BrowseKind::CommentActions, std::to_string(c.id),
                 std::to_string(detailState().detail.card.aid), {}, "评论操作"};
    return true;
  }
  if (!commentUi())
    return false;
  const auto &frame = browseState();
  if (frame.selection >= frame.page.items.size() ||
      frame.page.items[frame.selection].comment.id.empty())
    return false;
  item = frame.page.items[frame.selection];
  comment = item.comment;
  return true;
}
std::vector<App::CommentTile> App::commentMenuTiles() const {
  std::vector<CommentTile> tiles = {CommentTile::Reply, CommentTile::Like};
  CommunityComment c;
  BrowseItem item;
  const bool thread = hasBrowse() && browseState().page.request.kind == BrowseKind::Replies;
  if (selectedCommentData(c, item) && c.replies > 0 && !thread)
    tiles.push_back(CommentTile::Replies);
  tiles.push_back(CommentTile::More);
  return tiles;
}
void App::commentMoveSelection(int dy) {
  auto *ui = commentUi();
  if (!ui)
    return;
  *ui = {};
  if (screen() == Screen::Comments) {
    auto &state = commentsState();
    if (state.items.empty())
      return;
    const int last = static_cast<int>(state.items.size()) - 1;
    const int value = std::max(0, std::min(last, static_cast<int>(state.selected) + dy));
    state.selected = static_cast<size_t>(value);
    // Reaching the last comment fetches the next page, so there is no separate button for it.
    if (dy > 0 && value == last)
      loadMoreComments();
  } else {
    browseMove(0, dy);
  }
}
void App::commentToggle() {
  auto *ui = commentUi();
  if (!ui || busy())
    return;
  if (screen() == Screen::Browse && browseState().selection >= browseState().page.items.size()) {
    browseMore();
    return;
  }
  CommunityComment c;
  BrowseItem item;
  if (!selectedCommentData(c, item))
    return;
  ui->expanded = !ui->expanded;
  ui->inner = 0;
}
void App::commentOpenMenu() {
  auto *ui = commentUi();
  CommunityComment c;
  BrowseItem item;
  if (!ui || busy() || !selectedCommentData(c, item))
    return;
  ui->menuOpen = true;
  ui->menuIndex = 0;
}
void App::commentMenuMove(int dx) {
  auto *ui = commentUi();
  if (!ui || !ui->menuOpen)
    return;
  const int count = static_cast<int>(commentMenuTiles().size());
  ui->menuIndex = std::max(0, std::min(count - 1, ui->menuIndex + dx));
}
void App::commentMenuClose() {
  if (auto *ui = commentUi())
    ui->menuOpen = false;
}
void App::commentReply(const CommunityComment &c, const BrowseRequest &request) {
  inputWrite_ = {};
  inputWrite_.action = CommunityAction::AddComment;
  inputWrite_.oid = request.aux;
  inputWrite_.type = request.type;
  inputWrite_.parent = c.id;
  inputWrite_.root = c.root.empty() || c.root == "0" ? c.id : c.root;
  if (!sessions_.current().valid()) {
    communityLoginPending_ = true;
    openLogin();
    return;
  }
  communityInputPending_ = true;
}
void App::commentVote(const CommunityComment &c, const BrowseRequest &request) {
  CommunityWrite w;
  w.action = CommunityAction::LikeComment;
  w.oid = request.aux;
  w.id = c.id;
  w.type = request.type;
  w.undo = c.action == 1;
  communityWrite(w);
}
void App::commentOpenReplies(const CommunityComment &c, const BrowseRequest &request) {
  auto r = BrowseRequest{BrowseKind::Replies, request.aux,
                         c.root.empty() || c.root == "0" ? c.id : c.root, {}, "评论回复"};
  r.type = request.type;
  openBrowse(r);
}
void App::commentMenuActivate() {
  auto *ui = commentUi();
  if (!ui || !ui->menuOpen || busy())
    return;
  const auto tiles = commentMenuTiles();
  CommunityComment c;
  BrowseItem item;
  if (!selectedCommentData(c, item) || tiles.empty())
    return;
  const auto tile = tiles[static_cast<size_t>(std::min<int>(ui->menuIndex, static_cast<int>(tiles.size()) - 1))];
  ui->menuOpen = false;
  switch (tile) {
  case CommentTile::Reply: commentReply(c, item.next); break;
  case CommentTile::Like: commentVote(c, item.next); break;
  case CommentTile::Replies: commentOpenReplies(c, item.next); break;
  case CommentTile::More: pushCommentActions(item); break;
  }
}
void App::commentQuickLike() {
  CommunityComment c;
  BrowseItem item;
  if (!busy() && commentPage() && selectedCommentData(c, item))
    commentVote(c, item.next);
}
void App::commentQuickReplies() {
  CommunityComment c;
  BrowseItem item;
  const bool thread = hasBrowse() && browseState().page.request.kind == BrowseKind::Replies;
  if (!busy() && commentPage() && !thread && selectedCommentData(c, item) && c.replies > 0)
    commentOpenReplies(c, item.next);
}

#ifdef BILI_UI_FIXTURE
// Diagnostic build only (-DBILI_UI_FIXTURE=ON): start on a comment page filled with sample data so
// the layouts can be captured in the emulator without an account or network. The file
// ui-fixture.txt in the package directory selects "comments" or "replies".
void App::loadUiFixture() {
  char mode[16]{};
  if (FILE *file = std::fopen(CLIENT_PATH("/ui-fixture.txt"), "rb")) {
    std::fread(mode, 1, sizeof(mode) - 1, file);
    std::fclose(file);
  }
  const bool replies = std::string(mode).rfind("replies", 0) == 0;
  if (!replies && std::string(mode).rfind("comments", 0) != 0)
    return;
  DetailRoute detail;
  detail.loaded = true;
  detail.detail.card.bvid = "BV1fixture00";
  detail.detail.card.aid = 1001;
  detail.detail.card.title = "Building a Bilibili client for 32 MB of RAM";
  detail.detail.card.author = "TinyCircuits Lab";
  navigation_.push(std::move(detail));
  struct Sample { const char *author, *time, *text; int like, replies; };
  static const Sample samples[] = {
      {"像素老炮", "2小时前", "这个 PSP 播放器真的做出来了，480×272 的小屏幕上还能放弹幕，当年想都不敢想。", 12034, 3},
      {"Synth_Lin", "5小时前", "看了三遍。网络、界面渲染、解码全部跑在掌机上，不是串流，这一点最厉害。作者能不能再讲讲 Media Engine 的缓存一致性问题，那一段我没完全看懂，特别是 DMA 和 VFPU 状态怎么处理。", 3481, 12},
      {"小林同学", "昨天", "磁贴界面太好看了，一下回到 Windows Phone 的年代。", 864, 0},
      {"Retro_Bench", "昨天", "3000 上很流畅。", 207, 1},
      {"掌机考古学家", "2天前", "第一次在 PSP 上看到这么顺的列表滚动，焦点环的滑动很舒服，希望以后能支持自定义强调色。", 95, 0},
      {"Dot_Painter", "3天前", "封面加载速度不错，缓存策略有写文档吗？", 41, 2},
  };
  if (replies) {
    BrowseRoute thread;
    thread.page.request = BrowseRequest{BrowseKind::Replies, "1001", "5001", {}, "评论回复"};
    thread.page.title = "评论回复";
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
      BrowseItem item;
      item.title = samples[i].author;
      item.text = samples[i].text;
      item.next = {BrowseKind::CommentActions, std::to_string(7000 + i), "1001", {}, "评论操作"};
      item.comment = {std::to_string(7000 + i), "5001", "5001", "1", samples[i].author, samples[i].text,
                      samples[i].like, 0, 0};
      thread.page.items.push_back(std::move(item));
    }
    thread.page.hasMore = true;
    pushBrowse(std::move(thread));
    return;
  }
  CommentsRoute list;
  list.end = false;
  list.next = 2;
  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    CommentItem item;
    item.id = 5000 + i;
    item.author = samples[i].author;
    item.time = samples[i].time;
    item.message = samples[i].text;
    item.like = samples[i].like;
    item.replies = samples[i].replies;
    item.top = i == 0;
    item.uploader = i == 1;
    item.mid = std::to_string(100 + i);
    item.root = "0";
    item.parent = "0";
    if (i == 1) {
      item.preview.push_back({"掌机考古学家", "Media Engine 的部分在文档里写了，缓存刷新是关键。"});
      item.preview.push_back({"像素老炮", "同问，求作者展开讲讲。"});
    }
    list.items.push_back(std::move(item));
  }
  list.selected = 1;
  navigation_.push(std::move(list));
}
#endif

} // namespace pspapp
