"""G2b map icons (test_map_icons.py): what a real game with map-icons-255 gives the test, on the fake games.

- move_step: keeps the game's facing (degrees, the state's `facing`) the way the `move` walks (0 = +z, 90 = +x).
- state: each player row says `self` as the real bridge does (coop_playtest.cpp: "self"), and while the scene is
  InsideTheDekuTree with its map flag set, `map_native` (coop_map_icons.h State, docs/playtest.md) as the module
  gives it: the copies up, one shown for the other game with its name set and its turn written = read back at
  kPaneTurn 0x68 (plan: -facing - a quarter, radians), and the game's own arrow's words with our turn at index 2.
"""
import math

SCENE = "InsideTheDekuTree"
MAP_FLAG = "DungeonInfo.InsideTheDekuTree.IsGetMap"   # (test_map_icons.MAP_FLAG)
TURN_AT = 0x68                                          # (coop_map_icons.h kPaneTurn)


def move_step(game, pos):
    dx, dz = pos[0] - game.pos[0], pos[2] - game.pos[2]
    if dx or dz:
        game.facing = math.degrees(math.atan2(dx, dz)) % 360
    return None


def _flag(game):
    flags = getattr(game, "flags", None) or {}
    return bool(flags.get(MAP_FLAG))


def _turn(facing_deg):
    t = -math.radians(facing_deg) - math.pi / 2
    return math.atan2(math.sin(t), math.cos(t))


def state(game, st):
    me = 1 if game.tag == "A" else 2
    for p in (st.get("coop") or {}).get("players", []):
        p["self"] = p.get("number") == me
    st["map_native"] = None
    if st.get("scene") != SCENE or not _flag(game):
        return
    other = game.other
    t = _turn(other.facing or 0.0)
    words = [1.0, 1.0, _turn(game.facing or 0.0), game.pos[0], game.pos[2], 1.0, 1.0, 0.0]
    st["map_native"] = {"up": True, "why": "", "table": True, "turn_at": TURN_AT, "game_arrow": True,
                        "game_arrow_words": words,
                        "shown": [{"copy": 0, "x": other.pos[0], "y": other.pos[2], "turn": t, "wrote_turn": t,
                                   "name": other.tag, "name_set": True}]}
