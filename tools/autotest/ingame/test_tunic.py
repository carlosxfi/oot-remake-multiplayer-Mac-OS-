"""Rows #202 (own colour) and 198 (a puppet's tunic copy drawn in your own Link's texels) as one bridge test: the port of
hs-tunic's own-colour line (tools/autotest/groups/hookshot-parity-wave1.txt:37) with row 198's check from
offline/row-198-gear-run.ps1 run F1 (reports/port-sweep-c131.md G1c). Owner: port-sweep-c131.

Not ported here (said in the report): row-198-gear-run's C0 control (the old module, before the fix: a build, not a
test) and F2 gear-parts (look_boots / look_hylian_frame = gold: coop-options.ini keys with no switch,
lichii/hook/coop_settings.cpp kVariables; a bridge test cannot set them).
"""
import pytest

from test_gear import face_each_other
from test_rows import need_child_in_play

BLUE = "A:LICHII_COOP_COLOR=blue"                  # wave1:37 switches (LICHII_COOP_PUPPET=1 is in every boot: BASE_ENV)
# wave1:37 expects, A's log. The 'wears' line names LICHII_COOP_SELF_TUNIC when set, else green
# (lichii/hook/coop_puppet_config.cpp:55-59): run.ps1 put green's own files in, a live.py boot may not, so \S+.
STARTS = r"coop: your Link wears \S+ at the start; green players get CoopPuppet_<Age>_Green"
OWN_BLUE = r"coop: look: your Link.s tunic is tunic-3070FF now"        # coop_look_tunic.cpp:505
PACK_WARN = r"the puppet pack's tunic files are"                       # wave1:37 never= (coop_look_live.cpp:728/733)
CROSS = r"row 198 cross guard"                                         # coop_look_live.cpp:591 (row-198-gear-run F1)


@pytest.mark.budget(180)
@pytest.mark.look
@pytest.mark.needs_env(BLUE)
@pytest.mark.row("#202 own colour + 198 no cross guard (hs-tunic, row-198-gear F1)")
def test_own_colour_no_cross_guard(pair):
    with pair.setup():
        need_child_in_play(pair)
        face_each_other(pair)          # hs-tunic.A.txt: A steps off and stands by B; B.txt: faces A, camera behind
    # the colour is set at the module's start: these lines are from the boot (pos=0), as the old row read them
    assert pair.log["A"].find(STARTS, pos=0), "A: no /%s/ (wave1:37)" % STARTS
    pair.until("A log /%s/" % OWN_BLUE, lambda: pair.log["A"].find(OWN_BLUE, pos=0), max_frames=300)
    assert pair.log["A"].find(PACK_WARN, pos=0) is None, "A: %s" % pair.log["A"].find(PACK_WARN, pos=0)
    pair.step_both(120)                # hs-tunic.B.txt b-sees-a: 120 + 170 frames in; the puppets are drawn by now
    # wave1:37 see=hs-tunic-A/B: A's own Link blue + B's puppet green on A; A's puppet blue on B. Still colours: one
    # shot each game (docs/ingame-tests.md step 6). Tolerance 0.80: two Links that breathe, Kakariko's people.
    for x in ("A", "B"):
        pair.check_shot(x, "tunic-blue", tolerance=0.80)
    pair.step_both(120)
    for x in ("A", "B"):               # row 198: the guard must never fire, in either game, from the boot on
        line = pair.log[x].find(CROSS, pos=0)
        assert line is None, "%s: %s" % (x, line)
