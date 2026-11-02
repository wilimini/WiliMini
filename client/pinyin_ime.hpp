#pragma once
// Built-in pinyin input method (no platform dependencies, so it is unit-tested on the host).
//
// The dictionary (client/assets/ime/pinyin.dat, built by scripts/prepare-pinyin-dict.py) stays on
// the Memory Stick: only its small block index and syllable list are held in RAM, and a lookup
// reads a few blocks of about 1 KB with fseek/fread.
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace pspapp {

class PinyinDictionary {
public:
    PinyinDictionary() = default;
    ~PinyinDictionary();
    PinyinDictionary(const PinyinDictionary&) = delete;
    PinyinDictionary& operator=(const PinyinDictionary&) = delete;

    bool open(const char* path);
    void close();
    bool isOpen() const { return file_ != nullptr; }
    bool isSyllable(const std::string& text) const { return syllables_.count(text) != 0; }

    enum TableId { Full = 0, Abbreviation = 1, Prefix = 2, TableCount = 3 };
    struct Entry {
        std::string key;
        std::string text; // UTF-8
        uint16_t weight = 0;
    };
    // Calls visit for each record whose key equals `key` (or starts with it when `prefix`), in
    // key order and weight-descending within a key. Stops when visit returns false or after
    // `limit` records.
    void scan(TableId table, const std::string& key, bool prefix, size_t limit,
              const std::function<bool(const Entry&)>& visit) const;

private:
    struct Block {
        uint32_t offset = 0;
        std::string firstKey;
    };
    struct Table {
        uint32_t count = 0, indexOffset = 0, indexBytes = 0, dataOffset = 0, dataBytes = 0;
        std::vector<Block> blocks;
    };
    struct CachedBlock {
        int table = -1;
        size_t index = 0;
        unsigned stamp = 0;
        std::vector<uint8_t> bytes;
    };

    const std::vector<uint8_t>* block(int table, size_t index) const;
    size_t firstBlock(const Table& table, const std::string& key) const;

    std::FILE* file_ = nullptr;
    Table tables_[TableCount];
    std::unordered_set<std::string> syllables_;
    mutable CachedBlock cache_[6];
    mutable unsigned clock_ = 0;
};

struct ImeCandidate {
    std::string text;      // UTF-8
    uint8_t consumed = 0;  // letters of the input this candidate replaces
    uint16_t weight = 0;
};

// Composition state: typed letters, the characters already chosen for this phrase, and the
// ranked candidates for the remaining letters.
class PinyinIme {
public:
    explicit PinyinIme(const PinyinDictionary& dictionary) : dictionary_(dictionary) {}

    static constexpr size_t kMaxInput = 24;
    static constexpr size_t kMaxCandidates = 60;

    bool addLetter(char letter); // a-z (any case); false when not accepted
    void backspace();            // removes the last letter
    void reset();
    // Replaces the input with the candidate; true when the phrase is complete and takeCommit()
    // has text.
    bool choose(size_t index);

    bool composing() const { return !buffer_.empty() || !chosen_.empty(); }
    const std::string& buffer() const { return buffer_; }
    const std::string& chosen() const { return chosen_; }
    const std::vector<ImeCandidate>& candidates() const { return candidates_; }
    // Text to show in the pre-edit area: chosen characters followed by the remaining letters.
    std::string preedit() const { return chosen_ + buffer_; }

    // Finished text (chosen characters), or empty. Clears the composition when it returns text.
    std::string takeCommit();
    // Commits what is shown in the pre-edit area as plain text (e.g. for the enter key).
    std::string commitRaw();

private:
    void refresh();
    void add(const std::string& text, unsigned consumed, uint16_t weight);

    const PinyinDictionary& dictionary_;
    std::string buffer_, chosen_;
    std::vector<ImeCandidate> candidates_;
    std::unordered_set<std::string> seen_;
};

// UTF-8 helpers shared with the UI.
size_t utf8Length(const std::string& text);
void utf8PopBack(std::string& text);

} // namespace pspapp
