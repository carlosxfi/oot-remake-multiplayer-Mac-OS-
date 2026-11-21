// SPDX-License-Identifier: GPL-3.0-or-later
// No puppet pack (lichii/hook/coop_pack_notice.h): the chat says so once per
// game run, when another player is first shown, and never with a pack.
#include <doctest/doctest.h>

#include <string>

#include "coop_pack_notice.h"

TEST_CASE("pack notice: once, only without a pack and with someone shown") {
    CoopPackNotice::Once once;
    CHECK_FALSE(once.Due(false, false));  // alone: nothing to explain yet
    CHECK(once.Due(false, true));
    CHECK_FALSE(once.Due(false, true));   // once per run
    CoopPackNotice::Once with_pack;
    CHECK_FALSE(with_pack.Due(true, true));
}

TEST_CASE("pack notice: off until the module turns it on, then the line says what to do") {
    CHECK(CoopPackNotice::TakeLines(true).empty());  // the emulator never calls Enable
    CoopPackNotice::Enable(false);
    CHECK(CoopPackNotice::TakeLines(false).empty());
    const auto lines = CoopPackNotice::TakeLines(true);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("No puppet pack") == 0);
#ifdef _WIN32
    CHECK(lines[0].find("Start Squadala.bat") != std::string::npos);
#else
    CHECK(lines[0].find("start-squadala.sh") != std::string::npos);
#endif
    CHECK(CoopPackNotice::TakeLines(true).empty());
}
