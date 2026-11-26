"""Row 294 (player report: a guest won Epona; the host then could never win her; the next day, on the host's save,
neither game had a horse: stuck before Gerudo Valley and horseback archery). State checks through the bridge.

Root cause (lichii/hook/coop_quests.h, "Row 294"): owning Epona is the save's OwnedHorseList (Struct 0x4920b64f,
`OwnedHorseList.ActorName`, a String64Array of 10), written only by Animal_Epona_Adult.EventCopyAndRegisterOwnedHorse
in Get_Epona.bfevfl's escape subflows. Co-op shared Get_Epona's Complete without it: a game at Complete has no field
Epona and Ingo no longer races, so it could never win her. The fix: the hook registers her first (EnsureEpona,
coop_hook.cpp: ActorName[first empty] = Animal_Epona_Adult through the game's String64Array queue, the trace the
game's own registration leaves in a won save), then writes Complete; the quest catch-up (in a session) mends a save
already at Complete with no horse the same way.

Values (each cited):
- `Step_Get_Epona`: the quest's Enum, hash 0x92431c55 (coop_quests.cpp Table; the old scenarios' `setenum
  0x92431c55 Complete`, epona-own.B.txt:44). `Complete` hashes to 0x4c0a63f4 (coop_hook.cpp ride getter before row
  294; tests/lichii/quests_test.cpp "row 294" checks flag_hash("Complete")).
- `OwnedHorseList.ActorName[0]`: element 0 (bridge `flag_read` / `flag` NAME[i], docs/playtest.md); won saves hold
  `Animal_Epona_Adult` there (coord-multiplayer test-saves owner-worlds-1125 wrld_03 and saves-incoming
  oot-defeated-ganon slot_00, read from their progress.sav).
- The log lines: coop_hook.cpp EnsureEpona ("coop: story: Epona registered as this game's own horse
  (OwnedHorseList.ActorName[0] = Animal_Epona_Adult): <why> (row 294)") and the quests' raise line
  ("coop: quests: Get_Epona <from> -> Complete (shared Threads of Time; issue 193)", coop_hook.cpp take_raised).
- `Market_Child`: a warp the bridge took on 11-23 (test_row53.py SCENE), for the scene load in the mend test.
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_rows import need_child_in_play   # noqa: E402  (the pilot rows' start-state check)

QUEST = "Step_Get_Epona"
COMPLETE = "0x4c0a63f4"                    # flag_read's enum value: the hash, 0x and 8 hex digits
HORSE = "OwnedHorseList.ActorName[0]"
EPONA = "Animal_Epona_Adult"
REGISTERED = r"coop: story: Epona registered as this game's own horse \(OwnedHorseList\.ActorName\[0\] = " \
             r"Animal_Epona_Adult\): %s"
SHARED_WHY = r"another player won her"
MENDED_WHY = r"Get_Epona was Complete here with no owned horse \(mended\)"
RAISED = r"coop: quests: Get_Epona \S+ -> Complete"
SHARE_FRAMES = 600     # the watcher sends a change within frames; the end may be held one try (EnsureEpona queued)
MEND_FRAMES = 300      # the catch-up runs once a second (CoopEvery::kSecondUs) while no event runs
APPLY_FRAMES = 30      # the game's applier empties the queue at its next GameData pass


def quest(pair, x):
    r = pair.flag_read(x, QUEST)
    assert r.get("kind") == "enum", "%s: %s reads as %s (not the quest's Enum)" % (x, QUEST, r.get("kind"))
    return r.get("value")


def horse(pair, x):
    r = pair.flag_read(x, HORSE)
    assert r.get("kind") == "text", ("%s: %s reads as %s: this module has no String64Array read (row 294's "
                                     "flag_read NAME[i])" % (x, HORSE, r.get("kind")))
    return r.get("value")


def set_horse(pair, x, text):
    r = pair.flag(x, HORSE, text)
    assert r.get("value") == text, "%s: %s not set to %r: %s" % (x, HORSE, text, r)   # (an error raises BridgeError)


@pytest.mark.budget(180)
@pytest.mark.row("#294 a won Epona reaches the other game with her horse (fresh win, both games)")
def test_epona_won_reaches_the_other_with_the_horse(pair):
    with pair.setup():
        need_child_in_play(pair)
        for x in ("A", "B"):
            q = quest(pair, x)
            if q == COMPLETE:
                raise AssertionError("%s: %s is already Complete: this boot is not fresh (the group keeps the end; "
                                     "run this test first on a lane's boot)" % (x, QUEST))
            set_horse(pair, x, "")
        pair.step_both(APPLY_FRAMES)
        for x in ("A", "B"):
            assert horse(pair, x) == "", "%s: %s did not empty" % (x, HORSE)
        mark = {x: pair.log[x].mark() for x in ("A", "B")}
    # B wins her: what its own escape flow leaves, the horse registered (the game's EventCopyAndRegisterOwnedHorse),
    # then Complete (Get_Epona.bfevfl: event 149 then 147).
    set_horse(pair, "B", EPONA)
    pair.step_both(APPLY_FRAMES)
    pair.flag("B", QUEST, "Complete")
    pair.until("A's %s is Complete" % QUEST, lambda: quest(pair, "A") == COMPLETE, SHARE_FRAMES)
    pair.step_both(APPLY_FRAMES)
    assert horse(pair, "A") == EPONA, "A took Complete without the horse (the row 294 bug): %s = %r" % (
        HORSE, horse(pair, "A"))
    reg = pair.expect_log("A", REGISTERED % SHARED_WHY, max_frames=60)
    raised = pair.expect_log("A", RAISED, max_frames=60)
    # the horse first, the end after (the log's order)
    lines = pair.log["A"].since(mark["A"])
    assert lines.index(reg) < lines.index(raised), "A: Complete came before the horse:\n%s\n%s" % (raised, reg)
    # B won her itself: nothing registered for it by co-op
    assert horse(pair, "B") == EPONA, "B lost its own horse: %r" % horse(pair, "B")
    assert pair.log["B"].find(r"coop: story: Epona registered", pos=mark["B"]) is None, "B: co-op registered a horse " \
        "for the game that won her"
    pair.notes.append("A: %s = %s, %s Complete" % (HORSE, EPONA, QUEST))


@pytest.mark.budget(240)
@pytest.mark.row("#294 a save at Get_Epona Complete with no owned horse gets her back (mend)")
def test_epona_complete_without_horse_is_mended(pair):
    with pair.setup():
        need_child_in_play(pair)
        if quest(pair, "A") != COMPLETE:
            # (her first, so the set-up's own Complete is never a bare one the catch-up mends around the mark)
            set_horse(pair, "A", EPONA)
            pair.step_both(APPLY_FRAMES)
            pair.flag("A", QUEST, "Complete")
            pair.until("A's %s is Complete" % QUEST, lambda: quest(pair, "A") == COMPLETE, SHARE_FRAMES)
        mark = pair.log["A"].mark()
    # The player report's save: Complete, and no horse (a shared end before 0.12.0).
    set_horse(pair, "A", "")
    pair.expect_log("A", r'coop: test settext OwnedHorseList\.ActorName\[0\] "": set', max_frames=30)
    pair.expect_log("A", REGISTERED % MENDED_WHY, max_frames=MEND_FRAMES)
    pair.step_both(APPLY_FRAMES)
    assert horse(pair, "A") == EPONA, "A: not mended: %s = %r" % (HORSE, horse(pair, "A"))
    assert quest(pair, "A") == COMPLETE, "A: the mend moved %s" % QUEST
    # A scene load: a live-only write goes back at a load (issue 57); the applier's saved copy stays.
    st = pair.warp_one("A", "Market_Child")
    assert (st.get("scene") or "").lower() == "market_child", "A is in %s after the warp" % st.get("scene")
    pair.step_both(APPLY_FRAMES)
    assert horse(pair, "A") == EPONA, "A: the horse went at the scene load: %r" % horse(pair, "A")
    regs = [ln for ln in pair.log["A"].since(mark) if "coop: story: Epona registered" in ln]
    assert len(regs) == 1, "A: registered %d times (once is the mend): %s" % (len(regs), regs)
    pair.notes.append("A: mended and kept through a scene load")
