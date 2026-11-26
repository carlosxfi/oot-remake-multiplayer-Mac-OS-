"""Fake-game reactions for row 53's Temple of Time probe (ingame/test_row53.py; owner row-53-ocarina-5).
Only in Market_Child: a place over the temple floor drops onto it, the shut Door of Time is a wall for `move`, and
BT_FAKE_PLACE_PUT_BACK=1 (selftest) is the 11-23 fault: the place is "done" but Link is not moved.
"""
import os

SCENE = "Market_Child"
TEMPLE_FLOOR = 103.9      # fake: the floor a place drops onto (test_row53's spots are 1.6 m over it)
DOOR_WALL_X = 117.8       # fake: Link stops here at the shut door (test_row53: the door's x 118.38)


def act(game, words):
    if game.scene != SCENE or not words or words[0] != "place":
        return False
    if os.environ.get("BT_FAKE_PLACE_PUT_BACK") == "1":
        return True                       # "done", but the game put Link back (Link stays where he was)
    game.pos = [float(v) for v in words[1:4]]
    if game.pos[1] > TEMPLE_FLOOR:
        game.later(20, lambda: game.pos.__setitem__(1, TEMPLE_FLOOR))
    return True


def move_step(game, pos):
    if game.scene == SCENE and pos[0] > DOOR_WALL_X:
        return [DOOR_WALL_X, pos[1], pos[2]]
    return None
