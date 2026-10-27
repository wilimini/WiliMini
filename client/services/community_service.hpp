#pragma once
#include "bilibili_api.hpp"
#include "session_store.hpp"

namespace pspapp {
// Destinations mirror Wiliwili's home, dynamic, mine, inbox and comment pages.
enum class BrowseKind {
  Discover,
  Personal,
  Inbox,
  DynamicFeed,
  DynamicDetail,
  Space,
  SpaceVideos,
  Notifications,
  Sessions,
  Conversation,
  Bangumi,
  Cinema,
  Season,
  LiveAreas,
  LiveRooms,
  LiveRoom,
  History,
  WatchLater,
  Collections,
  Collection,
  Following,
  FollowBangumi,
  FollowCinema,
  Comments,
  Replies,
  CommentActions,
  Text,
  Video,
  Episode,
  LiveStream,
  Image,
  VideoActions,
  ChooseCollection
};
struct BrowseRequest {
  BrowseKind kind = BrowseKind::Discover;
  std::string id, aux, cursor, title;
  int page = 1;
  int type = 1; // Bilibili comment object type; video=1, dynamic=11/17.
  BrowseRequest() = default;
  BrowseRequest(BrowseKind destination, std::string object = {},
                std::string extra = {}, std::string offset = {},
                std::string label = {}, int index = 1, int objectType = 1)
      : kind(destination), id(std::move(object)), aux(std::move(extra)),
        cursor(std::move(offset)), title(std::move(label)), page(index),
        type(objectType) {}
};
struct CommunityComment {
  std::string id, root, parent, mid, author, message;
  int likes = 0, replies = 0, action = 0;
};
struct BrowseItem {
  std::string key, title, subtitle, text, picture, bvid, mid;
  uint64_t aid = 0, cid = 0, epId = 0;
  int duration = 0;
  int64_t startMs = 0;
  uint64_t seasonId = 0;
  bool completed = false;
  BrowseRequest next;
  CommunityComment comment;
};
struct BrowsePage {
  BrowseRequest request;
  std::string title, description, nextCursor, nextAux, subtitle;
  std::vector<BrowseItem> items;
  bool hasMore = false;
  bool followed = false;
};
enum class CommunityAction {
  LikeDynamic,
  LikeComment,
  HateComment,
  DeleteComment,
  AddComment,
  SendMessage,
  Follow,
  FollowSeason,
  WatchLaterAdd,
  WatchLaterDelete,
  Collect,
  Acknowledge,
  ReportHistory
};
struct CommunityWrite {
  CommunityAction action = CommunityAction::AddComment;
  std::string oid, id, root, parent, text;
  int type = 1;
  bool undo = false;
  uint64_t aid = 0, cid = 0, seasonId = 0, episodeId = 0;
  int progress = 0, duration = 0;
};
struct PlaybackProgress {
  uint64_t cid = 0;
  int64_t positionMs = 0;
};
class CommunityService {
public:
  explicit CommunityService(BilibiliApi &api) : api_(api) {}
  Result<BrowsePage> load(const BrowseRequest &request, const Session &session);
  Result<ApiResponse> write(const CommunityWrite &request,
                            const Session &session);
  Result<PlaybackProgress> videoProgress(const std::string &bvid, uint64_t cid);
  static CommunityComment comment(const nlohmann::json &value);

private:
  BilibiliApi &api_;
};
const char *browseTitle(BrowseKind kind);
bool browseNeedsLogin(BrowseKind kind);
void updateCommentVote(int &likes, int &action, bool like, bool undo);
bool isPgcPosterPage(BrowseKind kind);
std::string pgcCoverKey(const BrowseItem &item);
} // namespace pspapp
