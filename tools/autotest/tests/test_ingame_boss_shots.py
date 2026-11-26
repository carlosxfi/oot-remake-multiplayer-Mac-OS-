"""Selftests of rows 222/286 (ingame/test_boss_shots.py + ingame/fakes/boss_shots.py), no game: fake bridges.
Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

pytest_run, summary, results = T.pytest_run, T.summary, T.results


class BossShots(unittest.TestCase):
    """Rows 222/286 (ingame/test_boss_shots.py): PASS on the fake pair with the rows' switches; FAIL when the copy makes
    none of the owner's shots, or counts them made with no bullet (fakes/boss_shots.py BT_FAKE_BOSS_ONE_SIDED); deselected
    without the switches."""
    ENV = ["--env", "LICHII_COOP_DEBUG_BOSS=1", "--env", "LICHII_COOP_DEBUG_OBJECTS=1", "--env", "LICHII_COOP_CHEATS=1"]

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_bosses(self, tag, env=None, more=()):
        out = os.path.join(self.tmp, tag)
        p = pytest_run(["test_boss_shots.py", "--fake", "--no-rerun", "--out", out, "-q"] + list(more), env=env)
        return p, summary(out)

    def test_pass_with_the_switches(self):
        p, s = self.run_bosses("ok", more=self.ENV)
        self.assertEqual(results(s), {"test_twinrova_shots": "PASS", "test_phantom_ganon_shots": "PASS"}, p.stdout[-2000:])
        kept = os.path.join(self.tmp, "ok", "lane1", "shots-kept")     # the old run's paired shots, both games
        self.assertEqual(len(os.listdir(os.path.join(kept, "twinrova"))), 2 * (10 + 24))
        self.assertEqual(len(os.listdir(os.path.join(kept, "phantomganon"))), 2 * 24)

    def test_fail_when_the_copy_makes_none(self):
        p, s = self.run_bosses("none", env={"BT_FAKE_BOSS_ONE_SIDED": "1"}, more=self.ENV)
        self.assertEqual(set(results(s).values()), {"FAIL"}, p.stdout[-2000:])
        self.assertIn("UNPROVEN: no owner's shot sent and made in a copy", p.stdout)

    def test_fail_when_a_bullet_is_in_one_game_only(self):
        p, s = self.run_bosses("bullets", env={"BT_FAKE_BOSS_ONE_SIDED": "bullets"}, more=self.ENV)
        self.assertEqual(set(results(s).values()), {"FAIL"}, p.stdout[-2000:])
        self.assertIn("made in one game only", p.stdout)

    def test_deselected_without_the_switches(self):
        p, s = self.run_bosses("none-env")
        self.assertEqual(s["tests"], [], p.stdout[-2000:])
        self.assertEqual(sorted(d["test"] for d in s["deselected"]),
                         ["test_boss_shots.py::test_phantom_ganon_shots", "test_boss_shots.py::test_twinrova_shots"])


if __name__ == "__main__":
    unittest.main()
