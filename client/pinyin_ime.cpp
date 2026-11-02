#include "pinyin_ime.hpp"

#include <algorithm>
#include <cstring>

namespace pspapp {
namespace {

uint16_t read16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t read32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void appendUtf8(std::string& out, uint32_t codepoint) {
    if (codepoint < 0x80) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

constexpr size_t kHeaderBytes = 8 + 3 * 20;

} // namespace

PinyinDictionary::~PinyinDictionary() { close(); }

void PinyinDictionary::close() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
    for (auto& table : tables_) table = Table{};
    syllables_.clear();
    for (auto& entry : cache_) entry = CachedBlock{};
}

bool PinyinDictionary::open(const char* path) {
    close();
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    uint8_t header[kHeaderBytes];
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header) || std::memcmp(header, "WPYD", 4) != 0 ||
        read16(header + 4) != 1) {
        std::fclose(file);
        return false;
    }
    const unsigned syllableCount = read16(header + 6);
    std::fseek(file, 0, SEEK_END);
    const long fileSize = std::ftell(file);
    // Syllable list directly after the header.
    std::fseek(file, static_cast<long>(kHeaderBytes), SEEK_SET);
    for (unsigned i = 0; i < syllableCount; ++i) {
        uint8_t length = 0;
        char text[256];
        if (std::fread(&length, 1, 1, file) != 1 || std::fread(text, 1, length, file) != length) {
            std::fclose(file);
            return false;
        }
        syllables_.insert(std::string(text, length));
    }
    bool ok = !syllables_.empty();
    for (int t = 0; t < TableCount && ok; ++t) {
        const uint8_t* p = header + 8 + t * 20;
        Table& table = tables_[t];
        table.count = read32(p);
        table.indexOffset = read32(p + 4);
        table.indexBytes = read32(p + 8);
        table.dataOffset = read32(p + 12);
        table.dataBytes = read32(p + 16);
        if (static_cast<uint64_t>(table.indexOffset) + table.indexBytes > static_cast<uint64_t>(fileSize) ||
            static_cast<uint64_t>(table.dataOffset) + table.dataBytes > static_cast<uint64_t>(fileSize)) {
            ok = false;
            break;
        }
        std::vector<uint8_t> index(table.indexBytes);
        std::fseek(file, static_cast<long>(table.indexOffset), SEEK_SET);
        if (!index.empty() && std::fread(index.data(), 1, index.size(), file) != index.size()) {
            ok = false;
            break;
        }
        for (size_t pos = 0; pos < index.size();) {
            if (pos + 5 > index.size() || pos + 5 + index[pos + 4] > index.size()) {
                ok = false;
                break;
            }
            Block block;
            block.offset = read32(&index[pos]);
            block.firstKey.assign(reinterpret_cast<const char*>(&index[pos + 5]), index[pos + 4]);
            pos += 5 + index[pos + 4];
            if (block.offset > table.dataBytes) {
                ok = false;
                break;
            }
            table.blocks.push_back(std::move(block));
        }
    }
    if (!ok) {
        std::fclose(file);
        for (auto& table : tables_) table = Table{};
        syllables_.clear();
        return false;
    }
    file_ = file;
    return true;
}

const std::vector<uint8_t>* PinyinDictionary::block(int tableId, size_t index) const {
    CachedBlock* victim = &cache_[0];
    for (auto& entry : cache_) {
        if (entry.table == tableId && entry.index == index) {
            entry.stamp = ++clock_;
            return &entry.bytes;
        }
        if (entry.stamp < victim->stamp) victim = &entry;
    }
    const Table& table = tables_[tableId];
    const uint32_t begin = table.blocks[index].offset;
    const uint32_t end = index + 1 < table.blocks.size() ? table.blocks[index + 1].offset : table.dataBytes;
    victim->table = -1;
    victim->bytes.assign(end - begin, 0);
    if (std::fseek(file_, static_cast<long>(table.dataOffset + begin), SEEK_SET) != 0 ||
        std::fread(victim->bytes.data(), 1, victim->bytes.size(), file_) != victim->bytes.size())
        return nullptr;
    victim->table = tableId;
    victim->index = index;
    victim->stamp = ++clock_;
    return &victim->bytes;
}

// The block that can hold the first record with key >= `key`: the one before the first block
// whose first key is >= `key`, because equal keys may straddle a boundary.
size_t PinyinDictionary::firstBlock(const Table& table, const std::string& key) const {
    size_t low = 0, high = table.blocks.size();
    while (low < high) {
        const size_t mid = (low + high) / 2;
        if (table.blocks[mid].firstKey < key) low = mid + 1; else high = mid;
    }
    return low > 0 ? low - 1 : 0;
}

void PinyinDictionary::scan(TableId tableId, const std::string& key, bool prefix, size_t limit,
                            const std::function<bool(const Entry&)>& visit) const {
    if (!file_ || key.empty()) return;
    const Table& table = tables_[tableId];
    size_t visited = 0;
    for (size_t index = firstBlock(table, key); index < table.blocks.size(); ++index) {
        const std::vector<uint8_t>* bytes = block(tableId, index);
        if (!bytes) return;
        for (size_t pos = 0; pos < bytes->size();) {
            const uint8_t* p = bytes->data() + pos;
            const size_t keyLength = p[0];
            if (pos + 1 + keyLength + 1 > bytes->size()) return;
            const size_t chars = p[1 + keyLength];
            const size_t total = 1 + keyLength + 1 + chars * 2 + 2;
            if (pos + total > bytes->size()) return;
            Entry entry;
            entry.key.assign(reinterpret_cast<const char*>(p + 1), keyLength);
            pos += total;
            const int order = prefix ? entry.key.compare(0, key.size(), key) : entry.key.compare(key);
            if (order < 0) continue;   // before the range
            if (order > 0) return;     // past the range
            const uint8_t* text = p + 1 + keyLength + 1;
            for (size_t i = 0; i < chars; ++i) appendUtf8(entry.text, read16(text + i * 2));
            entry.weight = read16(text + chars * 2);
            if (!visit(entry)) return;
            if (++visited >= limit) return;
        }
    }
}

bool PinyinIme::addLetter(char letter) {
    if (letter >= 'A' && letter <= 'Z') letter = static_cast<char>(letter - 'A' + 'a');
    if (letter < 'a' || letter > 'z' || buffer_.size() >= kMaxInput) return false;
    buffer_.push_back(letter);
    refresh();
    return true;
}

void PinyinIme::backspace() {
    if (!buffer_.empty()) buffer_.pop_back();
    refresh();
}

void PinyinIme::reset() {
    buffer_.clear();
    chosen_.clear();
    candidates_.clear();
    seen_.clear();
}

bool PinyinIme::choose(size_t index) {
    if (index >= candidates_.size()) return false;
    const ImeCandidate candidate = candidates_[index];
    chosen_ += candidate.text;
    buffer_.erase(0, std::min<size_t>(candidate.consumed, buffer_.size()));
    refresh();
    return buffer_.empty();
}

std::string PinyinIme::takeCommit() {
    if (!buffer_.empty() || chosen_.empty()) return {};
    std::string text = std::move(chosen_);
    reset();
    return text;
}

std::string PinyinIme::commitRaw() {
    std::string text = preedit();
    reset();
    return text;
}

void PinyinIme::add(const std::string& text, unsigned consumed, uint16_t weight) {
    if (candidates_.size() >= kMaxCandidates || !seen_.insert(text).second) return;
    candidates_.push_back({text, static_cast<uint8_t>(consumed), weight});
}

void PinyinIme::refresh() {
    candidates_.clear();
    seen_.clear();
    if (buffer_.empty() || !dictionary_.isOpen()) return;
    const std::string& in = buffer_;
    const size_t n = in.size();

    // 1. Whole input is a complete reading.
    dictionary_.scan(PinyinDictionary::Full, in, false, 40, [&](const PinyinDictionary::Entry& e) {
        add(e.text, static_cast<unsigned>(n), e.weight);
        return true;
    });
    // 2. Whole input is the initials of a word (e.g. "nh" for the word read "ni hao").
    if (n >= 2) {
        dictionary_.scan(PinyinDictionary::Abbreviation, in, false, 20, [&](const PinyinDictionary::Entry& e) {
            add(e.text, static_cast<unsigned>(n), e.weight);
            return true;
        });
    }
    // 3. Words that begin with the input. Short inputs use the precomputed best-of table;
    //    longer ones scan the keys that start with them and keep the heaviest.
    if (n <= 3) {
        dictionary_.scan(PinyinDictionary::Prefix, in, false, 12, [&](const PinyinDictionary::Entry& e) {
            add(e.text, static_cast<unsigned>(n), e.weight);
            return true;
        });
    } else {
        std::vector<ImeCandidate> found;
        dictionary_.scan(PinyinDictionary::Full, in, true, 3000, [&](const PinyinDictionary::Entry& e) {
            if (e.key.size() > n) found.push_back({e.text, static_cast<uint8_t>(n), e.weight});
            return true;
        });
        std::stable_sort(found.begin(), found.end(),
                         [](const ImeCandidate& a, const ImeCandidate& b) { return a.weight > b.weight; });
        for (size_t i = 0; i < found.size() && i < 24; ++i) add(found[i].text, n, found[i].weight);
    }
    // 4. A reading for the first part only; the rest stays in the input. Only split where the
    //    letters form whole syllables.
    std::vector<bool> reach(n + 1, false);
    reach[0] = true;
    for (size_t end = 1; end <= n; ++end)
        for (size_t length = 1; length <= 6 && length <= end && !reach[end]; ++length)
            reach[end] = reach[end - length] && dictionary_.isSyllable(in.substr(end - length, length));
    for (size_t split = n - 1; split >= 1; --split) {
        if (!reach[split]) continue;
        unsigned taken = 0;
        dictionary_.scan(PinyinDictionary::Full, in.substr(0, split), false, 5, [&](const PinyinDictionary::Entry& e) {
            if (taken++ < 5) add(e.text, static_cast<unsigned>(split), e.weight);
            return taken < 5;
        });
    }
}

size_t utf8Length(const std::string& text) {
    size_t count = 0;
    for (unsigned char c : text) count += (c & 0xC0) != 0x80;
    return count;
}

void utf8PopBack(std::string& text) {
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
    if (!text.empty()) text.pop_back();
}

} // namespace pspapp
