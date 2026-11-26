"""Fake-game reactions for ingame/test_tunic.py (rows #202 + 198; owner port-sweep-c131). Only with A's colour switch.

- the boot's colour lines (A): lichii/hook/coop_puppet_config.cpp:55-59 ('your Link wears ... at the start; green
  players get CoopPuppet_<Age>_Green') and hookshot/src/coop_look_tunic.cpp:505 ('your Link's tunic is tunic-3070FF
  now (N ...').
Selftest switch (tests/test_ingame_tunic.py): BT_FAKE_TUNIC_BUG=cross -> B logs the row 198 cross guard
(hookshot/src/coop_look_live.cpp:591) 200 frames in.
"""
import os


def setup(game):
    if game.tag != "A" or not game.env_on("LICHII_COOP_COLOR=blue"):
        return
    game.say("coop: your Link wears green at the start; green players get CoopPuppet_<Age>_Green")
    game.later(30, lambda: game.say("coop: look: your Link's tunic is tunic-3070FF now (4 drawn)"))


def lane(fl):
    if os.environ.get("BT_FAKE_TUNIC_BUG") == "cross" and fl.g["A"].env_on("LICHII_COOP_COLOR=blue"):
        b = fl.g["B"]
        b.later(200, lambda: b.say("coop: look: row 198 cross guard: your colours and a puppet's lie on one texture: "
                                   "tunic #0 on puppets' tunics #21 @0x1234"))
