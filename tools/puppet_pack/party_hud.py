"""The other players' hearts on the game's own HUD (PlayerStatus_00), as the game's own heart parts.

The game's HUD holds one heart part, Pa_HeartGauge_00 (PaHeartGauge_00), and its code makes the 20 hearts inside it
at run time: 20 whole hearts (PaHeartGaugeChildNoGauge_00) and one part heart (PaHeartGaugeChildGauge_00) that it moves
onto the heart that is partly filled. The game's gauge object sets those from Link's own life every time it changes, so
they cannot show another player. This step adds our own copies of the same two part layouts to PlayerStatus_00, for
SLOTS other players: HEARTS whole hearts and one part heart each, and one line of the game's text (PaEnemyLocation_00)
for the player's name; and one more line for "+N" when more players are there than slots. Every copy is stored hidden
and at the source's place: the co-op module (hookshot/src/coop_party_hud.h) places, shows and sets them with the
game's own calls, as the game sets its own. A game without the module looks as it always did.

Nothing of the game is in this file: the copies are made from the player's own layout at pack time.

    add_party(layout) -> the layout with the copies (raises ui_pack.UiPackError when it cannot be made)
"""
from . import ui_pack

LAYOUT = "PlayerStatus_00.buil"
SOURCE = "Pa_HeartGauge_00"                  # the part copied: its place and parent (N_ScaleL_00, the top left corner)
WHOLE = "PaHeartGaugeChildNoGauge_00"        # a whole or empty heart
PART = "PaHeartGaugeChildGauge_00"           # the part heart (quarters)
LABEL = "PaEnemyLocation_00"                 # a line of the game's text (T_Text_00, T_Text_01)
SLOTS = 3
HEARTS = 20
HEART_SIZE = (144.0, 144.0)                  # the two heart layouts' own size (their N_Front_00)
LABEL_SIZE = (1000.0, 76.0)                  # PaEnemyLocation_00's text panes
HEART_SCALE = (0.75, 0.75)                   # smaller than Link's own
LABEL_SCALE = (0.75, 0.75)


def heart(slot, i):
    return "Pa_CoopHeart_%d_%02d" % (slot, i)


def gauge(slot):
    return "Pa_CoopGauge_%d" % slot


def label(slot):
    return "Pa_CoopName_%d" % slot


MORE = "Pa_CoopMore_0"


def names():
    """Every copy's name, in the order they are added."""
    out = []
    for s in range(SLOTS):
        out.append(label(s))
        out.extend(heart(s, i) for i in range(HEARTS))
        out.append(gauge(s))    # (after its slot's whole hearts: drawn over them)
    out.append(MORE)
    return out


def _copy(name):
    if name.startswith("Pa_CoopHeart_"):
        return ui_pack.Copy(SOURCE, name, hidden=True, part_layout=WHOLE, size=HEART_SIZE, scale=HEART_SCALE)
    if name.startswith("Pa_CoopGauge_"):
        return ui_pack.Copy(SOURCE, name, hidden=True, part_layout=PART, size=HEART_SIZE, scale=HEART_SCALE)
    return ui_pack.Copy(SOURCE, name, hidden=True, part_layout=LABEL, size=LABEL_SIZE, scale=LABEL_SCALE)


def add_party(layout):
    """PlayerStatus_00 with the other players' hearts and name lines (see the module's notes). A layout that has them
    already is returned as it is."""
    panes, _ = ui_pack.read_layout(layout)
    have = {p.name for p in panes}
    if SOURCE not in have:
        raise ui_pack.UiPackError("%s has no %s" % (LAYOUT, SOURCE))
    want = names()
    if all(n in have for n in want):
        return bytes(layout)
    if any(n in have for n in want):
        raise ui_pack.UiPackError("%s has some of the co-op hearts but not all" % LAYOUT)
    # (the source's parent ends before the last pane: the copies are its children by their parent index, as the
    # - menu's tabs)
    return ui_pack.clone_parts(layout, [_copy(n) for n in want], out_of_order=True)
