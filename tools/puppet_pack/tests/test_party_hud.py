"""Tests for the other players' hearts on the game's HUD (party_hud.py). Synthetic data only: no game files.

    python tools/puppet_pack/tests/test_party_hud.py
"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from puppet_pack import ui_pack, party_hud as ph  # noqa: E402
from test_ui_pack import make_layout  # noqa: E402

# Shaped as the game's PlayerStatus_00: the heart part under the top left corner's pane, whose subtree ends before
# the last pane.
STATUS = [("RootPane", -1), ("N_VRS_00", 0), ("N_All_00", 0), ("N_ScaleL_00", 2), ("Pa_HeartGauge_00", 3),
          ("N_ScaleL_01", 3), ("Pa_MpGauge_00", 5), ("N_ScaleR_00", 2), ("Pa_SelectItem_00", 7)]
STATUS_PARTS = {"Pa_HeartGauge_00": ("PaHeartGauge_00", []), "Pa_MpGauge_00": ("PaMpGauge_00", []),
                "Pa_SelectItem_00": ("PaItemBtn_00", [])}


def status():
    return make_layout(STATUS, STATUS_PARTS)


class PartyHudTest(unittest.TestCase):
    def test_names(self):
        names = ph.names()
        self.assertEqual(len(names), ph.SLOTS * (ph.HEARTS + 2) + 2)
        self.assertEqual(len(set(names)), len(names))
        for s in range(ph.SLOTS):   # (a slot's part heart after its whole hearts: drawn over them)
            self.assertGreater(names.index(ph.gauge(s)), names.index(ph.heart(s, ph.HEARTS - 1)))

    def test_the_copies(self):
        d = status()
        n = ph.add_party(d)
        before, _ = ui_pack.read_layout(d)
        panes, parts = ui_pack.read_layout(n)
        self.assertEqual([p.name for p in panes], [p.name for p in before] + ph.names())
        self.assertEqual([(p.name, p.record) for p in panes[:len(before)]], [(p.name, p.record) for p in before])
        shows = dict(ui_pack.part_layouts(n))
        src = before[4].record
        for p in panes[len(before):]:
            want = ph.WHOLE if "Heart" in p.name else ph.PART if "Gauge" in p.name else ph.LABEL
            self.assertEqual(shows[p.name], want)
            kind = struct.unpack_from("<I", n, p.record + 0x38)[0]
            self.assertEqual(kind & 1, 0, p.name)                                         # stored hidden
            self.assertEqual(struct.unpack_from("<I", n, p.record + 0x28)[0], 3)          # under N_ScaleL_00
            self.assertEqual(n[p.record + 0x1c:p.record + 0x24], d[src + 0x1c:src + 0x24])   # at the source's place
            size = struct.unpack_from("<2f", n, p.record + 0x2c)
            scale = struct.unpack_from("<2f", n, p.record + 0x10)
            self.assertEqual(size, ph.LABEL_SIZE if want == ph.LABEL else ph.HEART_SIZE)
            self.assertEqual(scale, ph.LABEL_SCALE if want == ph.LABEL else ph.HEART_SCALE)
            self.assertEqual(p.id, ui_pack.part_id(p.name))
        self.assertEqual(shows["Pa_HeartGauge_00"], "PaHeartGauge_00")                    # the game's own as it was
        self.assertEqual(n[:8], d[:8])

    def test_twice_is_once(self):
        n = ph.add_party(status())
        self.assertEqual(ph.add_party(n), n)

    def test_refusals(self):
        with self.assertRaises(ui_pack.UiPackError):
            ph.add_party(make_layout())                       # no heart part
        with self.assertRaises(ui_pack.UiPackError):
            ph.add_party(b"not a layout")
        some = ui_pack.clone_parts(status(), [ui_pack.Copy("Pa_HeartGauge_00", ph.MORE)], out_of_order=True)
        with self.assertRaises(ui_pack.UiPackError):
            ph.add_party(some)

    def test_the_archive_must_hold_the_part_layouts(self):
        d = status()
        n = ph.add_party(d)
        read = [(ph.LAYOUT, d), ("PaHeartGauge_00.buil", b"x")]
        own = [(l + ".buil", b"x") for l in (ph.WHOLE, ph.PART, ph.LABEL)]
        ui_pack.check_own_parts(read + own, [(ph.LAYOUT, n)] + read[1:] + own, "Common")
        with self.assertRaises(ui_pack.UiPackError):
            ui_pack.check_own_parts(read + own[:2], [(ph.LAYOUT, n)] + read[1:] + own[:2], "Common")


if __name__ == "__main__":
    unittest.main()
