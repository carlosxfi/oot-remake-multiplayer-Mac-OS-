"""Fake-game reactions for row 294 (ingame/test_epona_294.py). Hooks: fakegame.py docstring.

A `flag` of Step_Get_Epona = Complete in one game reaches the other 12 frames later as the fixed module does it
(coop_hook.cpp EnsureEpona, coop_quests.cpp Store::put): the horse first (OwnedHorseList.ActorName[0] =
Animal_Epona_Adult, its log line), then Complete (the quests' raise line). A game at Complete whose element 0 is
emptied gets her back 60 frames later (the quest catch-up's mend, once a second). `flag_read` answers the quest as
kind "enum" (its value's hash) and NAME[i] as kind "text" (docs/playtest.md flag_read).
BT_FAKE_EPONA=nohorse: the other game takes Complete with no horse (the bug as reported); =nomend: no mend.
"""
import os

QUEST = "Step_Get_Epona"
HORSE = "OwnedHorseList.ActorName[0]"
EPONA = "Animal_Epona_Adult"
COMPLETE = "0x4c0a63f4"
REG = "coop: story: Epona registered as this game's own horse (OwnedHorseList.ActorName[0] = Animal_Epona_Adult): %s " \
      "(row 294)"
SHARED = "another player won her: Get_Epona's end comes with her"
MENDED = "Get_Epona was Complete here with no owned horse (mended)"


def setup(game):
    game.flags.setdefault(QUEST, "Ready")
    game.flags.setdefault(HORSE, "")


def _mend_later(game):
    if os.environ.get("BT_FAKE_EPONA") == "nomend":
        return

    def mend():
        if game.flags.get(QUEST) == "Complete" and game.flags.get(HORSE) == "":
            game.flags[HORSE] = EPONA
            game.say(REG % MENDED)
    game.later(60, mend)


def command(game, m):
    if m.get("cmd") != "flag" or m.get("name") not in (QUEST, HORSE):
        return None
    name, value = m["name"], m["value"]
    if name == HORSE:
        game.say('coop: test settext %s "%s": set' % (HORSE, value))
        if value == "" and game.flags.get(QUEST) == "Complete":
            _mend_later(game)
        return None
    o = game.other
    if value == "Complete" and o is not None:
        def share():
            if o.flags.get(QUEST) == "Complete":
                return
            if os.environ.get("BT_FAKE_EPONA") != "nohorse" and o.flags.get(HORSE) == "":
                o.flags[HORSE] = EPONA
                o.say(REG % SHARED)
            was = o.flags.get(QUEST)
            o.flags[QUEST] = "Complete"
            o.say("coop: quests: Get_Epona %s -> Complete (shared Threads of Time; issue 193)" % was)
        o.later(12, share)
        if game.flags.get(HORSE) == "":
            game.flags[QUEST] = "Complete"   # (set before the mend looks; the built-in answer sets it again)
            _mend_later(game)
    return None   # (the built-in answer sets this game's own flag and replies `was`)


def after(game, m, r):
    if m.get("cmd") != "flag_read":
        return
    if m.get("name") == QUEST:
        r["kind"] = "enum"
        r["value"] = COMPLETE if game.flags.get(QUEST) == "Complete" else "0x11111111"
    elif m.get("name") == HORSE:
        r["kind"] = "text"
        r["value"] = game.flags.get(HORSE, "")
