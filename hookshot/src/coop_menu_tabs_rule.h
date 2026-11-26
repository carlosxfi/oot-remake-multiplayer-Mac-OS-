// SPDX-License-Identifier: GPL-3.0-or-later
// Where co-op's menu is when F1 (or Insert, Esc, a pad's menu key) is pressed: the game's - menu or Longshot's F1 menu
// (f1-event-c31, regression C3-1 of rc-01113). menu-polish-01113 moved co-op's tabs (Multiplayer, Lobby, Mods) from
// Longshot's menu to the game's - menu, decided once by the pack (one with our - tabs: none left in Longshot's menu).
// But the - menu opens only in free play or over a game menu: in a cutscene, a talk, a load, on the title or a file
// screen it cannot, and F1 then reached a Longshot menu with no co-op tab (no Leave from a stuck event; 0.11.12 always
// had them there). The decision is by NOW: whenever our - menu cannot open now, Longshot's menu carries co-op's tabs
// again, as in 0.11.12, and F1 opens it on Multiplayer; in free play F1 stays the - menu's Multiplayer tab (owner, O5).
// Pure: coop_hookshot.cpp (hos_coop_menu_tabs, hos_coop_key) feeds it; tests/hookshot/menu_tabs_rule_test.cpp.
#pragma once

namespace CoopMenuTabsRule {

struct Now {
    bool minus_tabs = true;   // the pack has our - tabs (has_pack and no - menu seen without them: NoMinusTabs)
    bool minus_now = false;   // our - menu can open now: free play (our Link in a scene, no event, no load) or a game menu up
    bool tab_shown = false;   // one of co-op's tabs shows in Longshot's menu now (HOS_COOP_MENU_SHOW .. _HIDE)
};

// How many of co-op's tabs Longshot's menu shows (`tabs`: CoopPause::Tabs()). A tab that shows stays while it does,
// so an event that ends with the player on Multiplayer does not pull the page from under them.
constexpr int LongshotTabs(const Now& now, int tabs) {
    return !now.minus_tabs || !now.minus_now || now.tab_shown ? tabs : 0;
}

enum class F1 {
    kLongshot,          // F1 left to Longshot: it opens or closes its own menu (a pack without our - tabs, as before;
                        // or one of co-op's tabs shows: F1 closes the menu, as Longshot's F1 does)
    kMinusMenu,         // the game's - menu on co-op's Multiplayer tab (CoopPauseTab::OpenMultiplayer), free play
    kLongshotOnCoop,    // Longshot's menu opened on co-op's Multiplayer tab (hos_coop_menu_wanted), F1 taken
};

constexpr F1 OnF1(const Now& now) {
    if (!now.minus_tabs) return F1::kLongshot;
    if (now.minus_now) return F1::kMinusMenu;
    return now.tab_shown ? F1::kLongshot : F1::kLongshotOnCoop;
}

// F6 (Players): the - menu's Players view when it can open now; else F6's held list (the one a pack without our - tabs
// uses), not nothing (0.11.12-rc logged "nothing pressed" and showed nothing outside free play).
constexpr bool F6ToMinusMenu(const Now& now) { return now.minus_tabs && now.minus_now; }

}  // namespace CoopMenuTabsRule
