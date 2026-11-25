// SPDX-License-Identifier: GPL-3.0-or-later
// The session's chat to look back at (hookshot/src/coop_chat_history.h, player report 282).
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "coop_chat_history.h"
#include "coop_text_fit.h"

using namespace CoopChatHistory;

TEST_CASE("chat history: a short line is one row, a long one goes on at whole words") {
    CHECK(Rows({"Saria: hello", "Mido joined"}, 46) == std::vector<std::string>{"Saria: hello", "Mido joined"});
    const std::vector<std::string> rows = Rows({"Saria: one two three four five six seven"}, 16);
    REQUIRE(rows.size() >= 3);
    CHECK(rows[0] == "Saria: one two");
    for (std::size_t i = 1; i < rows.size(); ++i) CHECK(rows[i].rfind("  ", 0) == 0);   // (its later rows indented)
    std::string joined;
    for (const std::string& r : rows) {
        CHECK(r.size() <= 16);
        CHECK(CoopTextFit::Halves(r) <= 32);
        joined += (joined.empty() ? "" : " ") + r.substr(r.rfind("  ", 0) == 0 ? 2 : 0);
    }
    CHECK(joined == "Saria: one two three four five six seven");   // (nothing lost, nothing doubled)
    // (no spaces: cut between letters, never inside one; wide letters count double)
    const std::vector<std::string> w = Rows({std::string(40, 'W')}, 16);
    for (const std::string& r : w) CHECK(CoopTextFit::Halves(r) <= 32);
    const std::string zh = "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xBD\xA0\xE5\xA5\xBD"
                           "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xBD\xA0\xE5\xA5\xBD";   // (12 CJK letters)
    for (const std::string& r : Rows({zh}, 10)) {
        CHECK(CoopTextFit::Halves(r) <= 20);
        CHECK((r.size() - (r.rfind("  ", 0) == 0 ? 2 : 0)) % 3 == 0);   // (whole letters)
    }
    CHECK(Rows({""}, 46) == std::vector<std::string>{""});
    CHECK(Rows({}, 46).empty());
}

TEST_CASE("chat history: it opens on the newest page, ZL older, ZR newer, round") {
    std::vector<std::string> rows;
    for (int i = 0; i < 14; ++i) rows.push_back("line " + std::to_string(i));
    CHECK(Pages(14) == 3);
    CHECK(Pages(0) == 1);
    CHECK(Shown(kNewest, 3) == 2);   // ("Page 3/3")
    CHECK(Shown(0, 3) == 0);
    CHECK(Shown(5, 3) == 2);          // (a page beyond the last: the oldest lines went meanwhile)
    // Pages from the oldest; the newest holds what is left.
    CHECK(PageRows(rows, 0) == std::vector<std::string>{"line 0", "line 1", "line 2", "line 3", "line 4", "line 5"});
    CHECK(PageRows(rows, 1) == std::vector<std::string>{"line 6", "line 7", "line 8", "line 9", "line 10", "line 11"});
    CHECK(PageRows(rows, kNewest) == std::vector<std::string>{"line 12", "line 13"});
    CHECK(PageRows(rows, 2) == PageRows(rows, kNewest));
    CHECK(Step(kNewest, 3, -1) == 1);   // (ZL: older)
    CHECK(Step(1, 3, -1) == 0);
    CHECK(Step(0, 3, -1) == kNewest);   // (ZL on the oldest: round to the newest, which follows new lines again)
    CHECK(Step(kNewest, 3, 1) == 0);    // (ZR on the newest: the oldest)
    CHECK(Step(1, 3, 1) == kNewest);
    CHECK(Step(kNewest, 1, -1) == kNewest);   // (one page: it stays)
    // An older page looked at keeps its rows while lines come; the newest follows them.
    for (int i = 14; i < 20; ++i) rows.push_back("line " + std::to_string(i));
    CHECK(PageRows(rows, 1).front() == "line 6");
    CHECK(PageRows(rows, kNewest) == std::vector<std::string>{"line 18", "line 19"});
    CHECK(PageRows({}, kNewest).empty());
    CHECK(PageRows({"a", "b"}, 0) == std::vector<std::string>{"a", "b"});
}
