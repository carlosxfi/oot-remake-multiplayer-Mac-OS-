"""Fake-game reactions for row 285's port (ingame/test_levitate.py; owner port-sweep-c131-8). Lines are the module's
(test_levitate.py cites each writer); places are the old scenario's (test_levitate.py constants).

- B pressing A in InsideTheDekuTree within 2 m of the map chest (6.51, 7.25; B stands 1.53 m off it), the first time:
  the chest's get scene (B's state `event` on for 600 frames) and A logs 'coop: puppet: P2 show -> hold (their cutscene)' 20 frames later,
  'P2 hold -> show' when it ends.
Selftest switch BT_FAKE_LEVITATE_BUG (tests/test_ingame_levitate.py): nohold -> A never holds B's puppet (no hold
line); opened -> the chest was opened before (no get scene); nochar -> B's character flag does not take.
"""
import math
import os

SCENE, CHEST = "InsideTheDekuTree", (6.51, 7.25)
CHAR_FLAG = "CharacterMenu_SelectedCharacter"
EVENT_FRAMES = 600
HOLD = "coop: puppet: P2 show -> hold (their cutscene)"
SHOW = "coop: puppet: P2 hold -> show"


def _bug():
    return os.environ.get("BT_FAKE_LEVITATE_BUG", "")


def setup(game):
    game.lev = {"opened": _bug() == "opened", "event": False}


def _end(game):
    game.lev["event"] = False
    game.other.say(SHOW)


def press(game, buttons):
    if game.tag != "B" or game.scene != SCENE or "A" not in buttons or game.lev["opened"]:
        return False
    if math.hypot(game.pos[0] - CHEST[0], game.pos[2] - CHEST[1]) > 2.0:
        return False
    game.lev["opened"], game.lev["event"] = True, True
    if _bug() != "nohold":
        game.other.later(20, lambda: game.other.say(HOLD))
    game.later(EVENT_FRAMES, lambda: _end(game))
    return True


def after(game, m, r):
    if m.get("cmd") == "flag" and _bug() == "nochar" and m.get("name") == CHAR_FLAG and game.tag == "B":
        game.flags[CHAR_FLAG] = r["value"] = 0


def state(game, st):
    if game.tag == "B" and game.lev["event"]:
        st["event"] = True
