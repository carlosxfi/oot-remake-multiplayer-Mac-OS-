"""Fake-game reactions for rows 222/286 (ingame/test_boss_shots.py; owner twinrova-shots-222). Hooks: fakegame.py docstring.

Reacts only on a boot with the rows' switch LICHII_COOP_DEBUG_OBJECTS=1 and only in TwinrovaStage / PhantomGanonStage.
What it logs is the module's text: the shooter line (coop_hookshot.cpp SetBossShooter, at the world hello), the sync
note (coop_boss_native.cpp:527), the bullets (coop_objects.cpp:6794 'objects: + <name>') and the counts
(coop_hookshot.cpp LogBossShooter). A warp to a boss stage answers 'timeout' with Link already there and its intro
event running (state event true for INTRO_FRAMES: the bridge's WarpArrived wants no event), so
Pair.warp_both(arrive_in_event=True) is exercised as on a real game. ZL in a boss stage = a
shot: A (the owner) sends it, B (a copy) makes it.
BT_FAKE_BOSS_ONE_SIDED=1: B makes none of the owner's shots (the bug); =bullets: B counts them made but no bullet
appears. Either way the tests must FAIL.
"""
import os

SWITCH = "LICHII_COOP_DEBUG_OBJECTS=1"
INTRO_FRAMES = 300   # the fake's boss intro; the tests step 900 / 1950 frames after the warp (the old a-in waits)
BOSSES = {"TwinrovaStage": ["Enemy_DungeonBoss_Koume", "Enemy_DungeonBoss_Kotake"],
          "PhantomGanonStage": ["Enemy_DungeonBoss_PhantomGanon"]}
BULLETS = {"TwinrovaStage": ["Twinrova_Bullet_FireBullet", "Twinrova_Bullet_IceBullet"],
           "TwinrovaStage fused": ["Twinrova_Bullet_FireField", "Twinrova_Bullet_IceField"],
           "PhantomGanonStage": ["DungeonBossPhantomGanon_Bullet_LightBall"]}


def _on(game):
    return game.env_on(SWITCH)


def _role(game):
    return "ours" if game.tag == "A" else "a copy"


def setup(game):
    game.shots_fused, game.shots_sent, game.shots_made, game.intro_until = False, 0, 0, 0
    if _on(game):
        game.say("coop: bosses: boss shots the owner's (wrapped)")


def command(game, m):
    if not _on(game) or m.get("cmd") != "warp" or m.get("scene") not in BOSSES:
        return None
    game.run(40)
    game.scene, game.shots_fused, game.intro_until = m["scene"], False, game.frame + INTRO_FRAMES
    for i, boss in enumerate(BOSSES[game.scene]):
        game.say("coop: %s: native boss sync on (BSA 0x%x, %s)" % (boss, 0x100 + i, _role(game)))
    return {"ok": False, "error": "timeout", "message": "not in %s after %s s; held (the intro event)" % (
        game.scene, m.get("timeout"))}


def state(game, st):
    if _on(game) and game.scene in BOSSES and game.frame < game.intro_until and st.get("event") is False:
        st["event"] = True


def press(game, buttons):
    if not _on(game) or "ZL" not in buttons or game.scene not in BOSSES:
        return False
    names = BULLETS.get(game.scene + (" fused" if game.shots_fused else ""), [])
    mode = os.environ.get("BT_FAKE_BOSS_ONE_SIDED")
    if game.tag == "A":
        game.shots_sent += 1
    elif mode == "1":
        return True
    else:
        game.shots_made += 1
        if mode == "bullets":
            names = []
    for n in names:
        game.say("coop: objects: + %s 0x%x state 0 at 0.00 0.00 0.00 hash 0x1" % (n, 0x2000 + game.frame))
    game.say("coop: bosses: boss shots: ours sent %d (0 refused by the game); copies: 0 own skipped, 0 own run before "
             "the owner's came, %d owner's made (0 refused), 0 no template, 0 not prepared, 0 dropped, 0 started "
             "again, 0 put back" % (game.shots_sent, game.shots_made))
    return True


def act(game, words):
    if _on(game) and words[:1] == ["kill"] and words[-1:] == ["Enemy_DungeonBoss_Kotake"] and game.scene == "TwinrovaStage":
        def merge():
            game.shots_fused = True
            game.say("coop: Enemy_DungeonBoss_Twinrova: native boss sync on (BSA 0x200, %s)" % _role(game))
        game.later(30, merge)
        return True
    return False
