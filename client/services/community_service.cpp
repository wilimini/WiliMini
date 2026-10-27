#include "community_service.hpp"
#include "../api/wiliwili/bilibili/api.h"
#include <algorithm>
#include <ctime>
#include <stdexcept>

namespace pspapp {
using Json = nlohmann::json;
using namespace bilibili;
namespace {
std::string str(const Json &j, const char *key,
                const std::string &fallback = {}) {
  if (!j.is_object() || !j.contains(key) || j.at(key).is_null())
    return fallback;
  const auto &v = j.at(key);
  if (v.is_string())
    return v.get<std::string>();
  if (v.is_number_integer())
    return v.dump();
  return fallback;
}
const Json &obj(const Json &j, const char *key) {
  static const Json empty = Json::object();
  return j.is_object() && j.contains(key) && !j.at(key).is_null() ? j.at(key)
                                                                  : empty;
}
const Json &array(const Json &j, const char *key) {
  static const Json empty = Json::array();
  const auto &v = obj(j, key);
  return v.is_array() ? v : empty;
}
int number(const Json &j, const char *key, int fallback = 0) {
  auto value = str(j, key);
  try {
    return value.empty() ? fallback : std::stoi(value);
  } catch (...) {
    return fallback;
  }
}
bool flag(const Json &j, const char *key) {
  const auto &v = obj(j, key);
  return v.is_boolean() ? v.get<bool>() : number(j, key) != 0;
}
BrowseItem link(BrowseKind kind, const std::string &title,
                const std::string &id = {}, const std::string &aux = {}) {
  BrowseItem item;
  item.key = std::to_string(static_cast<int>(kind)) + ":" + id + ":" + aux;
  item.title = title;
  item.next = {kind, id, aux, {}, title};
  return item;
}
BrowseItem video(const Json &j) {
  auto item = link(BrowseKind::Video, str(j, "title"), str(j, "bvid"));
  item.bvid = str(j, "bvid");
  item.aid = std::stoull(str(j, "aid", str(j, "id", "0")));
  item.cid = std::stoull(str(j, "cid", "0"));
  item.mid = str(obj(j, "owner"), "mid", str(j, "mid"));
  item.subtitle = str(obj(j, "owner"), "name", str(j, "author"));
  item.duration = number(j, "duration");
  item.picture = str(j, "pic", str(j, "cover"));
  return item;
}
BrowseItem season(const Json &j) {
  auto item = link(BrowseKind::Season, str(j, "title"), str(j, "season_id"));
  item.picture = str(j, "cover");
  item.subtitle = str(obj(j, "new_ep"), "index_show", str(j, "index_show"));
  return item;
}
BrowseItem commentItem(const Json &j, const BrowseRequest &r) {
  BrowseItem item = link(BrowseKind::CommentActions,
                         str(obj(j, "member"), "uname"), str(j, "rpid"));
  item.comment = CommunityService::comment(j);
  item.text = item.comment.message;
  item.subtitle = std::to_string(item.comment.likes) + " 赞 · " +
                  std::to_string(item.comment.replies) + " 回复";
  item.next.aux = r.id;
  item.next.type = r.type;
  item.mid = item.comment.mid;
  return item;
}
std::string csrf(const Session &session) {
  const auto start = session.cookies.find("bili_jct=");
  if (start == std::string::npos)
    return {};
  const size_t value = start + 9, end = session.cookies.find(';', value);
  return session.cookies.substr(value,
                                end == std::string::npos ? end : end - value);
}
Json normalizeDynamic(const Json &raw) {
  if (!obj(raw, "modules").is_array())
    return raw;
  Json j = raw;
  Json modules = Json::object();
  for (const auto &module : raw.at("modules")) {
    const auto type = str(module, "module_type");
    if (type == "MODULE_TYPE_AUTHOR") {
      const auto &author = obj(module, "module_author");
      modules["module_author"] = obj(author, "user");
      modules["module_author"]["pub_time"] = str(author, "pub_text");
    } else if (type == "MODULE_TYPE_DESC")
      modules["module_dynamic"]["desc"] = obj(module, "module_desc");
    else if (type == "MODULE_TYPE_DYNAMIC") {
      const auto &data = obj(module, "module_dynamic");
      auto &major = modules["module_dynamic"]["major"];
      if (data.contains("dyn_archive"))
        major["archive"] = data.at("dyn_archive");
      if (data.contains("dyn_draw"))
        major["draw"] = data.at("dyn_draw");
      if (data.contains("dyn_live_rcmd"))
        major["live_rcmd"] = data.at("dyn_live_rcmd");
      if (data.contains("dyn_forward"))
        j["orig"] = obj(data.at("dyn_forward"), "item");
    } else if (type == "MODULE_TYPE_STAT") {
      modules["module_stat"] = obj(module, "module_stat");
      const auto &comment = obj(modules["module_stat"], "comment");
      j["basic"] = {{"comment_id_str", str(comment, "comment_id")},
                    {"comment_type", number(comment, "comment_type")}};
    } else if (type == "MODULE_TYPE_ITEM_NULL")
      modules["module_dynamic"]["desc"] = obj(module, "module_item_null");
  }
  j["modules"] = std::move(modules);
  return j;
}
void dynamicItem(BrowsePage &out, const Json &raw, bool detail) {
  const Json j = normalizeDynamic(raw);
  const auto &modules = obj(j, "modules"),
             &author = obj(modules, "module_author");
  const auto &dynamic = obj(modules, "module_dynamic"),
             &major = obj(dynamic, "major");
  const auto &archive = obj(major, "archive");
  out.followed = flag(obj(obj(modules, "module_stat"), "like"), "like_state") ||
                 flag(obj(obj(modules, "module_stat"), "like"), "status");
  if (!detail) {
    auto item =
        link(BrowseKind::DynamicDetail, str(author, "name"), str(j, "id_str"));
    item.subtitle = str(author, "pub_time");
    item.text = str(obj(dynamic, "desc"), "text",
                    str(archive, "title", str(j, "type")));
    item.picture = str(archive, "cover");
    out.items.push_back(std::move(item));
    return;
  }
  out.title = str(author, "name", "动态详情");
  out.description = str(obj(dynamic, "desc"), "text");
  if (!out.description.empty()) {
    auto text = link(BrowseKind::Text, "阅读全文");
    text.text = out.description;
    out.items.push_back(std::move(text));
  }
  if (!str(archive, "bvid").empty()) {
    auto item = video(archive);
    item.picture = str(archive, "cover");
    out.items.push_back(std::move(item));
  }
  auto live = obj(major, "live_rcmd");
  if (live.contains("content") && live.at("content").is_string())
    live = Json::parse(live.at("content").get<std::string>(), nullptr, false);
  const auto &play = obj(obj(live, "card_info"), "live_play_info").empty()
                         ? obj(live, "live_play_info")
                         : obj(obj(live, "card_info"), "live_play_info");
  if (!str(play, "room_id").empty())
    out.items.push_back(
        link(BrowseKind::LiveRoom, str(play, "title"), str(play, "room_id")));
  if (!str(author, "mid").empty())
    out.items.push_back(link(BrowseKind::Space,
                             "UP 主 · " + str(author, "name"),
                             str(author, "mid")));
  const auto &comment = obj(obj(modules, "module_stat"), "comment");
  const auto &basic = obj(j, "basic");
  const std::string oid = str(basic, "comment_id_str");
  if (!oid.empty()) {
    auto item =
        link(BrowseKind::Comments, "评论 · " + str(comment, "count", "0"), oid);
    item.next.type = number(basic, "comment_type");
    out.items.push_back(std::move(item));
  }
  if (j.contains("orig") && j.at("orig").is_object()) {
    auto original = link(BrowseKind::DynamicDetail, "查看转发原动态",
                         str(j.at("orig"), "id_str"));
    out.items.push_back(std::move(original));
  }
  for (const auto &image : array(obj(major, "draw"), "items")) {
    auto item = link(BrowseKind::Image, "动态图片",
                     str(j, "id_str") + "_" + std::to_string(out.items.size()));
    item.picture = str(image, "src");
    item.text = item.picture;
    out.items.push_back(std::move(item));
  }
}
} // namespace
bool isPgcPosterPage(BrowseKind kind) {
  return kind == BrowseKind::Bangumi || kind == BrowseKind::Cinema ||
         kind == BrowseKind::FollowBangumi || kind == BrowseKind::FollowCinema;
}
std::string pgcCoverKey(const BrowseItem &item) {
  if (item.next.kind == BrowseKind::Season && !item.next.id.empty() &&
      item.next.id.find_first_not_of("0123456789") == std::string::npos)
    return "pgc_season_" + item.next.id;
  if (item.next.kind == BrowseKind::Episode)
    return "pgc_episode_" + std::to_string(item.epId);
  return {};
}
void updateCommentVote(int &likes, int &action, bool like, bool undo) {
  const int next = undo ? 0 : (like ? 1 : 2);
  likes = std::max(0, likes + (next == 1 ? 1 : 0) - (action == 1 ? 1 : 0));
  action = next;
}
const char *browseTitle(BrowseKind kind) {
  switch (kind) {
  case BrowseKind::VideoActions:
    return "视频操作";
  case BrowseKind::ChooseCollection:
    return "选择收藏夹";
  case BrowseKind::Discover:
    return "发现";
  case BrowseKind::Personal:
    return "我的 / 云端内容";
  case BrowseKind::Inbox:
    return "消息";
  case BrowseKind::DynamicFeed:
    return "动态";
  case BrowseKind::DynamicDetail:
    return "动态详情";
  case BrowseKind::Space:
    return "用户空间";
  case BrowseKind::SpaceVideos:
    return "用户投稿";
  case BrowseKind::Notifications:
    return "消息通知";
  case BrowseKind::Sessions:
    return "私信";
  case BrowseKind::Conversation:
    return "私信会话";
  case BrowseKind::Bangumi:
    return "番剧";
  case BrowseKind::Cinema:
    return "影视";
  case BrowseKind::Season:
    return "剧集详情";
  case BrowseKind::LiveAreas:
    return "直播分区";
  case BrowseKind::LiveRooms:
    return "直播";
  case BrowseKind::LiveRoom:
    return "直播间";
  case BrowseKind::History:
    return "云端历史";
  case BrowseKind::WatchLater:
    return "云端稍后再看";
  case BrowseKind::Collections:
    return "收藏夹";
  case BrowseKind::Collection:
    return "收藏夹视频";
  case BrowseKind::Following:
    return "关注的人";
  case BrowseKind::FollowBangumi:
    return "我的追番";
  case BrowseKind::FollowCinema:
    return "我的追剧";
  case BrowseKind::Comments:
    return "评论";
  case BrowseKind::Replies:
    return "评论回复";
  case BrowseKind::CommentActions:
    return "评论操作";
  default:
    return "详情";
  }
}
bool browseNeedsLogin(BrowseKind kind) {
  switch (kind) {
  case BrowseKind::Personal:
  case BrowseKind::ChooseCollection:
  case BrowseKind::DynamicFeed:
  case BrowseKind::Inbox:
  case BrowseKind::Notifications:
  case BrowseKind::Sessions:
  case BrowseKind::Conversation:
  case BrowseKind::History:
  case BrowseKind::WatchLater:
  case BrowseKind::Collections:
  case BrowseKind::Following:
  case BrowseKind::FollowBangumi:
  case BrowseKind::FollowCinema:
    return true;
  default:
    return false;
  }
}
CommunityComment CommunityService::comment(const Json &j) {
  CommunityComment c;
  c.id = str(j, "rpid");
  c.root = str(j, "root", "0");
  c.parent = str(j, "parent", "0");
  c.mid = str(obj(j, "member"), "mid");
  c.author = str(obj(j, "member"), "uname");
  c.message = str(obj(j, "content"), "message");
  c.likes = number(j, "like");
  c.replies = number(j, "rcount");
  c.action = number(j, "action");
  return c;
}
Result<BrowsePage> CommunityService::load(const BrowseRequest &r,
                                          const Session &session) {
  const auto op = ErrorOperation::Request;
  if (browseNeedsLogin(r.kind) &&
      !(r.kind == BrowseKind::DynamicFeed && !r.id.empty()) && !session.valid())
    return Result<BrowsePage>::failure(
        {ErrorDomain::Auth, ErrorKind::LoginRequired, op});
  BrowsePage out;
  out.request = r;
  out.title = r.title.empty() ? browseTitle(r.kind) : r.title;
  std::string endpoint;
  ApiParams params;
  bool signedRequest = false;
  const std::string pn = std::to_string(r.page);
  switch (r.kind) {
  case BrowseKind::VideoActions: {
    if (session.valid()) {
      auto later = api_.get(Api::WatchLater, {}, false, op);
      if (!later)
        return Result<BrowsePage>::failure(later.error);
      for (const auto &item : array(later.value.data, "list"))
        if (str(item, "aid") == r.id)
          out.followed = true;
    }
    out.items = {link(BrowseKind::Text, "本地稍后观看", "local-later"),
                 link(BrowseKind::Text,
                      out.followed ? "从云端稍后再看移除" : "加入云端稍后再看",
                      "cloud-later"),
                 link(BrowseKind::ChooseCollection, "选择收藏夹", r.id),
                 link(BrowseKind::Text, "从头播放", "restart")};
    return {std::move(out), {}};
  }
  case BrowseKind::ChooseCollection:
    endpoint = Api::CollectionListAll;
    params = {{"rid", r.id}, {"type", "2"}, {"up_mid", session.mid}};
    break;
  case BrowseKind::Discover:
    out.items = {link(BrowseKind::Bangumi, "番剧"),
                 link(BrowseKind::Cinema, "影视"),
                 link(BrowseKind::LiveAreas, "直播分区"),
                 link(BrowseKind::LiveRooms, "直播推荐")};
    return {std::move(out), {}};
  case BrowseKind::Personal:
    out.items = {link(BrowseKind::Space, "我的空间", session.mid),
                 link(BrowseKind::History, "云端历史"),
                 link(BrowseKind::WatchLater, "稍后再看"),
                 link(BrowseKind::Collections, "收藏夹", session.mid),
                 link(BrowseKind::Following, "关注的人", session.mid),
                 link(BrowseKind::FollowBangumi, "追番", session.mid),
                 link(BrowseKind::FollowCinema, "追剧", session.mid)};
    return {std::move(out), {}};
  case BrowseKind::Inbox:
    out.items = {link(BrowseKind::Notifications, "回复我的", "reply"),
                 link(BrowseKind::Notifications, "@我", "at"),
                 link(BrowseKind::Notifications, "收到的赞", "like"),
                 link(BrowseKind::Sessions, "私信")};
    return {std::move(out), {}};
  case BrowseKind::DynamicFeed:
    endpoint = Api::DynamicArticle;
    params = {{"page", pn},
              {"offset", r.cursor},
              {"host_mid", r.id.empty() ? "0" : r.id}};
    break;
  case BrowseKind::DynamicDetail:
    endpoint = Api::DynamicDetail;
    params = {{"id", r.id}};
    break;
  case BrowseKind::Space:
    endpoint = Api::UserCards;
    params = {{"uids", r.id}};
    break;
  case BrowseKind::SpaceVideos:
    endpoint = Api::UserDynamicVideo;
    params = {{"mid", r.id}, {"pn", pn}, {"ps", "20"}};
    break;
  case BrowseKind::Notifications:
    endpoint = r.id == "reply" ? Api::MsgFeedReply
               : r.id == "at"  ? Api::MsgFeedAt
                               : Api::MsgFeedLike;
    params = {{"id", r.cursor}, {r.id + "_time", r.aux}};
    break;
  case BrowseKind::Sessions:
    endpoint = Api::ChatSessions;
    params = {{"begin_ts", r.cursor.empty() ? "0" : r.cursor},
              {"mobi_app", "web"}};
    break;
  case BrowseKind::Conversation:
    endpoint = Api::ChatFetchMsgs;
    params = {{"sender_device_id", "1"},
              {"talker_id", r.id},
              {"session_type", r.aux.empty() ? "1" : r.aux},
              {"begin_seqno", r.cursor},
              {"size", "20"},
              {"mobi_app", "web"}};
    break;
  case BrowseKind::Bangumi:
  case BrowseKind::Cinema:
    endpoint = r.kind == BrowseKind::Bangumi ? Api::Bangumi : Api::Cinema;
    params = {{"is_refresh", r.cursor.empty() ? "1" : "0"},
              {"cursor", r.cursor}};
    break;
  case BrowseKind::Season:
    endpoint = Api::SeasonDetail;
    params = {{"season_id", r.id}};
    break;
  case BrowseKind::LiveAreas:
    endpoint = Api::LiveAreaList;
    params = {{"platform", "web"}};
    break;
  case BrowseKind::LiveRooms:
    endpoint = r.id.empty() ? Api::LiveFeed : Api::LiveFeedSecond;
    params = {{"parent_area_id", r.aux.empty() ? "0" : r.aux},
              {"area_id", r.id.empty() ? "0" : r.id},
              {"page", pn},
              {"platform", "web"},
              {"device", "switch"},
              {"page_size", "20"},
              {"scale", "xxhdpi"},
              {"source_name", "pc"},
              {"mobi_app", "pc_electron"}};
    signedRequest = !r.id.empty();
    break;
  case BrowseKind::LiveRoom:
    endpoint = Api::RoomPlayInfo;
    params = {{"room_id", r.id},   {"no_playurl", "0"},
              {"mask", "1"},       {"qn", r.aux.empty() ? "0" : r.aux},
              {"protocol", "0,1"}, {"format", "0,1,2"},
              {"codec", "0,1,2"},  {"platform", "web"},
              {"dolby", "5"},      {"ptype", "8"},
              {"panorama", "1"}};
    break;
  case BrowseKind::History:
    endpoint = Api::HistoryVideo;
    params = {{"max", r.cursor.empty() ? "0" : r.cursor},
              {"view_at", r.aux.empty() ? "0" : r.aux},
              {"business", "archive"},
              {"ps", "20"}};
    break;
  case BrowseKind::WatchLater:
    endpoint = Api::WatchLater;
    break;
  case BrowseKind::Collections:
    endpoint = Api::CollectionList;
    params = {{"platform", "pc"}, {"up_mid", r.id}, {"pn", pn}, {"ps", "20"}};
    break;
  case BrowseKind::Collection:
    endpoint = Api::CollectionVideoList;
    params = {
        {"media_id", r.id}, {"pn", pn}, {"ps", "20"}, {"platform", "web"}};
    break;
  case BrowseKind::Following:
    endpoint = Api::DynamicUpListV2;
    params = {};
    break;
  case BrowseKind::FollowBangumi:
  case BrowseKind::FollowCinema:
    endpoint = Api::UserBangumiCollection;
    params = {{"vmid", r.id},
              {"pn", pn},
              {"ps", "20"},
              {"type", r.kind == BrowseKind::FollowBangumi ? "1" : "2"},
              {"platform", "web"},
              {"follow_status", "0"}};
    break;
  case BrowseKind::Comments:
    endpoint = Api::Comment;
    params = {{"oid", r.id},
              {"type", std::to_string(r.type)},
              {"mode", "3"},
              {"next", r.cursor.empty() ? "0" : r.cursor}};
    break;
  case BrowseKind::Replies:
    endpoint = Api::CommentDetail;
    params = {{"oid", r.id},
              {"root", r.aux},
              {"type", std::to_string(r.type)},
              {"next", r.cursor.empty() ? "0" : r.cursor}};
    break;
  default:
    return Result<BrowsePage>::failure(
        {ErrorDomain::Api, ErrorKind::InvalidResponse, op});
  }
  auto result = api_.get(endpoint, params, signedRequest, op);
  if (!result)
    return Result<BrowsePage>::failure(result.error);
  try {
    const auto &d = result.value.data;
    if (!d.is_object() && !(r.kind == BrowseKind::Space && d.is_array()))
      throw std::runtime_error("Expected data");
    switch (r.kind) {
    case BrowseKind::DynamicFeed:
      if (r.page == 1 && r.id.empty())
        out.items.push_back(
            link(BrowseKind::Following, "关注的人", session.mid, "dynamic"));
      for (const auto &j : d.at("items"))
        dynamicItem(out, j, false);
      out.hasMore = flag(d, "has_more");
      out.nextCursor = str(d, "offset");
      break;
    case BrowseKind::DynamicDetail:
      dynamicItem(out, d.at("item"), true);
      break;
    case BrowseKind::Space: {
      const auto &card = d.is_array() ? d.at(0) : d.at(r.id);
      out.title = str(card, "name");
      out.description = "UID " + r.id + " · Lv" + str(card, "level");
      out.items = {link(BrowseKind::SpaceVideos, "投稿", r.id),
                   link(BrowseKind::DynamicFeed, "动态", r.id),
                   link(BrowseKind::Collections, "收藏夹", r.id),
                   link(BrowseKind::Conversation, "私信", r.id, "1")};
      out.items.back().next.title = out.title;
      if (session.valid()) {
        auto relation = api_.get(Api::UserRelation, {{"fid", r.id}}, false, op);
        if (relation)
          out.followed = number(relation.value.data, "attribute") == 2 ||
                         number(relation.value.data, "attribute") == 6;
      }
      break;
    }
    case BrowseKind::SpaceVideos:
      for (const auto &j : d.at("archives"))
        out.items.push_back(video(j));
      out.hasMore = r.page * 20 < number(d.at("page"), "count");
      break;
    case BrowseKind::Notifications: {
      const auto &feed = r.id == "like" ? d.at("total") : d;
      for (const auto &j : array(feed, "items")) {
        const auto &source = obj(j, "item");
        auto item =
            link(BrowseKind::Text, str(source, "title", "消息"), str(j, "id"));
        item.text = str(source, "source_content");
        item.subtitle = str(obj(j, "user"), "nickname");
        item.mid = str(obj(j, "user"), "mid");
        const auto uri = str(source, "uri");
        const auto slash = uri.find_last_of('/');
        if (slash != std::string::npos) {
          std::string target = uri.substr(slash + 1);
          target = target.substr(0, target.find_first_of("#?"));
          if (uri.rfind("https://t.bilibili", 0) == 0) {
            item.next.kind = BrowseKind::DynamicDetail;
            item.next.id = target;
          } else if ((str(source, "type") == "video" ||
                      str(source, "type") == "reply") &&
                     target.rfind("BV", 0) == 0) {
            item.next.kind = BrowseKind::Video;
            item.bvid = target;
          }
        }
        if (item.next.kind == BrowseKind::Text &&
            str(source, "type") == "video") {
          item.next.kind = BrowseKind::Video;
          item.aid = std::stoull(str(source, "subject_id", "0"));
        }
        out.items.push_back(std::move(item));
      }
      const auto &cursor = feed.at("cursor");
      out.hasMore = !flag(cursor, "is_end");
      out.nextCursor = str(cursor, "id");
      out.nextAux = str(cursor, "time");
      break;
    }
    case BrowseKind::Sessions: {
      std::string uids;
      for (const auto &j : array(d, "session_list"))
        if (number(j, "session_type") == 1 &&
            str(obj(j, "account_info"), "name").empty()) {
          if (!uids.empty())
            uids += ',';
          uids += str(j, "talker_id");
        }
      Json cards = Json::object();
      if (!uids.empty()) {
        auto users = api_.get(Api::UserCards, {{"uids", uids}}, false, op);
        if (!users)
          return Result<BrowsePage>::failure(users.error);
        if (users.value.data.is_array()) {
          for (const auto &user : users.value.data)
            cards[str(user, "mid")] = user;
        } else
          cards = users.value.data;
      }
      for (const auto &j : array(d, "session_list")) {
        if (number(j, "session_type") != 1)
          continue;
        const auto uid = str(j, "talker_id");
        auto item =
            link(BrowseKind::Conversation,
                 str(obj(j, "account_info"), "name",
                     str(obj(cards, uid.c_str()), "name", "UID " + uid)),
                 uid, str(j, "session_type", "1"));
        item.subtitle = "未读 " + str(j, "unread_count", "0");
        const auto &last = obj(j, "last_msg");
        auto content = obj(last, "content");
        if (content.is_string())
          content = Json::parse(content.get<std::string>(), nullptr, false);
        item.text = str(content, "content");
        out.items.push_back(std::move(item));
      }
      out.hasMore = false;
      out.nextCursor = std::to_string(std::time(nullptr) * 1000000ULL);
      break;
    }
    case BrowseKind::Conversation:
      for (const auto &j : array(d, "messages")) {
        auto item = link(BrowseKind::Text,
                         str(j, "sender_uid") == session.mid
                             ? "我"
                             : (str(j, "sender_uid") == r.id && !r.title.empty()
                                    ? r.title
                                    : "UID " + str(j, "sender_uid")),
                         str(j, "msg_key"));
        auto content = obj(j, "content");
        if (content.is_string())
          content = Json::parse(content.get<std::string>(), nullptr, false);
        item.text = str(content, "content",
                        number(j, "msg_type") == 2 ? "[图片]" : "[消息]");
        item.subtitle = str(j, "timestamp");
        item.mid = str(j, "sender_uid");
        out.items.push_back(std::move(item));
      }
      out.hasMore = flag(d, "has_more");
      out.nextCursor = str(d, "max_seqno");
      break;
    case BrowseKind::Bangumi:
    case BrowseKind::Cinema:
      for (const auto &module : array(d, "modules"))
        for (const auto &j : array(module, "items"))
          if (!str(j, "season_id").empty())
            out.items.push_back(season(j));
      out.hasMore = flag(d, "has_next");
      out.nextCursor = str(d, "next_cursor");
      break;
    case BrowseKind::Season: {
      out.title = str(d, "title");
      out.description = str(d, "evaluate");
      out.subtitle = str(obj(d, "new_ep"), "desc");
      for (const auto &style : array(d, "styles"))
        if (style.is_string()) {
          if (!out.subtitle.empty()) out.subtitle += " · ";
          out.subtitle += style.get<std::string>();
        }
      if (!out.description.empty()) {
        auto text = link(BrowseKind::Text, "简介");
        text.text = out.description;
        out.items.push_back(std::move(text));
      }
      auto appendEpisode = [&](const Json &j) {
        auto label = str(j, "title");
        if (!label.empty() && label.size() == 1 && label[0] >= '0' && label[0] <= '9') label = "0" + label;
        auto item = link(BrowseKind::Episode,
                         label + " " + str(j, "long_title"),
                         str(j, "id"));
        item.bvid = str(j, "bvid");
        item.aid = std::stoull(str(j, "aid", "0"));
        item.cid = std::stoull(str(j, "cid", "0"));
        item.epId = std::stoull(str(j, "id", "0"));
        item.picture = str(j, "cover");
        item.duration = number(j, "duration") / 1000;
        item.seasonId = std::stoull(r.id);
        const auto &user = obj(d, "user_status");
        if (str(user, "last_ep_id") == str(j, "id"))
          item.startMs = std::max(0, number(user, "last_time")) * 1000LL;
        item.completed = str(user, "last_ep_id") == str(j, "id") && number(user, "last_time") < 0;
        item.subtitle = str(j, "badge");
        out.items.push_back(std::move(item));
      };
      for (const auto &j : array(d, "episodes"))
        appendEpisode(j);
      for (const auto &section : array(d, "section"))
        for (const auto &j : array(section, "episodes"))
          appendEpisode(j);
      if (session.valid()) {
        auto status =
            api_.get(Api::SeasonStatus,
                     {{"season_id", r.id},
                      {"ts", std::to_string(std::time(nullptr) * 1000ULL)}},
                     false, op);
        if (status)
          out.followed = number(status.value.data, "follow") != 0;
      }
      break;
    }
    case BrowseKind::LiveAreas:
      for (const auto &group : d.at("list"))
        for (const auto &j : array(group, "area_list"))
          out.items.push_back(link(BrowseKind::LiveRooms,
                                   str(group, "name") + " / " + str(j, "name"),
                                   str(j, "id"), str(j, "parent_id")));
      break;
    case BrowseKind::LiveRooms: {
      auto appendRoom = [&](const Json &j) {
        auto item = link(BrowseKind::LiveRoom, str(j, "title"),
                         str(j, "roomid", str(j, "room_id")));
        item.subtitle = str(j, "uname");
        item.picture = str(j, "cover", str(j, "user_cover"));
        item.mid = str(j, "uid");
        out.items.push_back(std::move(item));
      };
      for (const auto &j : array(d, "list"))
        appendRoom(j);
      for (const auto &card : array(d, "card_list")) {
        const auto &data = obj(card, "card_data");
        const auto type = str(card, "card_type");
        if (type == "small_card_v1" || type == "second_card_v1")
          appendRoom(obj(data, type.c_str()));
        if (type == "my_idol_v1")
          for (const auto &j : array(obj(data, "my_idol_v1"), "list"))
            appendRoom(j);
      }
      out.hasMore = r.id.empty() ? flag(d, "has_more") : out.items.size() >= 20;
      break;
    }
    case BrowseKind::LiveRoom: {
      out.description = number(d, "live_status") == 1 ? "直播中" : "未开播";
      out.items.push_back(link(BrowseKind::Space, "主播空间", str(d, "uid")));
      const auto &playurl = obj(obj(d, "playurl_info"), "playurl");
      for (const auto &stream : array(playurl, "stream"))
        for (const auto &format : array(stream, "format"))
          for (const auto &codec : array(format, "codec")) {
            if (str(codec, "codec_name") != "avc" ||
                str(format, "format_name") != "flv")
              continue;
            int line = 0;
            for (const auto &url : array(codec, "url_info")) {
              auto item = link(BrowseKind::LiveStream,
                               str(format, "format_name") + " · 线路 " +
                                   std::to_string(++line));
              item.text =
                  str(url, "host") + str(codec, "base_url") + str(url, "extra");
              item.subtitle = "清晰度 " + str(codec, "current_qn");
              out.items.push_back(std::move(item));
            }
          }
      for (const auto &quality : array(playurl, "g_qn_desc")) {
        auto item =
            link(BrowseKind::LiveRoom, "清晰度 · " + str(quality, "desc"), r.id,
                 str(quality, "qn"));
        out.items.push_back(std::move(item));
      }
      break;
    }
    case BrowseKind::History:
      for (const auto &j : array(d, "list")) {
        Json v = j;
        const auto &history = obj(j, "history");
        v["aid"] = obj(history, "oid");
        v["cid"] = obj(history, "cid");
        v["bvid"] = str(history, "bvid");
        v["author"] = str(j, "author_name");
        auto item = video(v);
        item.startMs = std::max(0, number(j, "progress")) * 1000LL;
        item.subtitle += " · 进度 " + str(j, "progress", "0") + "秒";
        out.items.push_back(std::move(item));
      }
      out.nextCursor = str(obj(d, "cursor"), "max");
      out.nextAux = str(obj(d, "cursor"), "view_at");
      out.hasMore = !out.items.empty() && out.nextCursor != "0";
      break;
    case BrowseKind::WatchLater:
      for (const auto &j : array(d, "list")) {
        auto item = video(j);
        item.startMs = std::max(0, number(j, "progress")) * 1000LL;
        out.items.push_back(std::move(item));
      }
      break;
    case BrowseKind::ChooseCollection:
      for (const auto &j : array(d, "list")) {
        auto item = link(BrowseKind::ChooseCollection, str(j, "title"),
                         str(j, "id"), r.id);
        item.subtitle =
            number(j, "fav_state") ? "已收藏 · × 移除" : "未收藏 · × 加入";
        item.duration = number(j, "fav_state");
        out.items.push_back(std::move(item));
      }
      break;
    case BrowseKind::Collections:
      for (const auto &j : array(d, "list")) {
        auto item = link(BrowseKind::Collection, str(j, "title"), str(j, "id"));
        item.subtitle = str(j, "media_count", "0") + " 个视频";
        out.items.push_back(std::move(item));
      }
      out.hasMore = flag(d, "has_more");
      break;
    case BrowseKind::Collection:
      for (const auto &j : array(d, "medias")) {
        auto item = video(j);
        item.subtitle = str(obj(j, "upper"), "name");
        out.items.push_back(std::move(item));
      }
      out.hasMore = flag(d, "has_more");
      break;
    case BrowseKind::Following:
      for (const auto &j : array(d, "items")) {
        auto item = link(r.aux == "dynamic" ? BrowseKind::DynamicFeed
                                            : BrowseKind::Space,
                         str(j, "uname"), str(j, "mid"));
        out.items.push_back(std::move(item));
      }
      break;
    case BrowseKind::FollowBangumi:
    case BrowseKind::FollowCinema:
      for (const auto &j : array(d, "list"))
        out.items.push_back(season(j));
      out.hasMore = r.page * 20 < number(d, "total");
      break;
    case BrowseKind::Comments:
    case BrowseKind::Replies: {
      const auto &cursor = d.at("cursor");
      out.hasMore = !flag(cursor, "is_end");
      out.nextCursor = str(cursor, "next");
      if (r.kind == BrowseKind::Comments) {
        if (r.cursor.empty() || r.cursor == "0")
          for (const auto &j : array(d, "top_replies"))
            out.items.push_back(commentItem(j, r));
        for (const auto &j : array(d, "replies"))
          out.items.push_back(commentItem(j, r));
      } else {
        const auto &root = d.at("root");
        if (r.cursor.empty() || r.cursor == "0")
          out.items.push_back(commentItem(root, r));
        for (const auto &j : array(root, "replies"))
          out.items.push_back(commentItem(j, r));
      }
      break;
    }
    default:
      break;
    }
    if (out.hasMore && out.nextCursor == r.cursor &&
        (r.kind == BrowseKind::DynamicFeed ||
         r.kind == BrowseKind::Notifications ||
         r.kind == BrowseKind::Conversation || r.kind == BrowseKind::Comments ||
         r.kind == BrowseKind::Replies))
      out.hasMore = false;
    return {std::move(out), {}};
  } catch (const std::exception &) {
    return Result<BrowsePage>::failure(
        {ErrorDomain::Api, ErrorKind::InvalidResponse, op});
  }
}
Result<PlaybackProgress>
CommunityService::videoProgress(const std::string &bvid, uint64_t cid) {
  auto result =
      api_.get(Api::PageDetail, {{"bvid", bvid}, {"cid", std::to_string(cid)}},
               true, ErrorOperation::Request);
  if (!result)
    return Result<PlaybackProgress>::failure(result.error);
  try {
    PlaybackProgress progress;
    if (!result.value.data.contains("last_play_cid") ||
        !result.value.data.contains("last_play_time"))
      throw std::runtime_error("Missing playback history");
    progress.cid = std::stoull(str(result.value.data, "last_play_cid", "0"));
    progress.positionMs = std::max<int64_t>(
        0, std::stoll(str(result.value.data, "last_play_time", "0")));
    return {progress, {}};
  } catch (...) {
    return Result<PlaybackProgress>::failure({ErrorDomain::Api,
                                              ErrorKind::InvalidResponse,
                                              ErrorOperation::Request});
  }
}
Result<ApiResponse> CommunityService::write(const CommunityWrite &w,
                                            const Session &session) {
  const auto op = ErrorOperation::Request;
  const auto token = csrf(session);
  if (!session.valid() || token.empty())
    return Result<ApiResponse>::failure(
        {ErrorDomain::Auth, ErrorKind::LoginRequired, op});
  ApiParams p = {{"csrf", token}};
  std::string endpoint;
  switch (w.action) {
  case CommunityAction::ReportHistory:
    endpoint = Api::ProgressReport;
    p.insert(p.end(), {{"mid", session.mid},
                       {"aid", std::to_string(w.aid)},
                       {"cid", std::to_string(w.cid)},
                       {"progress", std::to_string(w.progress)},
                       {"duration", std::to_string(w.duration)},
                       {"type", w.episodeId ? "4" : "3"},
                       {"dt", "9"}});
    if (w.episodeId) {
      p.emplace_back("epid", std::to_string(w.episodeId));
      if (w.seasonId)
        p.emplace_back("sid", std::to_string(w.seasonId));
    }
    break;
  case CommunityAction::LikeDynamic:
    endpoint = Api::DynamicLike;
    p.insert(p.end(), {{"dynamic_id", w.id}, {"up", w.undo ? "2" : "1"}});
    break;
  case CommunityAction::LikeComment:
  case CommunityAction::HateComment:
  case CommunityAction::DeleteComment:
    endpoint = w.action == CommunityAction::LikeComment   ? Api::CommentLike
               : w.action == CommunityAction::HateComment ? Api::CommentDisLike
                                                          : Api::CommentDel;
    p.insert(
        p.end(),
        {{"oid", w.oid}, {"rpid", w.id}, {"type", std::to_string(w.type)}});
    if (w.action != CommunityAction::DeleteComment) {
      p.emplace_back("action", w.undo ? "0" : "1");
      p.emplace_back("ordering", "heat");
    }
    break;
  case CommunityAction::AddComment:
    endpoint = Api::CommentAdd;
    p.insert(p.end(), {{"oid", w.oid},
                       {"message", w.text},
                       {"type", std::to_string(w.type)},
                       {"plat", "1"}});
    if (!w.root.empty() && w.root != "0")
      p.emplace_back("root", w.root);
    if (!w.parent.empty() && w.parent != "0")
      p.emplace_back("parent", w.parent);
    break;
  case CommunityAction::SendMessage:
    endpoint = Api::ChatSendMsg + "?w_sender_uid=" + urlEncode(session.mid) +
               "&w_receiver_id=" + urlEncode(w.id);
    p.insert(p.end(), {{"msg[msg_type]", "1"},
                       {"msg[content]", Json{{"content", w.text}}.dump()},
                       {"msg[sender_uid]", session.mid},
                       {"msg[receiver_id]", w.id},
                       {"msg[receiver_type]", "1"},
                       {"msg[msg_status]", "0"},
                       {"msg[timestamp]", std::to_string(std::time(nullptr))},
                       {"msg[dev_id]", "0"},
                       {"msg[new_face_version]", "1"}});
    break;
  case CommunityAction::Follow:
    endpoint = Api::Follow;
    p.insert(p.end(),
             {{"fid", w.id}, {"act", w.undo ? "2" : "1"}, {"re_src", "11"}});
    break;
  case CommunityAction::FollowSeason:
    endpoint = w.undo ? Api::UndoFollowSeason : Api::FollowSeason;
    p.emplace_back("season_id", w.id);
    break;
  case CommunityAction::WatchLaterAdd:
  case CommunityAction::WatchLaterDelete:
    endpoint = w.action == CommunityAction::WatchLaterAdd
                   ? "//api.bilibili.com/x/v2/history/toview/add"
                   : "//api.bilibili.com/x/v2/history/toview/del";
    p.emplace_back("aid", w.id);
    break;
  case CommunityAction::Collect:
    endpoint = Api::ModifyFavorite;
    p.insert(p.end(), {{"rid", w.oid},
                       {"type", "2"},
                       {w.undo ? "del_media_ids" : "add_media_ids", w.id}});
    break;
  case CommunityAction::Acknowledge:
    // Upstream uses a GET for acknowledgment; it must not be retried
    // automatically.
    return api_.get(Api::ChatUpdateAct,
                    {{"talker_id", w.id},
                     {"session_type", std::to_string(w.type)},
                     {"ack_seqno", w.text},
                     {"csrf", token},
                     {"mobi_app", "web"}},
                    false, op, false);
  }
  auto result = api_.post(endpoint, p, op);
  if (result && w.action == CommunityAction::AddComment &&
      number(result.value.data, "success_action") != 0)
    return Result<ApiResponse>::failure(
        {ErrorDomain::Api, ErrorKind::ApiRejected, op});
  return result;
}
} // namespace pspapp
