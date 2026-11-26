"""Row 53's selftest (ingame/test_row53.py + ingame/fakes/row53.py; owner row-53-ocarina-5), run by test_ingame.py's
load_tests."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402


class Row53(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_row53_fails_when_a_place_is_put_back(self):
        """PASS on the fake; FAIL (never PASS) when the game puts Link back after a place (the 11-23 fault:
        'place ... done' logged, Link still at the gate)."""
        out = os.path.join(self.tmp, "ok")
        T.pytest_run([".", "--fake", "--out", out, "-q", "-p", "no:cacheprovider", "-k", "row53"])
        s = T.summary(out)
        self.assertEqual(T.results(s), {"test_temple_of_time_places": "PASS"})
        notes = s["tests"][0]["notes"]
        self.assertEqual(sum("LANDED" in n for n in notes), 3, notes)
        self.assertTrue(any(n.startswith("p-at-door x ") for n in notes), notes)
        out = os.path.join(self.tmp, "bad")
        p = T.pytest_run([".", "--fake", "--out", out, "-q", "-p", "no:cacheprovider", "-k", "row53"],
                         env={"BT_FAKE_PLACE_PUT_BACK": "1"})
        s = T.summary(out)
        self.assertEqual(T.results(s), {"test_temple_of_time_places": "FAIL"}, p.stdout[-1500:])
        self.assertIn("put back", s["tests"][-1]["why"])
        self.assertNotEqual(p.returncode, 0)


if __name__ == "__main__":
    unittest.main()
