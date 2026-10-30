#include "../client/services/subtitles.hpp"
#include <cassert>
#include <iostream>
int main() {
    using namespace pspapp;
    auto cues = parseSubtitles(R"({"body":[{"from":3,"to":5,"content":"后一句"},{"from":0,"to":2,"content":"First\nline"},{"from":-1,"to":2,"content":"bad"},{"from":2,"to":1,"content":"bad"}]})");
    assert(cues.size() == 2 && cues[0].text == "First\nline");
    assert(subtitleAt(cues, -1) == -1 && subtitleAt(cues, 0) == 0);
    assert(subtitleAt(cues, 1999) == 0 && subtitleAt(cues, 2000) == -1);
    assert(subtitleAt(cues, 3000) == 1 && subtitleAt(cues, 5000) == -1);
    assert(subtitleAt(cues, 1000) == 0); // Backward seek, no stale cursor.
    cues = {{0, 10000, "long"}, {2000, 3000, "short"}};
    assert(subtitleAt(cues, 2500) == 1 && subtitleAt(cues, 4000) == 0);
    assert(parseSubtitles("bad").empty());
    assert(parseSubtitles(R"({"body":null})").empty());
    assert(parseSubtitles(std::string(512*1024+1, ' ')).empty());
    std::string large = "{\"body\":[";
    for (int i = 0; i < 2049; ++i) { if (i) large += ','; large += R"({"from":0,"to":1,"content":"x"})"; }
    large += "]}"; assert(parseSubtitles(large).empty());
    std::cout << "subtitles: PASS\n";
}
