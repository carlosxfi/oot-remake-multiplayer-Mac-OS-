// SPDX-License-Identifier: GPL-3.0-or-later
// The chat log (lichii/hook/coop_chat.h): the banner reads the last kKeep lines, the session's history (row 282)
// the last kHistory.
#include <doctest/doctest.h>

#include <string>

#include "coop_chat.h"

TEST_CASE("chat log: the banner's last lines and the session's history, oldest first") {
    CoopChat::Log log;
    CHECK(log.Lines().empty());
    CHECK(log.History().empty());
    log.Add("Saria: hi", false);
    log.Add("Mido joined", true);
    REQUIRE(log.History().size() == 2);
    CHECK(log.History()[0].text == "Saria: hi");
    CHECK(log.History()[1].notice);
    CHECK(log.Lines().size() == 2);
    for (int i = 0; i < 150; ++i) log.Add("line " + std::to_string(i), false);
    // The ring: the history keeps the last kHistory, oldest first; the banner the last kKeep.
    const auto history = log.History();
    REQUIRE(history.size() == CoopChat::kHistory);
    CHECK(history.front().text == "line " + std::to_string(150 - static_cast<int>(CoopChat::kHistory)));
    CHECK(history.back().text == "line 149");
    const auto lines = log.Lines();
    REQUIRE(lines.size() == CoopChat::kKeep);
    CHECK(lines.front().text == "line " + std::to_string(150 - static_cast<int>(CoopChat::kKeep)));
    CHECK(lines.back().text == "line 149");
}
