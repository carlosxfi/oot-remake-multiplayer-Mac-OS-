"""Selftests of row 83's bridge test (tools/autotest/ingame/test_unfocus.py) on the fake games (ingame/fakes/row83.py).
Run by test_ingame.py's load_tests (CI) or alone."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402


class Row83BackWindowFps(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_fails_when_a_game_behind_loses_frames(self):
        out = os.path.join(self.tmp, "o")
        T.pytest_run([".", "--fake", "--out", out, "-q", "-k", "back_window", "--no-rerun"], env={"BT_FAKE_FPS_B": "40"})
        s = T.summary(out)
        self.assertEqual(s["counts"]["FAIL"], 1, s)
        self.assertIn("B (behind: not in front): fps 40.0", s["tests"][0]["why"])

    def test_passes_at_the_cap_and_keeps_the_fps(self):
        out = os.path.join(self.tmp, "o")
        T.pytest_run([".", "--fake", "--out", out, "-q", "-k", "back_window"])
        s = T.summary(out)
        self.assertEqual(s["counts"]["PASS"], 1, s)
        self.assertTrue(any("fps 60.0/60.0/60.0/60.0" in n for n in s["tests"][0]["notes"]), s["tests"][0])


if __name__ == "__main__":
    unittest.main()
