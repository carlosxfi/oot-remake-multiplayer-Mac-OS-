"""Selftests of test_look.py::test_kokiri_hood_band (kokiri-band-c124's port) on the fake games
(ingame/fakes/kokiri_band.py); run by tests/test_ingame.py's load_tests."""
import os
import re
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_ingame import pytest_run, results, summary   # noqa: E402


class KokiriBand(unittest.TestCase):
    """Deselected without its switches; with them it runs to its shots and puts both games back to child, so the
    lane's next test still sets up. Its band check holds on the fake shots but ends NO-REF (no stored reference, and
    the thresholds are not measured: never PASS); the pack before the fix and overlapping boxes are SETUP-FAIL; the
    row's bug (the puppet's band blue) is FAIL."""
    ENV = ["--env", "LICHII_COOP_COLOR=blue", "--env", "LICHII_COOP_LOOK=cap_trim:yellow",
           "--env", "LICHII_COOP_DEBUG_ANIM=1", "--env", "LICHII_COOP_CHEATS=1"]
    TESTS = ["test_look.py::test_kokiri_hood_band", "test_rows.py::test_puppets_spawn"]

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_it(self, name, env_args, env=None):
        out = os.path.join(self.tmp, name)
        p = pytest_run(self.TESTS + ["--fake", "--no-rerun", "--out", out, "-q", "-s", "-p", "no:cacheprovider"]
                       + env_args, env=env)
        return summary(out), p

    def test_runs_with_its_switches_and_leaves_child(self):
        s, p = self.run_it("on", self.ENV)
        self.assertEqual(results(s), {"test_kokiri_hood_band": "NO-REF", "test_puppets_spawn": "PASS"},
                         p.stdout[-2000:])
        self.assertIn("the band thresholds are not measured yet", p.stdout)
        self.assertEqual(len(re.findall(r"(?m)^bandcheck \d", p.stdout)), 7, p.stdout[-2000:])   # (one line per distance)
        s, p = self.run_it("off", [])
        self.assertEqual([d["test"] for d in s["deselected"]], ["test_look.py::test_kokiri_hood_band"], p.stdout[-2000:])

    def test_old_cap_bin_is_setup_fail(self):
        s, p = self.run_it("old", self.ENV, env={"BT_FAKE_CAP_BLOCKS": "43228"})
        self.assertEqual(results(s)["test_kokiri_hood_band"], "SETUP-FAIL", p.stdout[-2000:])
        self.assertIn("not the fix's cap.bin", p.stdout)

    def test_probe_on_the_old_pack_is_no_ref(self):
        """KOKIRI_BAND_PROBE_PACK=43228: the old pack is taken, every bandcheck line printed, never PASS or FAIL."""
        s, p = self.run_it("probe", self.ENV, env={"BT_FAKE_CAP_BLOCKS": "43228", "BT_FAKE_BAND": "blue",
                                                   "KOKIRI_BAND_PROBE_PACK": "43228"})
        self.assertEqual(results(s)["test_kokiri_hood_band"], "NO-REF", p.stdout[-2000:])
        self.assertIn("probe run on the 43228 pack", p.stdout)
        self.assertEqual(len(re.findall(r"(?m)^bandcheck \d", p.stdout)), 7, p.stdout[-2000:])

    def test_blue_band_on_the_puppet_fails(self):
        """The row's bug (the puppet's hood band in the tunic's blue at a distance) is a FAIL, not a pass or NO-REF."""
        s, p = self.run_it("blue", self.ENV, env={"BT_FAKE_BAND": "blue"})
        self.assertEqual(results(s)["test_kokiri_hood_band"], "FAIL", p.stdout[-2000:])
        self.assertIn("the puppet's hood band is not the trim colour", p.stdout)
        self.assertEqual(results(s)["test_puppets_spawn"], "PASS", p.stdout[-2000:])   # (back to child after a FAIL)

    def test_puppet_on_the_line_of_sight_is_setup_fail(self):
        """A's puppet drawn over B's own Link (or a background box over the other Link): SETUP-FAIL, never judged."""
        s, p = self.run_it("line", self.ENV, env={"BT_FAKE_KOKIRI_ON_LINE": "1"})
        self.assertEqual(results(s)["test_kokiri_hood_band"], "SETUP-FAIL", p.stdout[-2000:])
        self.assertIn("the band check's boxes overlap", p.stdout)
        self.assertEqual(results(s)["test_puppets_spawn"], "PASS", p.stdout[-2000:])


if __name__ == "__main__":
    unittest.main()
