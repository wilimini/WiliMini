#include "comment_layout.hpp"
#include <cassert>

using namespace pspapp;

int main() {
    const std::vector<CommentRowSpec> rows = {{1, 0, 0}, {5, 2, 12}, {3, 0, 0}, {40, 0, 0}, {2, 0, 0}};
    CommentLayoutParams p;
    p.lineHeight = 18;
    p.viewportHeight = 180;

    // Collapsed rows show at most two lines, so a long comment is no taller than a two-line one.
    CommentLayout collapsed = layoutComments(rows, p);
    assert(collapsed.height[1] == collapsed.height[2]);
    assert(collapsed.height[0] < collapsed.height[1]);
    assert(collapsed.top[1] == collapsed.height[0]);
    assert(collapsed.total == collapsed.top[4] + collapsed.height[4]);

    // Expanding the selected row shows its whole message and the reply previews.
    p.selected = 1;
    p.expanded = true;
    CommentLayout open = layoutComments(rows, p);
    assert(open.height[1] > collapsed.height[1]);
    assert(open.visibleBody == 5 && open.maxInner == 0);
    assert(open.height[1] == kCommentPadTop + kCommentHeader + 5 * 18 + commentExtras(rows[1]) + kCommentPadBottom);
    assert(open.height[0] == collapsed.height[0] && open.height[2] == collapsed.height[2]);

    // With previews present a six-line message no longer fits and scrolls inside its row.
    std::vector<CommentRowSpec> sixLines = rows;
    sixLines[1].bodyLines = 6;
    p.selected = 1;
    p.expanded = true;
    CommentLayout scrolled = layoutComments(sixLines, p);
    assert(scrolled.visibleBody == 5 && scrolled.maxInner == 1);

    // A message taller than the viewport scrolls inside its row and the row fits the viewport.
    p.selected = 3;
    CommentLayout tall = layoutComments(rows, p);
    assert(tall.maxInner > 0 && tall.visibleBody < 40);
    assert(tall.height[3] <= p.viewportHeight);
    assert(tall.visibleBody + tall.maxInner == 40);
    assert(tall.scrollTarget == tall.top[3] || tall.scrollTarget == tall.total - p.viewportHeight);

    // The scroll target keeps the selected row in view and never goes negative or past the end.
    p.expanded = false;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        p.selected = i;
        CommentLayout l = layoutComments(rows, p);
        assert(l.scrollTarget >= 0 && l.scrollTarget <= std::max(0, l.total - p.viewportHeight));
        assert(l.top[i] >= l.scrollTarget || l.scrollTarget == 0);
        assert(l.top[i] + l.height[i] <= l.scrollTarget + p.viewportHeight || l.total <= p.viewportHeight ||
               l.scrollTarget == l.total - p.viewportHeight);
    }
    p.selected = 0;
    assert(layoutComments(rows, p).scrollTarget == 0);

    // An empty list is fine.
    assert(layoutComments({}, p).total == 0);
    return 0;
}
