// SPDX-License-Identifier: GPL-3.0-or-later
// Row 280 (hookshot/src/coop_part_fallback.h): the line the kit's part check wrote for a mod part with files left out
// (the game's own used), read for the banner and the Mods tab, and shown once each time play starts from the title.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mod_temp.h"
#include "coop_part_fallback.h"

namespace {

void Put(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

}  // namespace

TEST_CASE("part fallback: each named mod's first line, in order, none without the file") {
    const auto root = ModTestDir("part-fallback");
    Put(root / "OoT Character Select" / "coop-fallback.txt",
        "Character Select: the game's own Player_Adult used: the mod's need a file it does not have.\r\nmore\n");
    Put(root / "Other" / "romfs" / "x.bin", "x");
    Put(root / "Empty" / "coop-fallback.txt", "");
    Put(root / "Off" / "coop-fallback.txt", "Off: the game's own A used: the mod's need a file it does not have.\n");
    const auto lines = CoopPartFallback::Lines(root, {"Other", "OoT Character Select", "Empty", "Missing", ""});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "Character Select: the game's own Player_Adult used: the mod's need a file it does not have.");
    // (only the mods named: the module names the ones the running game uses)
    CHECK(CoopPartFallback::Lines(root, {"Other"}).empty());
    std::filesystem::remove_all(root);
}

TEST_CASE("part fallback: only printable English is shown, cut to its limit") {
    const auto root = ModTestDir("part-fallback-clean");
    Put(root / "A" / "coop-fallback.txt", std::string("bad \x1b[2J line\n"));
    Put(root / "B" / "coop-fallback.txt", std::string(400, 'x') + "\n");
    Put(root / "C" / "coop-fallback.txt", "caf\xc3\xa9\n");
    const auto lines = CoopPartFallback::Lines(root, {"A", "B", "C"});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == std::string(CoopPartFallback::kMaxBytes, 'x'));
    std::filesystem::remove_all(root);
}

TEST_CASE("part fallback: once each time play starts from the title") {
    CoopPartFallback::OncePerLoad once;
    CHECK_FALSE(once.Step(true, false));   // title
    CHECK_FALSE(once.Step(false, false));  // loading the file: no Link yet
    CHECK(once.Step(false, true));         // Link in the world: once
    CHECK_FALSE(once.Step(false, true));
    CHECK_FALSE(once.Step(false, false));  // a scene load in play: not again
    CHECK_FALSE(once.Step(false, true));
    CHECK_FALSE(once.Step(true, false));   // back to the title, then play again: once more
    CHECK(once.Step(false, true));
    // (the module started in play: the first frame with Link counts)
    CoopPartFallback::OncePerLoad late;
    CHECK(late.Step(false, true));
}
