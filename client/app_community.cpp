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
void App::cacheBrowseCovers(const BrowsePage &page) {
#ifndef BILI_DEVICE_OFFLINE_UI
  if (!preferences_.showCovers) return;
  for (const auto &item : page.items) {
    if (transfer_cancel) break;
    const auto key = pgcCoverKey(item);
    if (key.empty() || item.picture.empty()) continue;
    const std::string path = std::string(CLIENT_ROOT) + "/cache/" + key + ".jpg";
    if (FILE *cached = std::fopen(path.c_str(), "rb")) {
      std::fseek(cached, 0, SEEK_END);
      const auto size = std::ftell(cached);
      std::fclose(cached);
      if (size > 512) continue;
    }
    auto url = item.picture;
    if (url.rfind("//", 0) == 0) url = "https:" + url;
    const auto suffix = url.find('@');
    if (suffix != std::string::npos) url.resize(suffix);
    url += item.next.kind == BrowseKind::Season ? "@204w_272h_1c.jpg" : "@196w_110h_1c.jpg";
    bili_http_download(url.c_str(), path.c_str(), 160 * 1024);
  }
#else
  (void)page;
#endif
}
void App::openBrowse(BrowseRequest request) {
  if (busy_)
    return;
  if (screen_ != Screen::Browse) {
    browseReturnScreen_ = screen_;
    browseStack_.clear();
  }
  BrowseFrame frame;
  frame.page.request = std::move(request);
  frame.page.title = frame.page.request.title.empty()
                         ? browseTitle(frame.page.request.kind)
                         : frame.page.request.title;
  browseStack_.push_back(std::move(frame));
  screen_ = Screen::Browse;
  focusArea_ = FocusArea::Content;
  browseRefresh();
}
void App::browseRefresh() {
  if (busy_ || browseStack_.empty())
    return;
  auto &frame = browseStack_.back();
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
  if (busy_ || browseStack_.empty() || !browseStack_.back().page.hasMore)
    return;
  auto &frame = browseStack_.back();
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
  if (browseStack_.empty())
    return;
  auto &frame = browseStack_.back();
  if (frame.page.request.kind == BrowseKind::Text) {
    frame.scroll =
        std::max(0, std::min(browseScrollLimit_,
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
  confirmingDelete_ = false;
}
void App::browseBack() {
  if (busy_) {
    transfer_cancel = 1;
    return;
  }
  communityInputPending_ = false;
  confirmingDelete_ = false;
  if (browseStack_.size() > 1)
    browseStack_.pop_back();
  else {
    browseStack_.clear();
    screen_ = browseReturnScreen_;
    if (tab_ == Tab::Discover) {
      tab_ = Tab::Recommend;
      screen_ = Screen::Home;
    }
  }
  status_ = "返回";
}
void App::browseActivate() {
  if (busy_ || browseStack_.empty())
    return;
  auto &frame = browseStack_.back();
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
      BrowseFrame text;
      text.page.request = item.next;
      text.page.title = item.title;
      text.page.description = item.text;
      browseStack_.push_back(std::move(text));
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
      if (!confirmingDelete_) {
        confirmingDelete_ = true;
        status_ = "再次按 × 删除这条评论，○ 取消";
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
    activeComment_ = item;
    BrowseFrame actions;
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
    browseStack_.push_back(std::move(actions));
    return;
  }
  if (item.next.kind == BrowseKind::Image) {
    BrowseFrame image;
    image.page.request = item.next;
    image.page.title = item.title;
    image.context = item;
    image.page.description = "community_image_" + item.next.id;
    browseStack_.push_back(std::move(image));
    return;
  }
  if (item.next.kind == BrowseKind::Text) {
    BrowseFrame text;
    text.page.request = item.next;
    text.page.title = item.title;
    text.page.description = item.text;
    browseStack_.push_back(std::move(text));
    return;
  }
  if (item.next.kind == BrowseKind::Episode ||
      item.next.kind == BrowseKind::LiveStream) {
#ifdef BILI_DEVICE_OFFLINE_UI
    status_ = "离线 UI 构建：播放不可用";
#else
    jobMedia_ = item;
    startJob(Job::CommunityPlay);
#endif
    return;
  }
  openBrowse(item.next);
}
void App::commentActions() {
  if (busy_ || comments_.empty())
    return;
  selectedComment_ = std::min(selectedComment_, comments_.size() - 1);
  const auto &c = comments_[selectedComment_];
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
               std::to_string(detail_.card.aid),
               {},
               "评论操作"};
  browseReturnScreen_ = Screen::Comments;
  browseStack_.clear();
  BrowseFrame parent;
  parent.page.items = {item};
  browseStack_.push_back(std::move(parent));
  screen_ = Screen::Browse;
  browseActivate();
  // The existing comments screen owns its own list/scroll state.
  browseStack_.erase(browseStack_.begin());
}
void App::selectComment(int direction) {
  if (comments_.empty())
    return;
  selectedComment_ = static_cast<size_t>(
      std::max(0, std::min(static_cast<int>(comments_.size()) - 1,
                           static_cast<int>(selectedComment_) + direction)));
}
void App::beginComment() {
  if (busy_)
    return;
  inputWrite_ = {};
  inputWrite_.action = CommunityAction::AddComment;
  if (screen_ == Screen::Comments)
    inputWrite_.oid = std::to_string(detail_.card.aid);
  else if (screen_ == Screen::Browse && !browseStack_.empty()) {
    const auto &r = browseStack_.back().page.request;
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
  if (busy_ || browseStack_.empty() ||
      browseStack_.back().page.request.kind != BrowseKind::Conversation)
    return;
  inputWrite_ = {};
  inputWrite_.action = CommunityAction::SendMessage;
  inputWrite_.id = browseStack_.back().page.request.id;
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
  if (busy_)
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
  if (screen_ == Screen::Detail && detail_.card.aid)
    openBrowse({BrowseKind::VideoActions, std::to_string(detail_.card.aid)});
}
void App::communityShortcut() {
  if (screen_ == Screen::Mine) {
    openBrowse({BrowseKind::Personal});
    return;
  }
  if (screen_ == Screen::Browse && !browseStack_.empty()) {
    const auto kind = browseStack_.back().page.request.kind;
    if (kind == BrowseKind::DynamicDetail) {
      CommunityWrite w;
      w.action = CommunityAction::LikeDynamic;
      w.id = browseStack_.back().page.request.id;
      w.undo = browseStack_.back().page.followed;
      communityWrite(w);
    } else if (kind == BrowseKind::Conversation)
      beginMessage();
    else if (kind == BrowseKind::Comments || kind == BrowseKind::Replies)
      beginComment();
    else if (kind == BrowseKind::Space || kind == BrowseKind::Season) {
      CommunityWrite w;
      w.action = kind == BrowseKind::Space ? CommunityAction::Follow
                                           : CommunityAction::FollowSeason;
      w.id = browseStack_.back().page.request.id;
      w.undo = browseStack_.back().page.followed;
      communityWrite(w);
    } else if (kind == BrowseKind::WatchLater &&
               browseStack_.back().selection <
                   browseStack_.back().page.items.size()) {
      CommunityWrite w;
      w.action = CommunityAction::WatchLaterDelete;
      w.id = std::to_string(
          browseStack_.back().page.items[browseStack_.back().selection].aid);
      communityWrite(w);
    }
  }
}
void App::clearAccountPages() {
  // Account pages stay in memory only; none are persisted into public caches.
  browseStack_.clear();
  communityDraft_.clear();
  communityInputPending_ = false;
  communityLoginPending_ = false;
  activeComment_ = {};
  jobWrite_ = {};
  inputWrite_ = {};
  if (screen_ == Screen::Browse)
    screen_ = Screen::Mine;
  if (detailReturnScreen_ == Screen::Browse)
    detailReturnScreen_ = Screen::Mine;
}
void App::commitCommunityWrite() {
  communityDraft_.clear();
  confirmingDelete_ = false;
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
  for (auto &frame : browseStack_) {
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
  for (auto &c : comments_)
    if (std::to_string(c.id) == jobWrite_.id) {
      if (jobWrite_.action == CommunityAction::LikeComment ||
          jobWrite_.action == CommunityAction::HateComment)
        updateCommentVote(c.like, c.action,
                          jobWrite_.action == CommunityAction::LikeComment,
                          jobWrite_.undo);
    }
  if (jobWrite_.action == CommunityAction::DeleteComment) {
    comments_.erase(std::remove_if(comments_.begin(), comments_.end(),
                                   [&](const CommentItem &c) {
                                     return std::to_string(c.id) ==
                                            jobWrite_.id;
                                   }),
                    comments_.end());
    if (screen_ == Screen::Browse)
      browseBack();
  } else if (jobWrite_.action == CommunityAction::AddComment ||
             jobWrite_.action == CommunityAction::SendMessage) {
    if (screen_ == Screen::Comments)
      requestComments();
    else if (screen_ == Screen::Browse) {
      if (browseStack_.back().page.request.kind == BrowseKind::CommentActions)
        browseBack();
      if (screen_ == Screen::Browse)
        browseRefresh();
      else if (screen_ == Screen::Comments)
        requestComments();
    }
  } else if (screen_ == Screen::Browse && !browseStack_.empty() &&
             browseStack_.back().page.request.kind ==
                 BrowseKind::CommentActions) {
    browseStack_.back().context = activeComment_;
    auto &items = browseStack_.back().page.items;
    items[2].title = activeComment_.comment.action == 1 ? "取消点赞" : "点赞";
    items[3].title = activeComment_.comment.action == 2 ? "取消点踩" : "点踩";
  }
  if (jobWrite_.action == CommunityAction::LikeDynamic ||
      jobWrite_.action == CommunityAction::Follow ||
      jobWrite_.action == CommunityAction::FollowSeason ||
      jobWrite_.action == CommunityAction::Collect ||
      jobWrite_.action == CommunityAction::WatchLaterAdd ||
      jobWrite_.action == CommunityAction::WatchLaterDelete) {
    if (screen_ == Screen::Browse)
      browseRefresh();
  }
  status_ = "操作成功";
}
} // namespace pspapp
