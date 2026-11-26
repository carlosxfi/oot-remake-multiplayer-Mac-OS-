"""The SOLO boot, ONE place for every test that needs it (owner rule: non-multiplayer features must work solo, co-op
idle): `--solo` boots ONLY lane game A, with no co-op session: no server started, no LICHII_COOP_SERVER, no Host or
Join (bt_plugin boot_args: live.py start --only A without --server). The pair then has game A only (pair.c == {'A'};
step_both / press / warp_both / check_shot run on A).

A test marked with `MARK` runs only in a --solo run and is DESELECTED in any other ('deselected: needs_solo ...'). In
a --solo run every test WITHOUT the mark is deselected too ('needs_pair'): it needs both games. The setup proof
(bt_plugin lane_pair, before any test; again in prove()) is game A's module log line IDLE.
"""
import re

import pytest

MARK = pytest.mark.needs_solo
GAME = "A"
# hookshot/src/coop_hookshot.cpp (module start, no server in coop.ini or LICHII_COOP_SERVER, the Multiplayer tab there):
IDLE = "coop: no server in coop.ini: co-op idle until Host or Join on the menu's Multiplayer tab"
# A session's lines (a lane run's game-out-A.txt, shots-lane2/hs-volvagia-rocks/20261125-114351: "coop: on, server
# 127.0.0.1:27530, ...", "coop: connecting", "coop: welcomed as player 2 ...", "coop: connected"): none may be there.
CONNECTED = re.compile(r"coop: (on, server |connecting|welcomed |connected)")


def idle_in(text):
    """Whether a module log shows co-op idle (IDLE) and no session line anywhere."""
    return IDLE in text and not CONNECTED.search(text)


def prove(pair):
    """Setup proof (inside pair.setup()): only game A runs, and its co-op is idle."""
    assert set(pair.c) == {GAME}, "a solo boot has game A only, this pair has %s" % sorted(pair.c)
    with open(pair.games[GAME]["log"], encoding="utf-8", errors="replace") as f:
        text = f.read()
    assert idle_in(text), "game A's module log is not co-op idle (no %r, or a session line): not solo" % IDLE
