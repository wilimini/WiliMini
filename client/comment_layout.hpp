#pragma once

// Row layout for the comment lists (video comments and reply threads). Pure arithmetic with no
// PSP dependencies so it can be tested on the host (tests/comment_layout_test.cpp).
//
// Every row has a header line (author, time, likes). Collapsed rows show at most
// `collapsedLines` lines of the message. The selected row can be expanded: it shows the whole
// message (scrolled inside the row when it is taller than the viewport), up to two reply
// previews and a "view replies" line.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace pspapp {

struct CommentRowSpec {
    int bodyLines = 1;   // message lines at the list width
    int previews = 0;    // reply previews available (at most two are drawn)
    int replies = 0;     // total replies, for the "view replies" line
};

struct CommentLayoutParams {
    int lineHeight = 18;
    int viewportHeight = 180;
    int collapsedLines = 2;
    int selected = 0;
    bool expanded = false;
    int inner = 0;       // first visible message line of the expanded row
};

constexpr int kCommentHeader = 16, kCommentPadTop = 4, kCommentPadBottom = 6;
constexpr int kCommentPreviewLine = 14, kCommentRepliesLine = 15, kCommentExtrasGap = 4;

struct CommentLayout {
    std::vector<int> top, height;  // per row, relative to the start of the list
    int total = 0;
    int visibleBody = 0;           // message lines drawn for the selected row when expanded
    int maxInner = 0;              // largest useful scroll inside the expanded row
    int scrollTarget = 0;          // list scroll that brings the selected row into view
};

inline int commentExtras(const CommentRowSpec& row) {
    const int previews = std::min(row.previews, 2);
    if (!previews && !row.replies) return 0;
    return previews * kCommentPreviewLine + (row.replies ? kCommentRepliesLine : 0) + kCommentExtrasGap;
}

inline CommentLayout layoutComments(const std::vector<CommentRowSpec>& rows, const CommentLayoutParams& p) {
    CommentLayout out;
    out.top.resize(rows.size());
    out.height.resize(rows.size());
    int y = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const CommentRowSpec& row = rows[i];
        const bool open = p.expanded && static_cast<int>(i) == p.selected;
        int lines = std::min(row.bodyLines, p.collapsedLines);
        int extras = 0;
        if (open) {
            extras = commentExtras(row);
            const int room = p.viewportHeight - kCommentPadTop - kCommentHeader - kCommentPadBottom - extras;
            const int fit = std::max(1, room / p.lineHeight);
            lines = std::min(row.bodyLines, fit);
            out.visibleBody = lines;
            out.maxInner = std::max(0, row.bodyLines - lines);
        }
        out.top[i] = y;
        out.height[i] = kCommentPadTop + kCommentHeader + std::max(1, lines) * p.lineHeight + extras + kCommentPadBottom;
        y += out.height[i];
    }
    out.total = y;
    if (p.selected >= 0 && p.selected < static_cast<int>(rows.size())) {
        // The selected row sits at the top of the viewport (no half-cut line of the previous row).
        const int target = out.top[p.selected];
        out.scrollTarget = std::max(0, std::min(target, std::max(0, out.total - p.viewportHeight)));
    }
    return out;
}

} // namespace pspapp
