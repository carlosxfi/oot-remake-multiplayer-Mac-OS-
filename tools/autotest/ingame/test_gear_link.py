"""gear-link (report 293, Snail parity P4): Link's Equipment shown or hidden, on your own Link and on your puppet in the
other game; with or without Character Select (C #132: the game's own shown bit, no mod texture).

| Test | What it proves | Kind |
|---|---|---|
| test_gear_hidden_own_and_puppet | part 29 hides A's gear on A's own Link and on A's puppet in B | log + LOOK (shots on each game) |
| test_gear_shown_again | Hidden off: the pieces come back on both | log + LOOK |
| test_gear_hidden_without_character_select | the same on a game without the mod | log + LOOK; runs only where a game lacks it |

Set-up and judging, each cited where used:
- `menu look link_gear=hidden|none`: test only, the module's MENU verb (lichii/hook/coop_testinput.cpp, hookshot
  coop_pause.cpp TestMenu "look "): the part chosen as the Customize page chooses it (ChooseLookParts) and worn as when
  the menu closes (WearChosenLook), so it goes on the wire as a player's look does.
- Log lines: lichii/hook/coop_char_colours.cpp Wardrobe::Frame says `characters: equipment hidden on <your model|a
  puppet> (N piece(s))` and `characters: equipment shown again on ...` (LOG_INFO "coop: {}", hookshot DressCharacters).
- Character Select present: the module's ModPresent is "its saved Int CharacterMenu_SelectedCharacter is there"
  (coop_char_colours.cpp NoteOwnPick -> SetModPresent(pick.has_value())); the bridge's flag_read of that name is the
  same read (docs/playtest.md "flag_read": kind `unknown`, value null when the game has no such flag).
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_rows import need_child_in_play, place_in_front   # noqa: E402  (the shared set-up helpers)

HIDDEN_ON = r"characters: equipment hidden on %s \((\d+) piece\(s\)\)"
SHOWN_ON = r"characters: equipment shown again on %s \((\d+) piece\(s\)\)"
OWN, PUPPET = "your model", "a puppet"


def has_character_select(pair, x):
    r = pair.flag_read(x, "CharacterMenu_SelectedCharacter")
    return r.get("kind") not in (None, "unknown") and r.get("value") is not None


def wears_a_sword(pair, x):
    """A piece in the hidden handles (the sword: handle 0, kGearHandles) so Hidden has something to hide; the state's
    equips.weapon is PlayerStatus.EquipWeapon (docs/playtest.md "State")."""
    e = pair.state(x).get("equips") or {}
    assert e.get("weapon"), "%s wears no sword (equips %s): Hidden would hide nothing" % (x, e)


def face_each_other(pair):
    place_in_front(pair, "A", of="B")
    pair.act("B", "face %.2f %.2f" % tuple(pair.state("A")["pos"][::2]))
    pair.step_both(10)


def set_gear(pair, value):
    r = pair.act("A", "menu look link_gear=%s" % value)
    assert r.get("ok", True), "A refused 'menu look link_gear=%s': %s" % (value, r)


def hide_and_check(pair, shot):
    mark_a, mark_b = pair.log["A"].mark(), pair.log["B"].mark()
    set_gear(pair, "hidden")
    pair.expect_log("A", HIDDEN_ON % OWN, max_frames=240)
    pair.expect_log("B", HIDDEN_ON % PUPPET, max_frames=600)   # (the look over the wire, then the puppet's frame)
    pair.step_both(30)
    assert pair.log["A"].find(HIDDEN_ON % OWN, pos=mark_a), "A: own Link's gear not hidden"
    assert pair.log["B"].find(HIDDEN_ON % PUPPET, pos=mark_b), "B: A's puppet's gear not hidden"
    pair.check_shot("A", shot)   # A's own Link, from A's camera
    pair.check_shot("B", shot)   # A's puppet, facing B


def show_and_check(pair, shot):
    set_gear(pair, "none")
    pair.expect_log("A", SHOWN_ON % OWN, max_frames=240)
    pair.expect_log("B", SHOWN_ON % PUPPET, max_frames=600)
    pair.step_both(30)
    pair.check_shot("A", shot)
    pair.check_shot("B", shot)


@pytest.mark.budget(180)
@pytest.mark.look
@pytest.mark.row("#293 P4 Link's Equipment hidden (own Link + puppet)")
def test_gear_hidden_own_and_puppet(pair):
    with pair.setup():
        need_child_in_play(pair)
        wears_a_sword(pair, "A")
        face_each_other(pair)
    try:
        hide_and_check(pair, "gear-link-hidden")
        # Hidden every frame (the game shows a piece again when it uses it): still hidden later, said once.
        pair.step_both(120)
        pair.check_shot("B", "gear-link-hidden-later")
    finally:
        set_gear(pair, "none")
        pair.step_both(30)


@pytest.mark.budget(180)
@pytest.mark.look
@pytest.mark.row("#293 P4 Link's Equipment shown again")
def test_gear_shown_again(pair):
    with pair.setup():
        need_child_in_play(pair)
        wears_a_sword(pair, "A")
        face_each_other(pair)
    hide_and_check(pair, "gear-link-hidden")
    show_and_check(pair, "gear-link-shown")


@pytest.mark.budget(180)
@pytest.mark.look
@pytest.mark.row("#293 P4 Link's Equipment hidden without Character Select (C #132)")
def test_gear_hidden_without_character_select(pair):
    with pair.setup():
        need_child_in_play(pair)
        cs = {x: has_character_select(pair, x) for x in ("A", "B")}
        if all(cs.values()):
            pytest.skip("NOT RUN: both lane games have Character Select (a lane without the mod is needed)")
        wears_a_sword(pair, "A")
        face_each_other(pair)
    try:
        hide_and_check(pair, "gear-link-no-cs-hidden")
    finally:
        set_gear(pair, "none")
        pair.step_both(30)
