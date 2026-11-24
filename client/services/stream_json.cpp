#include "stream_json.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace pspapp {
namespace {

BrowseItem makeLink(BrowseKind kind, const std::string &title, const std::string &id = {}) {
    BrowseItem item;
    item.key = std::to_string(static_cast<int>(kind)) + ":" + id + ":";
    item.title = title;
    item.next = {kind, id, {}, {}, title};
    return item;
}

uint64_t toU64(const std::string &s) {
    char *end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    return !s.empty() && end && *end == '\0' ? static_cast<uint64_t>(v) : 0;
}

int toInt(const std::string &s) {
    char *end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 10);
    return !s.empty() && end && *end == '\0' ? static_cast<int>(v) : 0;
}

class SeasonSax : public nlohmann::json_sax<nlohmann::json> {
  public:
    SeasonSax(uint64_t seasonId, size_t offset, size_t limit, BrowsePage &out, StreamApiMeta &meta)
        : seasonId_(seasonId), offset_(offset), limit_(limit), out_(out), meta_(meta) {}

    void apply() {
        out_.items.clear();
        out_.title = std::move(title_);
        out_.description = std::move(desc_);
        out_.subtitle = std::move(newEpDesc_);
        for (const auto &s : styles_) {
            if (!out_.subtitle.empty()) out_.subtitle += " · ";
            out_.subtitle += s;
        }
        out_.episodeOffset = offset_;
        out_.totalEpisodes = total_;
        if (!out_.description.empty()) {
            auto text = makeLink(BrowseKind::Text, "简介");
            text.text = out_.description;
            out_.items.push_back(std::move(text));
        }
        for (auto &ep : window_) {
            if (hasLastEp_ && lastEpId_ != 0 && ep.epId == lastEpId_) {
                ep.startMs = std::max(0, lastTime_) * 1000LL;
                ep.completed = lastTime_ < 0;
            }
            out_.items.push_back(std::move(ep));
        }
    }

    bool null() override { key_.clear(); return true; }
    bool boolean(bool) override { key_.clear(); return true; }
    bool number_float(number_float_t, const string_t &) override { key_.clear(); return true; }
    bool binary(binary_t &) override { key_.clear(); return true; }

    bool number_integer(number_integer_t v) override {
        onInt(static_cast<int64_t>(v));
        key_.clear();
        return true;
    }

    bool number_unsigned(number_unsigned_t v) override {
        onUint(static_cast<uint64_t>(v));
        key_.clear();
        return true;
    }

    bool string(string_t &v) override {
        if (!skip_ && !stack_.empty()) {
            const C top = stack_.back();
            if (top == C::Result) {
                if (key_ == "title") title_ = v;
                else if (key_ == "evaluate") desc_ = v;
            } else if (top == C::NewEp && key_ == "desc") {
                newEpDesc_ = v;
            } else if (top == C::Styles) {
                styles_.push_back(v);
            } else if (top == C::UserStatus) {
                if (key_ == "last_ep_id") { lastEpId_ = toU64(v); hasLastEp_ = !v.empty(); }
                else if (key_ == "last_time") lastTime_ = toInt(v);
            } else if (top == C::Episode) {
                if (key_ == "title") ep_.title = v;
                else if (key_ == "long_title") ep_.longTitle = v;
                else if (key_ == "bvid") ep_.bvid = v;
                else if (key_ == "cover") ep_.cover = v;
                else if (key_ == "badge") ep_.badge = v;
                else if (key_ == "id") ep_.epId = toU64(v);
                else if (key_ == "aid") ep_.aid = toU64(v);
                else if (key_ == "cid") ep_.cid = toU64(v);
                else if (key_ == "duration") ep_.durationMs = toInt(v);
            }
        }
        key_.clear();
        return true;
    }

    bool key(string_t &v) override {
        if (!skip_ && !stack_.empty()) key_ = v;
        return true;
    }

    bool start_object(std::size_t) override {
        if (skip_) { ++skip_; return true; }
        if (stack_.empty()) { stack_.push_back(C::Root); return true; }
        const C top = stack_.back();
        const std::string k = std::move(key_);
        key_.clear();
        if (top == C::Root && !meta_.hasData && (k == "result" || k == "data")) {
            meta_.hasData = true;
            stack_.push_back(C::Result);
            return true;
        }
        if (top == C::Result && k == "new_ep") { stack_.push_back(C::NewEp); return true; }
        if (top == C::Result && k == "user_status") { stack_.push_back(C::UserStatus); return true; }
        if (top == C::SectionArr) { stack_.push_back(C::SectionObj); return true; }
        if (top == C::Episodes) {
            const size_t ordinal = total_++;
            if (ordinal >= offset_ && ordinal - offset_ < limit_) {
                ep_ = {};
                stack_.push_back(C::Episode);
                return true;
            }
        }
        skip_ = 1;
        return true;
    }

    bool end_object() override {
        if (skip_) { --skip_; return true; }
        if (stack_.empty()) return false;
        const C popped = stack_.back();
        stack_.pop_back();
        key_.clear();
        if (popped == C::Episode) emitEpisode();
        return true;
    }

    bool start_array(std::size_t) override {
        if (skip_) { ++skip_; return true; }
        if (stack_.empty()) return false;
        const C top = stack_.back();
        const std::string k = std::move(key_);
        key_.clear();
        if (top == C::Result && k == "styles") { stack_.push_back(C::Styles); return true; }
        if ((top == C::Result || top == C::SectionObj) && k == "episodes") {
            stack_.push_back(C::Episodes);
            return true;
        }
        if (top == C::Result && k == "section") { stack_.push_back(C::SectionArr); return true; }
        skip_ = 1;
        return true;
    }

    bool end_array() override {
        if (skip_) { --skip_; return true; }
        if (stack_.empty()) return false;
        stack_.pop_back();
        key_.clear();
        return true;
    }

    bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception &) override {
        return false;
    }

  private:
    enum class C { Root, Result, NewEp, Styles, UserStatus, Episodes, Episode, SectionArr, SectionObj };
    struct Ep {
        uint64_t aid = 0, cid = 0, epId = 0;
        int durationMs = 0;
        std::string title, longTitle, bvid, cover, badge;
    };

    void onInt(int64_t v) {
        if (skip_ || stack_.empty()) return;
        const C top = stack_.back();
        if (top == C::Root && key_ == "code") { meta_.code = static_cast<int>(v); meta_.hasCode = true; }
        else if (top == C::UserStatus && key_ == "last_time") lastTime_ = static_cast<int>(v);
        else if (top == C::UserStatus && key_ == "last_ep_id" && v >= 0) {
            lastEpId_ = static_cast<uint64_t>(v);
            hasLastEp_ = true;
        } else if (top == C::Episode) {
            if (key_ == "id" && v >= 0) ep_.epId = static_cast<uint64_t>(v);
            else if (key_ == "aid" && v >= 0) ep_.aid = static_cast<uint64_t>(v);
            else if (key_ == "cid" && v >= 0) ep_.cid = static_cast<uint64_t>(v);
            else if (key_ == "duration") ep_.durationMs = static_cast<int>(v);
            else if (key_ == "title") ep_.title = std::to_string(v);
        }
    }

    void onUint(uint64_t v) {
        if (skip_ || stack_.empty()) return;
        const C top = stack_.back();
        if (top == C::Root && key_ == "code") { meta_.code = static_cast<int>(v); meta_.hasCode = true; }
        else if (top == C::UserStatus && key_ == "last_time") lastTime_ = static_cast<int>(v);
        else if (top == C::UserStatus && key_ == "last_ep_id") { lastEpId_ = v; hasLastEp_ = true; }
        else if (top == C::Episode) {
            if (key_ == "id") ep_.epId = v;
            else if (key_ == "aid") ep_.aid = v;
            else if (key_ == "cid") ep_.cid = v;
            else if (key_ == "duration") ep_.durationMs = static_cast<int>(v);
            else if (key_ == "title") ep_.title = std::to_string(v);
        }
    }

    void emitEpisode() {
        std::string label = ep_.title;
        if (label.size() == 1 && label[0] >= '0' && label[0] <= '9') label = "0" + label;
        BrowseItem item = makeLink(BrowseKind::Episode, label + " " + ep_.longTitle,
                                   std::to_string(ep_.epId));
        item.bvid = std::move(ep_.bvid);
        item.aid = ep_.aid;
        item.cid = ep_.cid;
        item.epId = ep_.epId;
        item.picture = std::move(ep_.cover);
        item.duration = ep_.durationMs / 1000;
        item.seasonId = seasonId_;
        item.subtitle = std::move(ep_.badge);
        window_.push_back(std::move(item));
    }

    uint64_t seasonId_;
    size_t offset_, limit_, total_ = 0, skip_ = 0;
    BrowsePage &out_;
    StreamApiMeta &meta_;
    std::vector<C> stack_;
    std::string key_, title_, desc_, newEpDesc_;
    std::vector<std::string> styles_;
    uint64_t lastEpId_ = 0;
    int lastTime_ = 0;
    bool hasLastEp_ = false;
    Ep ep_;
    std::vector<BrowseItem> window_;
};

} // namespace

void parseSeasonStream(const HttpResponse &response, uint64_t seasonId,
                       size_t windowOffset, size_t windowSize,
                       BrowsePage &out, StreamApiMeta &meta) {
    meta = {};
    SeasonSax sax(seasonId, windowOffset, windowSize, out, meta);
    if (!response.spilled()) {
        meta.parseOk = nlohmann::json::sax_parse(response.body, &sax);
    } else if (std::FILE *file = std::fopen(response.bodyPath.c_str(), "rb")) {
        char buf[8192];
        std::setvbuf(file, buf, _IOFBF, sizeof(buf));
        try {
            meta.parseOk = nlohmann::json::sax_parse(file, &sax);
        } catch (...) {
            std::fclose(file);
            throw;
        }
        std::fclose(file);
    }
    if (meta.parseOk && meta.hasCode && meta.code == 0 && meta.hasData)
        sax.apply();
}

} // namespace pspapp
