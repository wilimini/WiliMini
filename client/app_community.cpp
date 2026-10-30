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
  const int movement = isPgcPosterPage(frame.page.request.kind) ? dx + dy * 4 : dy;
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
} // namespace pspapp
