// Host test for the built-in pinyin input method, run against the generated dictionary.
#include "../client/pinyin_ime.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>

using pspapp::ImeCandidate;
using pspapp::PinyinDictionary;
using pspapp::PinyinIme;

static int find(const PinyinIme& ime, const std::string& text, size_t limit = 1000) {
    const auto& list = ime.candidates();
    for (size_t i = 0; i < list.size() && i < limit; ++i)
        if (list[i].text == text) return static_cast<int>(i);
    return -1;
}

static void type(PinyinIme& ime, const char* letters) {
    for (const char* c = letters; *c; ++c) assert(ime.addLetter(*c));
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "client/assets/ime/pinyin.dat";
    PinyinDictionary dictionary;
    assert(dictionary.open(path));
    assert(dictionary.isSyllable("zhuang") && dictionary.isSyllable("lv") && !dictionary.isSyllable("zhh"));

    PinyinIme ime(dictionary);
    // Whole-word readings come first.
    type(ime, "nihao");
    assert(find(ime, "你好") == 0);
    ime.reset();
    type(ime, "zhongguo");
    assert(find(ime, "中国", 3) >= 0);
    ime.reset();
    // A single syllable lists characters, and both ways to split "xian" appear.
    type(ime, "ni");
    assert(find(ime, "你", 3) >= 0);
    ime.reset();
    type(ime, "xian");
    assert(find(ime, "先") >= 0 && find(ime, "西安") >= 0);
    ime.reset();
    // Initials.
    type(ime, "nh");
    assert(find(ime, "你好") >= 0);
    ime.reset();
    // Partial input offers completions; a single letter is not an error.
    type(ime, "nih");
    assert(find(ime, "你好") >= 0 && ime.candidates()[0].consumed == 3);
    ime.reset();
    type(ime, "w");
    assert(!ime.candidates().empty());
    ime.reset();
    type(ime, "qqqq");
    assert(ime.candidates().empty());
    ime.reset();

    // Choosing a candidate for the first part leaves the rest of the input.
    type(ime, "woaini");
    const int whole = find(ime, "我爱你");
    assert(whole >= 0);
    int first = -1;
    for (size_t i = 0; i < ime.candidates().size(); ++i)
        if (ime.candidates()[i].text == "我" && ime.candidates()[i].consumed == 2) first = static_cast<int>(i);
    assert(first >= 0);
    assert(!ime.choose(static_cast<size_t>(first)));
    assert(ime.buffer() == "aini" && ime.chosen() == "我" && ime.preedit() == "我aini");
    assert(find(ime, "爱你") >= 0);
    assert(ime.choose(static_cast<size_t>(find(ime, "爱你"))));
    assert(ime.takeCommit() == "我爱你" && !ime.composing());

    // Backspace shortens the input and refreshes the list.
    type(ime, "nihaoz");
    ime.backspace();
    assert(ime.buffer() == "nihao" && find(ime, "你好") == 0);
    ime.backspace(); ime.backspace(); ime.backspace(); ime.backspace(); ime.backspace();
    assert(!ime.composing() && ime.candidates().empty());
    ime.backspace(); // harmless when empty

    // Input limits and letter handling.
    assert(!ime.addLetter('1') && !ime.addLetter(' '));
    assert(ime.addLetter('N') && ime.buffer() == "n");
    ime.reset();
    for (size_t i = 0; i < PinyinIme::kMaxInput; ++i) assert(ime.addLetter('a'));
    assert(!ime.addLetter('a'));
    ime.reset();
    assert(ime.commitRaw().empty());
    type(ime, "abc");
    assert(ime.commitRaw() == "abc" && !ime.composing());

    // UTF-8 helpers.
    std::string text = "a你b好";
    assert(pspapp::utf8Length(text) == 4);
    pspapp::utf8PopBack(text);
    assert(text == "a你b");
    pspapp::utf8PopBack(text); pspapp::utf8PopBack(text);
    assert(text == "a");

    // Candidate lists stay bounded and unique.
    type(ime, "shi");
    assert(ime.candidates().size() <= PinyinIme::kMaxCandidates);
    ime.reset();

    // Timing, for the record (a keystroke builds one list).
    using clock = std::chrono::steady_clock;
    const char* words[] = {"zhongguoren", "woxiangkanshipin", "shenmeshihou", "bilibili", "zhi"};
    for (const char* word : words) {
        const auto start = clock::now();
        for (int i = 0; i < 20; ++i) { ime.reset(); type(ime, word); }
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - start).count();
        std::printf("  %-18s %6.1f us/keystroke-sequence, %zu candidates\n", word, us / 20.0 / std::string(word).size(),
                    ime.candidates().size());
    }

    // A missing or damaged file fails cleanly.
    PinyinDictionary missing;
    assert(!missing.open("/nonexistent/pinyin.dat"));
    PinyinIme closedIme(missing);
    assert(closedIme.addLetter('a') && closedIme.candidates().empty());
    std::puts("pinyin ime: PASS");
    return 0;
}
