// SPDX-License-Identifier: GPL-3.0-or-later
// Local co-op started from the game (hookshot/src/coop_local_start.h): this game's player number, the session an extra
// game was started into, the launcher's answer and the line the page shows for it, and the launcher's command line.
#include <doctest/doctest.h>

#include <set>
#include <string>

#include "coop_local_start.h"

using namespace CoopLocalStart;

TEST_CASE("local start: the player number comes from the game's folder") {
    CHECK(PlayerOf(std::filesystem::path("C:/kit/hookshot/game")) == 1);
    CHECK(PlayerOf(std::filesystem::path("C:/kit/hookshot/game/local-players/player2")) == 2);
    CHECK(PlayerOf(std::filesystem::path("C:/kit/hookshot/game/local-players/Player4/")) == 4);   // (case, a slash)
    CHECK(PlayerOf(std::filesystem::path("C:/kit/hookshot/game/local-players/player1")) == 1);   // (game 1 is the kit's)
    CHECK(PlayerOf(std::filesystem::path("C:/kit/hookshot/game/nucleus-players/player2")) == 1);   // (Nucleus: not ours)
    CHECK(PlayerOf(std::filesystem::path("C:/kit/hookshot/game/local-players/player22")) == 1);
}

TEST_CASE("local start: an extra game's session is its coop.ini's 127.0.0.1, port and password") {
    const auto s = SessionOf("# Local co-op\r\nserver = 127.0.0.1\r\nport = 27561\r\nname = Player 2\r\npassword = abc\r\n");
    REQUIRE(s);
    CHECK(s->port == 27561);
    CHECK(s->password == "abc");
    const auto colon = SessionOf("server = localhost:27562\npassword = x\n");
    REQUIRE(colon);
    CHECK(colon->port == 27562);
    CHECK_FALSE(SessionOf("server = 203.0.113.5\nport = 27561\npassword = abc\n"));   // (a friend's: not this PC's)
    CHECK_FALSE(SessionOf("server = 127.0.0.1\nport = 27561\n"));                     // (no password: not the launcher's)
    CHECK_FALSE(SessionOf(""));
}

TEST_CASE("local start: instances.txt gives the session's games") {
    CHECK(GamesListed("2=C:\\g\\local-players\\player2\r\n3=C:\\g\\local-players\\player3\r\n") == 3);
    CHECK(GamesListed("2=a\n") == 2);
    CHECK(GamesListed("") == 0);
    CHECK(GamesListed("x=1\n") == 0);
}

TEST_CASE("local start: the launcher's answer and the page's line for it") {
    CHECK_FALSE(ParseResult(""));   // (not written yet)
    const auto all = ParseResult("started=2,3\r\nrunning=\r\nfailed=\r\nproblem=\r\n");
    REQUIRE(all);
    CHECK(all->started == std::vector<int>{2, 3});
    CHECK(ResultNote(*all) == "Started: player 2 and 3. They join soon.");
    // fewer than asked: one did not start (the most important line), one ran already
    const auto some = ParseResult("started=2\r\nrunning=4\r\nfailed=3\r\nproblem=Player 3's game did not start: x\r\n");
    REQUIRE(some);
    CHECK(some->running == std::vector<int>{4});
    CHECK(some->failed == std::vector<int>{3});
    CHECK(some->problem == "Player 3's game did not start: x");
    CHECK(ResultNote(*some) == "Did not start: player 3.");
    // a game closed and started again: only the missing one; all running already
    const auto none = ParseResult("started=\r\nrunning=2,3\r\nfailed=\r\nproblem=\r\n");
    REQUIRE(none);
    CHECK(ResultNote(*none) == "Every game runs already.");
    const auto stopped = ParseResult("started=\r\nrunning=\r\nfailed=\r\nproblem=The launcher stopped: x\r\n");
    REQUIRE(stopped);
    CHECK(ResultNote(*stopped) == "Not started: see Start's help.");
    // every line fits a native list line (CoopPauseTab::kListLineChars, 46)
    auto worst = ParseResult("started=2,3,4\nrunning=\nfailed=2,3,4\nproblem=\n");
    REQUIRE(worst);
    CHECK(ResultNote(*worst).size() <= 46);
    worst->failed.clear();
    CHECK(ResultNote(*worst).size() <= 46);
}

TEST_CASE("local start: a session password is 12 letters and digits, another each time") {
    std::set<std::string> seen;
    for (std::uint64_t seed : {1ull, 2ull, 0ull, 0xdeadbeefull, 1ull << 40}) {
        const std::string p = NewPassword(seed);
        CHECK(p.size() == 12);
        for (const char c : p) CHECK(((c >= 'a' && c <= 'z') || (c >= '2' && c <= '9')));
        seen.insert(p);
    }
    CHECK(seen.size() == 5);
}

TEST_CASE("local start: the launcher's command line keeps a folder with spaces one argument") {
    Session s;
    s.port = 27560;
    s.password = "pw2abc";
    const std::string cmd = CommandLine(std::filesystem::path("C:/OOT REMAKE/kit"),
                                        std::filesystem::path("C:/OOT REMAKE/kit/hookshot/game/longshot.exe"), 3, s,
                                        4242, std::filesystem::path("C:/OOT REMAKE/kit/hookshot/game/local-players/r.txt"));
    CHECK(cmd.rfind("powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"", 0) == 0);
    CHECK(cmd.find("local-coop-start.ps1\"") != std::string::npos);
    CHECK(cmd.find(" -Kit \"C:") != std::string::npos);
    CHECK(cmd.find("OOT REMAKE") != std::string::npos);
    CHECK(cmd.find(" -Players 3 -Port 27560 -Password \"pw2abc\" -HostPid 4242 -Result \"") != std::string::npos);
    // more than the most: the most
    CHECK(CommandLine("k", "e", 9, s, 0, "r").find(" -Players 4 ") != std::string::npos);
}
