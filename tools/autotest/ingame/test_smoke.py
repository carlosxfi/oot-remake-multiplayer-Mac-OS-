"""The rc smoke tier (the first 10 minutes of an rc: a broken rc stops here): boot, co-op connect, warp, pause,
back to the title, load again. One boot per lane (bt_plugin.lane_pair); the session loads the save first
(live.py start: the runner's to_play, A until play), so 'menu -> load' is proven here by the title + load again test.

Sources of every value (worker-rules "no guessed values"):
- 'coop: connected': tools/autotest/groups/hookshot-smoke.txt [g1] walk's expect (the module's connect line).
- state members scene/loading/event/not_ready/coop.players: docs/playtest.md "State" (hookshot/src/coop_playtest.cpp).
- 'KakarikoVillage_Child': a map of Longshot's teleport list (villages; docs/playtest.md `warp`), the warp of
  9 boot scripts (scenarios/*.txt 'warp KakarikoVillage_Child ...').
- PLUS = the Switch '+' (the game's pause screen): coop_playtest_core.cpp kButtons; a held pause runs no Link update,
  so the snapshot goes stale (docs/playtest.md "State": 'a title, a pause or a load: no Link update runs').
- to-title.now + its two log lines: hookshot/src/coop_pause.cpp (LICHII_COOP_TEST_TO_TITLE=1, set by bt_plugin).
"""
import os

import pytest

pytestmark = pytest.mark.smoke

CONNECTED = r"coop: connected"
TO_TITLE_ON = r"coop: multiplayer tab: test: to-title\.now in .* starts the way back to the title"
TO_TITLE_ASKED = r"coop: multiplayer tab: test: back to the title asked \(to-title\.now\)"
WARP_TO = "KakarikoVillage_Child"


@pytest.mark.budget(60)
@pytest.mark.row("smoke: boot")
def test_boot(pair):
    with pair.setup():
        st = {x: pair.state(x) for x in ("A", "B")}
    for x, s in st.items():
        assert s.get("control") is True, "%s: Longshot's control bridge did not answer" % x
        assert pair.in_play(s), "%s not in play after the boot: scene=%s loading=%s event=%s not_ready=%s" % (
            x, s.get("scene"), s.get("loading"), s.get("event"), s.get("not_ready"))


@pytest.mark.budget(90)
@pytest.mark.row("smoke: co-op connect")
def test_connect(pair):
    for x in ("A", "B"):
        assert pair.log[x].find(CONNECTED, pos=0), "%s log has no '%s' since the start" % (x, CONNECTED)

    def both_listed():
        out = {}
        for x in ("A", "B"):
            players = ((pair.state(x).get("coop") or {}).get("players")) or []
            if len([p for p in players if p.get("state") == "connected"]) < 2:
                return None
            out[x] = players
        return out
    pair.until("both games list two connected players", both_listed, max_frames=600)


@pytest.mark.budget(150)
@pytest.mark.row("smoke: warp")
def test_warp(pair):
    st = pair.warp_both(WARP_TO)
    for x, s in st.items():
        assert pair.in_play(s), "%s after the warp: not in play (%s)" % (x, s.get("not_ready"))
        assert (s.get("scene") or "").lower() == WARP_TO.lower(), "%s is in %s, not %s" % (x, s.get("scene"), WARP_TO)


@pytest.mark.budget(90)
@pytest.mark.look
@pytest.mark.row("smoke: pause menu open / close (pause page shot on each game)")
def test_pause(pair):
    with pair.setup():
        for x in ("A", "B"):
            assert pair.in_play(pair.state(x)), "%s not in play before the pause" % x
    for x in ("A", "B"):
        pair.press(x, ["PLUS"], frames=4)
    pair.until("both games paused (no fresh Link sample)",
               lambda: all(pair.state(x).get("not_ready") is not None for x in ("A", "B")), max_frames=120)
    pair.step_both(30)                     # (the page's open animation; the shot is the open page)
    for x in ("A", "B"):
        pair.check_shot(x, "smoke-pause", tolerance=0.80)
    for x in ("A", "B"):
        pair.press(x, ["PLUS"], frames=4)
    pair.until("both games back in play after the second +",
               lambda: all(pair.in_play(pair.state(x)) for x in ("A", "B")), max_frames=300)


@pytest.mark.budget(300)
@pytest.mark.row("smoke: back to the title, menu -> load again")
def test_title_and_load_again(pair):
    with pair.setup():
        for x in ("A", "B"):
            assert pair.log[x].find(TO_TITLE_ON, pos=0), "%s: LICHII_COOP_TEST_TO_TITLE is not on (no '%s')" % (
                x, "to-title.now in ... starts")
    for x in ("A", "B"):
        with open(os.path.join(pair.games[x]["folder"], "to-title.now"), "w") as f:
            f.write("bt\n")
    for x in ("A", "B"):
        pair.expect_log(x, TO_TITLE_ASKED, max_frames=300)
    pair.until("both games left play (title)",
               lambda: all(not pair.in_play(pair.state(x)) for x in ("A", "B")), max_frames=900)
    pair.load_both(max_frames=3600)
    for x in ("A", "B"):
        s = pair.state(x)
        assert pair.in_play(s), "%s not in play after the load" % x
    pair.until("both games list two connected players again",
               lambda: all(len([p for p in ((pair.state(x).get("coop") or {}).get("players") or [])
                                if p.get("state") == "connected"]) >= 2 for x in ("A", "B")), max_frames=900)
