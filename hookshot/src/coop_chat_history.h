// SPDX-License-Identifier: GPL-3.0-or-later
// The session's chat to look back at (player report 282: "Is it no longer possible to view the in-game chat
// history?"). The banner shows each line once, for a few seconds (coop_native_ui.h, ChatFeed); the log keeps the
// session's last CoopChat::kHistory lines (100: chat and notices, as the banner shows both; lichii/hook/coop_chat.h),
// and a new session starts with none (the log is the session's: CoopHook::ChatHistory).
// Its one home: the - menu's Players view (the Multiplayer tab's Players row), whose list on the right of the page
// (the game's own Options rows, Pa_List_00..06) had nothing on it. Its heading "Chat  Page 3/3" with the game's own ZL
// and ZR icons beside it, as every co-op list pages (coop_list_pages.h); a page is kRows rows. A line too long for a
// row goes on over the next rows (whole words; its later rows start with two spaces, so a line's start is plain).
// It opens on the newest page (the last), and stays on the newest as lines come; ZL goes back to older lines, ZR on,
// round at both ends. Pages are counted from the oldest, so an older page looked at keeps its rows while lines come.
// Logic only: tests/hookshot/chat_history_test.cpp.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "coop_list_pages.h"
#include "coop_text_fit.h"

namespace CoopChatHistory {

inline constexpr int kRows = 6;   // the list's rows under its heading (coop_pause_tab.h kCells)

// The lines as rows of at most `n` letters (CoopTextFit's widths and bytes): a long line on over the next rows at
// whole words, its later rows after two spaces. An empty line: an empty row.
inline std::vector<std::string> Rows(const std::vector<std::string>& lines, std::size_t n) {
    std::vector<std::string> rows;
    n = std::max<std::size_t>(n, 8);
    for (const std::string& line : lines) {
        if (line.empty()) {
            rows.emplace_back();
            continue;
        }
        std::size_t at = 0;
        bool first = true;
        while (at < line.size()) {
            const std::size_t room = first ? n : n - 2;
            const std::size_t end = CoopTextFit::LineEnd(line, at, room);
            std::string row = line.substr(at, end - at);
            while (!row.empty() && row.back() == ' ') row.pop_back();
            rows.push_back(first ? row : "  " + row);
            first = false;
            at = end;
            while (at < line.size() && line[at] == ' ') ++at;   // (the space it broke at: not at a row's start)
        }
    }
    return rows;
}

inline int Pages(int rows) { return CoopListPages::Pages(rows, kRows); }

// The page shown (0 the oldest, as "Page 1/N" counts) for the page kept, `at`: kNewest follows the newest page (the
// last) as lines come; a page beyond the last (the oldest lines dropped meanwhile) is the last.
inline constexpr int kNewest = -1;
inline int Shown(int at, int pages) {
    pages = std::max(1, pages);
    return at < 0 || at >= pages ? pages - 1 : at;
}

// ZL (dir < 0: older) or ZR (dir > 0: newer) from the page kept `at`: the page to keep, round at both ends (the
// newest page: kNewest again, so it follows new lines).
inline int Step(int at, int pages, int dir) {
    pages = std::max(1, pages);
    const int next = CoopListPages::Step(Shown(at, pages), pages, dir);
    return next == pages - 1 ? kNewest : next;
}

// The rows on the page `at` (Shown): kRows a page from the oldest, so a page looked at keeps its rows while new
// lines come (they go on the newest page); the newest page holds what is left.
inline std::vector<std::string> PageRows(const std::vector<std::string>& rows, int at) {
    const int count = static_cast<int>(rows.size());
    const int start = Shown(at, Pages(count)) * kRows;
    const int end = std::min(count, start + kRows);
    return {rows.begin() + start, rows.begin() + std::max(start, end)};
}

}  // namespace CoopChatHistory
