"""vest-link (report 293, Snail parity P2, C #132): Link's Vest colour (part 27), painted by co-op on Link's own vest
textures, on your own Link and on your puppet in the other game; unset it is the tunic's colour (0.11.12 exactly).

| Test | What it proves | Kind |
|---|---|---|
| test_vest_own_and_puppet | a Vest colour on A's own Link and on A's puppet in B, then another colour | log + LOOK (hue + shots on each game) |
| test_vest_unset_is_tunic | Vest back to unset: the tunic's painting again, the picture as before | log + LOOK |
| test_vest_without_character_select | the same with B booted without the Character Select mod (both ways) | log + LOOK; needs_env |

Set-up and judging, each cited where used:
- `menu look link_vest=#RRGGBB|none`: test only, the module's MENU verb (lichii/hook/coop_testinput.cpp MENU "look ",
  hookshot coop_pause.cpp TestMenu "look "): the part chosen as the Customize page chooses it (ChooseLookParts) and worn
  as when the menu closes (WearChosenLook), so it goes on the wire as a player's look does. The bridge lower-cases the
  line; the colour is read by CoopColors::Parse (lichii/hook/coop_colors.cpp, HexDigit takes either case).
- Own Link: the tunic wardrobe (host.what "tunic", hookshot/src/coop_look_tunic.h) logs `coop: look: your Link's tunic
  is <painting> now (...)` when it draws a new painting (coop_look_tunic.cpp Step); the painting name is
  CoopLookParts::TunicPainting: `tunic-<tunic|green>-vest-RRGGBB[-cap-RRGGBB]` with a vest, upper-case hex (Hex).
- Puppets: `coop: look: the puppets' tunics are <name> now` (coop_look_live.cpp FollowTunics, on a change only); the
  name is CoopLookParts::PuppetTunicsName: `ptunics|<colour>=RRGGBB[+capRRGGBB][+vestRRGGBB]...`.
- A is player 1, B player 2 (the lanes' host and joiner): A's tunic is its slot colour 0x3070FF (blue) and B's 0xFF3030
  (red) (coop/protocol messages.hpp slot_color), so yellow and green vests are far from both on the colour wheel.
- Colour: imagecheck.changed_hue, the pixels that changed between two shots of the same place and the share of them in
  the vest's hue (no guessed mask region). Shots also go to refs (NO-REF candidates for the owner).
- No Character Select: the ONE no-CS lane game, tools/autotest/ingame/no_cs.py (gear-link 812219312, C #132): MARK
  (needs_env B:COOP_TEST_NO_CHARACTER_SELECT=1, harness-c132's switch) and prove() (B has no mod, the module's
  ModPresent read through flag_read).
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import imagecheck   # noqa: E402
import no_cs        # noqa: E402  (the ONE no-Character-Select game)
from test_gear_link import face_each_other   # noqa: E402  (the shared set-up helpers)
from test_rows import need_child_in_play   # noqa: E402

YELLOW, GREEN = 0xFFD000, 0x20C020

OWN_VEST = r"coop: look: your Link's tunic is tunic-(?:green|[0-9A-F]{6})-vest-%06X\b\S* now"
OWN_NO_VEST = r"coop: look: your Link's tunic is (?!\S*-vest-)\S+ now"
PUP_VEST = r"coop: look: the puppets' tunics are \S*\+vest%06X\S* now"
PUP_NO_VEST = r"coop: look: the puppets' tunics are (?!\S*\+vest)\S+ now"

# Colour check tolerances (this test's, not the game's): a vest is a few hundred pixels of a 320-wide shot at 2.5 m;
# at least half of what changed must be the vest's hue (the rest: Link's idle sway, edges).
MIN_CHANGED, MIN_SHARE = 20, 0.5


def set_vest(pair, x, value):
    r = pair.act(x, "menu look link_vest=%s" % value)
    assert r.get("ok", True), "%s refused 'menu look link_vest=%s': %s" % (x, value, r)


def wait_since(pair, x, pattern, mark, max_frames):
    return pair.until("%s log /%s/" % (x, pattern), lambda: pair.log[x].find(pattern, pos=mark), max_frames)


def wear(pair, who, colour):
    """`who` wears the Vest `colour` (None: unset); waits for its own Link's line and the puppet's line on the other."""
    other = "B" if who == "A" else "A"
    marks = {x: pair.log[x].mark() for x in ("A", "B")}
    set_vest(pair, who, "none" if colour is None else "#%06X" % colour)
    own = OWN_NO_VEST if colour is None else OWN_VEST % colour
    pup = PUP_NO_VEST if colour is None else PUP_VEST % colour
    wait_since(pair, who, own, marks[who], 240)
    wait_since(pair, other, pup, marks[other], 600)   # (the look over the wire, then the puppets' tunics' round)
    pair.step_both(30)


def shots(pair, name):
    return {x: pair.check_shot(x, name)["path"] for x in ("A", "B")}


def assert_hue(before, after, colour, what):
    for x in ("A", "B"):
        h = imagecheck.changed_hue(before[x], after[x], colour)
        assert h["changed"] >= MIN_CHANGED, "%s %s: only %d pixels changed (vest not repainted?) %s" % (x, what, h["changed"], h)
        assert h["share"] >= MIN_SHARE, "%s %s: %.0f%% of the changed pixels are #%06X's hue (want %.0f%%) %s" % (
            x, what, h["share"] * 100, colour, MIN_SHARE * 100, h)


def unset_both(pair):
    for x in ("A", "B"):
        set_vest(pair, x, "none")
    pair.step_both(60)


@pytest.mark.budget(240)
@pytest.mark.look
@pytest.mark.row("#293 P2 Link's Vest colour (own Link + puppet)")
def test_vest_own_and_puppet(pair):
    with pair.setup():
        need_child_in_play(pair)
        face_each_other(pair)
        unset_both(pair)
    try:
        before = shots(pair, "vest-link-unset")
        wear(pair, "A", YELLOW)
        yellow = shots(pair, "vest-link-yellow")
        assert_hue(before, yellow, YELLOW, "unset -> yellow")
        wear(pair, "A", GREEN)
        green = shots(pair, "vest-link-green")
        assert_hue(yellow, green, GREEN, "yellow -> green")
    finally:
        unset_both(pair)


@pytest.mark.budget(240)
@pytest.mark.look
@pytest.mark.row("#293 P2 Link's Vest unset = the tunic's colour (0.11.12)")
def test_vest_unset_is_tunic(pair):
    with pair.setup():
        need_child_in_play(pair)
        face_each_other(pair)
        unset_both(pair)
    try:
        before = shots(pair, "vest-link-unset")
        wear(pair, "A", YELLOW)
        wear(pair, "A", None)
        back = shots(pair, "vest-link-unset-again")
        for x in ("A", "B"):
            score, err = imagecheck.ssim(before[x], back[x])
            assert not err and score >= 0.90, "%s: unset again is not the picture before (SSIM %s %s)" % (x, score, err)
            h = imagecheck.changed_hue(before[x], back[x], YELLOW)
            assert h["coloured"] < MIN_CHANGED, "%s: yellow left after unset %s" % (x, h)
    finally:
        unset_both(pair)


@pytest.mark.budget(300)
@pytest.mark.look
@no_cs.MARK
@pytest.mark.row("#293 P2 Link's Vest without Character Select (C #132)")
def test_vest_without_character_select(pair):
    with pair.setup():
        need_child_in_play(pair)
        no_cs.prove(pair)
        face_each_other(pair)
        unset_both(pair)
    try:
        before = shots(pair, "vest-link-no-cs-unset")
        wear(pair, "A", YELLOW)   # A (with the mod) -> its puppet in the CS-less B
        a_yellow = shots(pair, "vest-link-no-cs-a-yellow")
        assert_hue(before, a_yellow, YELLOW, "A's vest yellow")
        wear(pair, "A", None)
        unset = shots(pair, "vest-link-no-cs-unset-again")
        wear(pair, "B", GREEN)    # B's own Link without the mod -> its puppet in A (same places: A faces B already)
        b_green = shots(pair, "vest-link-no-cs-b-green")
        assert_hue(unset, b_green, GREEN, "B's vest green")
    finally:
        unset_both(pair)
