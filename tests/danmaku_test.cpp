#include "../client/services/danmaku.hpp"
#include <cassert>
#include <iostream>
#include <fstream>
#include <iterator>
int main(int argc, char** argv) {
    if (argc > 1) {
        std::ifstream input(argv[1]);
        std::string xml((std::istreambuf_iterator<char>(input)), {});
        auto parsed = pspapp::parseDanmaku(xml);
        std::cout << "public XML parsed comments: " << parsed.size() << "\n";
        assert(!parsed.empty());
    }
    auto comments = pspapp::parseDanmaku("<i><d p=\"2,1,25,16777215,0\">A &amp; B &#x4e2d;</d><d p='1,5,25,255,0'>固定</d><d p='3,7,25,255,0'>ignored</d></i>");
    assert(comments.size() == 2 && comments[0].timeMs == 1000);
    assert(comments[1].text == "A & B 中" && comments[1].color == 0xffffff);
    assert(pspapp::parseDanmaku("<d p='nan,1,25,255'>bad</d>").empty());
    assert(pspapp::parseDanmaku("<d p='-1,1,25,255'>bad</d>").empty());
    assert(pspapp::parseDanmaku("<!DOCTYPE i><d p='0,1,25,255'>bad</d>").empty());
    assert(pspapp::parseDanmaku(std::string(512*1024+1, 'a')).empty());
    assert(pspapp::parseDanmaku("<d p='0,1,25,255'>&unknown;</d>").empty());
    pspapp::DanmakuLanes lanes;
    assert(lanes.place(0, 240, 1, 1) == 0);
    assert(lanes.place(4000, 50, 1, 1) == -1); // tail + gap has not entered
    assert(lanes.place(4300, 50, 1, 1) == 0); // equal speed cannot catch up
    assert(lanes.place(4400, 100, 5, 1) == -1); // fixed text cannot cover rolling text
    assert(lanes.place(14000, 100, 5, 1) == 0);
    assert(lanes.place(15000, 50, 1, 1) == -1);
    assert(lanes.place(18000, 50, 1, 1) == 0);
    pspapp::DanmakuLanes fixed;
    assert(fixed.place(0, 100, 4, 6) == 5);
    assert(fixed.place(0, 100, 5, 6) == 0);
    std::cout << "danmaku parser and collision checks: PASS\n";
}
