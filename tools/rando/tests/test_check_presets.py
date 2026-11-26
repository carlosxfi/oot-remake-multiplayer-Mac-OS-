"""Offline tests of tools/rando/check_presets.py (the decoding itself runs in CI's own step, with the pinned OOTRR)."""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import check_presets as cp  # noqa: E402


def preset(**kw):
    s = {"door_of_time_escape": "moat", "gerudo_fortress": "open", "door_of_time_condition": "open"}
    s.update(kw)
    return {"settings": s, "warnings": []}


class CheckPresets(unittest.TestCase):
    def test_header_strings_are_found(self):
        found = cp.own_presets(cp.HEADER.read_text(encoding="utf-8"))
        self.assertEqual(sorted(found), ["open", "open_fast"])
        for s in found.values():
            self.assertTrue(s.startswith("OOTRR1-"))

    def test_no_strings_is_a_failure(self):
        with self.assertRaises(cp.Fail):
            cp.own_presets("nothing here")

    def test_pin_is_read(self):
        pin = cp.read_pin(cp.PIN.read_text(encoding="utf-8"))
        self.assertTrue(pin["release"].startswith("v"))
        self.assertEqual(len(pin["linux_sha256"]), 64)
        with self.assertRaises(cp.Fail):
            cp.read_pin("release=v1\n")

    def test_good_pair(self):
        self.assertEqual(cp.check({"open": preset(), "open_fast": preset(gerudo_fortress="fast")}), [])

    def test_no_moat(self):
        bad = cp.check({"open": preset(door_of_time_escape="vanilla")})
        self.assertEqual(len(bad), 1)
        self.assertIn("moat", bad[0])

    def test_closed_door_in_fast(self):
        # the 0.11.12 open_fast: Door of Time "vanilla" beside open's "open"
        bad = cp.check({"open": preset(), "open_fast": preset(gerudo_fortress="fast", door_of_time_condition="vanilla")})
        self.assertEqual(len(bad), 1)
        self.assertIn("door_of_time_condition", bad[0])

    def test_warnings_fail(self):
        p = preset()
        p["warnings"] = ["unknown bits"]
        self.assertIn("warns", cp.check({"open": p})[0])


if __name__ == "__main__":
    unittest.main()
