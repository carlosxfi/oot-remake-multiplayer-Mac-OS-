"""Selftest of ingame/test_tunic.py + ingame/fakes/tunic.py (rows #202 + 198; owner port-sweep-c131), run by
test_ingame.py's load_tests: deselected without A's colour switch, NO-REF with it (a look shot, no references), FAIL
when the row 198 cross guard fires."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

ARGS = ["-q", "-p", "no:cacheprovider", "-k", "test_own_colour_no_cross_guard"]
NAME = "test_own_colour_no_cross_guard"


class Tunic(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, env_on=True, bug=None):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs")}
        if bug:
            env["BT_FAKE_TUNIC_BUG"] = bug
        args = [".", "--fake", "--out", out] + (["--env", "A:LICHII_COOP_COLOR=blue"] if env_on else []) + ARGS
        p = T.pytest_run(args, env=env)
        return p, T.summary(out)

    def test_without_the_switch_it_is_deselected(self):
        p, s = self.run_fake("off", env_on=False)
        self.assertNotIn(NAME, T.results(s), p.stdout[-1500:])
        self.assertTrue(any(d["test"].endswith(NAME) for d in s.get("deselected", [])), s.get("deselected"))

    def test_with_the_switch_no_ref(self):
        p, s = self.run_fake("on")
        self.assertEqual(T.results(s), {NAME: "NO-REF"}, p.stdout[-1500:])

    def test_cross_guard_fails(self):
        p, s = self.run_fake("cross", bug="cross")
        self.assertEqual(T.results(s), {NAME: "FAIL"}, p.stdout[-1500:])
        self.assertIn("row 198 cross guard", s["tests"][0]["why"])


if __name__ == "__main__":
    unittest.main()
