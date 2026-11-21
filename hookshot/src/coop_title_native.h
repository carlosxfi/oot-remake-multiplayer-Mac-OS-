// SPDX-License-Identifier: GPL-3.0-or-later
// The title's Host, Join and Co-op Settings on the title's own buttons (docs/hookshot.md "The title menu"): co-op's
// menu (coop_pause_menu.h, the overlay's model) opened on that page without its overlay (CoopPause::OpenNative), its
// rows shown three at a time on the title's first three buttons and Back on the fourth, in the pack's live texts
// (tools/puppet_pack/title_menu.py, Coop_Live_T0..T3). The pad's up and down move the menu's selection (the rows
// scroll with it), left and right change a value, A does the row the title's cursor is on, B goes back. A field the
// menu types into (a name, a port, a code) turns the four buttons into its keys (TypingLook): the game has no keyboard
// of its own (docs/game-ui.md, "Frames for a text field"), so nothing is drawn over the title.
//
// Logic only (tests/hookshot/title_native_test.cpp): what each button says and which row it is.
#pragma once

#include <array>
#include <string>

#include "coop_pause_menu.h"

namespace CoopTitleNative {

inline constexpr int kButtons = 4, kRowButtons = 3;   // (the fourth: Back)
inline constexpr std::size_t kTextBytes = 40;          // (a title button's width; the placeholder holds 63)

// A row's words on a title button: "Your name: Link", "Players when you host: < 4 >", "Voice: On", a note's line.
std::string RowText(const CoopPauseMenu::Row& r);

struct Look {
    std::array<std::string, kButtons> texts{};
    std::array<int, kButtons> rows{-1, -1, -1, -1};   // the menu's row each button shows (-1: none, or Back)
    int focus = 0;                                     // the button the menu's selected row is on
    int top = 0;                                       // the first row shown, among the rows shown (kept)
    int back = kButtons - 1;                           // Back's button: right under the last row
    int buttons = kButtons;                            // the title's buttons shown (the rows and Back)
};
// The buttons for the menu's view, the rows scrolled so its selected row shows (`top`: the last look's). Headings are
// left out; notes show (the menu never selects them).
Look LookFor(const CoopPauseMenu::View& v, int top);

// While a field is typed into: the first button shows the field ("Your name: Link_"), the others are its keys: Letter
// (its letter between arrows; left and right pick it, A puts it in), Delete (the letter before the cursor) and OK. Up
// and down move between the keys, B is Cancel (the field as it was), + is OK; the PC keyboard types as before.
enum class Key { kField, kLetter, kDelete, kOk };
inline constexpr int kFirstKey = static_cast<int>(Key::kLetter), kLastKey = static_cast<int>(Key::kOk);
// The letters Letter picks from, in order (the menu's field takes only its own: a port its digits, a name no control
// character): small letters, capitals, digits, the marks addresses and passwords use, then a space.
const std::string& Letters();
// The key `key` moved by `dy` (-1 up, +1 down), round the three keys.
int MoveKey(int key, int dy);
// The buttons while the menu's selected row is typed into, `key` focused.
Look TypingLook(const CoopPauseMenu::View& v, char letter, int key);

}  // namespace CoopTitleNative
