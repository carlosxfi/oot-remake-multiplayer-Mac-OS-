// SPDX-License-Identifier: GPL-3.0-or-later
// Where co-op's menu is when F1 is pressed (hookshot/src/coop_menu_tabs_rule.h, f1-event-c31: regression C3-1).
#include <doctest/doctest.h>

#include "coop_menu_tabs_rule.h"

using namespace CoopMenuTabsRule;

namespace {

constexpr int kTabs = 3;   // (Multiplayer, Lobby, Mods)

Now At(bool minus_tabs, bool minus_now, bool tab_shown) {
    Now now;
    now.minus_tabs = minus_tabs;
    now.minus_now = minus_now;
    now.tab_shown = tab_shown;
    return now;
}

}  // namespace

TEST_CASE("menu tabs rule: free play with our - tabs: F1 is the - menu, Longshot's menu has no co-op tab (owner O5)") {
    const Now free = At(true, true, false);
    CHECK(OnF1(free) == F1::kMinusMenu);
    CHECK(LongshotTabs(free, kTabs) == 0);
    CHECK(F6ToMinusMenu(free));
}

TEST_CASE("menu tabs rule: an event, a load, the title, a file screen: Longshot's menu with co-op's tabs, as 0.11.12") {
    // (C3-1: rc-01113 decided by pack only: F1 reached a Longshot menu with no co-op tab, no Leave from a stuck event)
    const Now event = At(true, false, false);
    CHECK(LongshotTabs(event, kTabs) == kTabs);
    CHECK(OnF1(event) == F1::kLongshotOnCoop);   // (opened on Multiplayer, not on Longshot's last tab)
    CHECK_FALSE(F6ToMinusMenu(event));           // (F6: the held list, not nothing)
}

TEST_CASE("menu tabs rule: a co-op tab showing stays while it shows, and F1 then closes Longshot's menu") {
    const Now event_shown = At(true, false, true);
    CHECK(LongshotTabs(event_shown, kTabs) == kTabs);
    CHECK(OnF1(event_shown) == F1::kLongshot);
    // The event ended with the player on Multiplayer in Longshot's menu: the tabs stay (no page pulled from under them).
    const Now free_shown = At(true, true, true);
    CHECK(LongshotTabs(free_shown, kTabs) == kTabs);
    CHECK(OnF1(free_shown) == F1::kMinusMenu);   // (free play again: F1 is co-op's home, Longshot's menu closed)
    // On Longshot's own tab when free play comes back: co-op's tabs leave the strip.
    CHECK(LongshotTabs(At(true, true, false), kTabs) == 0);
}

TEST_CASE("menu tabs rule: no pack or a pack without our - tabs: Longshot's menu with co-op's tabs, F1 Longshot's") {
    for (const bool minus_now : {false, true}) {
        for (const bool shown : {false, true}) {
            const Now old = At(false, minus_now, shown);
            CHECK(LongshotTabs(old, kTabs) == kTabs);
            CHECK(OnF1(old) == F1::kLongshot);
            CHECK_FALSE(F6ToMinusMenu(old));
        }
    }
}

TEST_CASE("menu tabs rule: a co-op tab is reachable from F1 in every state (never a Longshot menu without one)") {
    for (const bool minus_tabs : {false, true}) {
        for (const bool minus_now : {false, true}) {
            for (const bool shown : {false, true}) {
                const Now now = At(minus_tabs, minus_now, shown);
                const F1 f1 = OnF1(now);
                const bool reachable = f1 == F1::kMinusMenu || LongshotTabs(now, kTabs) > 0;
                CHECK(reachable);
            }
        }
    }
}
