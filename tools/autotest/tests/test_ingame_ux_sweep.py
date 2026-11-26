"""Selftests of ux-sweep-0120's bridge tests (tools/autotest/ingame/test_ux_sweep.py) on the fake games
(ingame/fakes/ux_sweep.py): each PASSes on the fixed fake and FAILs under the bug it guards (BT_FAKE_UX).
Run by test_ingame.py's load_tests (CI) or alone."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ingame as T   # noqa: E402

BOOT_ADULT = ["--boot", "live-twinrova-room"]


class UxSweep(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_one(self, k, mode="", extra=()):
        out = os.path.join(self.tmp, "o")
        T.pytest_run([".", "--fake", "--out", out, "-q", "-k", k, "--no-rerun"] + list(extra), env={"BT_FAKE_UX": mode})
        return T.summary(out)

    def check(self, s, result, why=None):
        self.assertEqual(s["counts"][result], 1, s)
        if why:
            self.assertIn(why, s["tests"][0]["why"])

    def test_gift_passes_when_shared_once(self):
        self.check(self.run_one("great_fairy_gift"), "PASS")

    def test_gift_fails_when_not_shared(self):
        self.check(self.run_one("great_fairy_gift", "gift-unshared"), "FAIL", "the gift is the group's")

    def test_gift_fails_when_a_zero_goes_back(self):
        self.check(self.run_one("great_fairy_gift", "gift-back"), "FAIL", "went back to 0")

    def test_scrub_passes(self):
        self.check(self.run_one("business_scrub"), "PASS")

    def test_scrub_fails_when_shared(self):
        self.check(self.run_one("business_scrub", "scrub-shared"), "FAIL", "shared copies")

    def test_scrub_fails_on_the_second_talk_softlock(self):
        self.check(self.run_one("business_scrub", "scrub-softlock"), "FAIL", "the second talk never ended")

    def test_lock_passes(self):
        self.check(self.run_one("small_key_door", extra=BOOT_ADULT), "PASS")

    def test_lock_fails_when_it_stays_shut_for_b(self):
        self.check(self.run_one("small_key_door", "lock-stuck", BOOT_ADULT), "FAIL", "stays locked for B")

    def test_lock_fails_when_the_lent_key_reaches_the_group(self):
        self.check(self.run_one("small_key_door", "lock-double", BOOT_ADULT), "FAIL", "one key per door")

    def test_final_blow_passes_at_the_cap(self):
        s = self.run_one("final_blow", extra=BOOT_ADULT)
        self.check(s, "PASS")
        self.assertTrue(any("B: fps 60.0/60.0/60.0" in n for n in s["tests"][0]["notes"]), s["tests"][0])

    def test_final_blow_fails_when_the_guest_drops_frames(self):
        out = os.path.join(self.tmp, "o")
        T.pytest_run([".", "--fake", "--out", out, "-q", "-k", "final_blow", "--no-rerun"] + BOOT_ADULT,
                     env={"BT_FAKE_UX": "", "BT_FAKE_FPS_B": "40"})
        self.check(T.summary(out), "FAIL", "frame drops after the final blow (row 283): B: fps 40.0")

    def test_final_blow_no_ref_when_the_kill_does_not_land(self):
        self.check(self.run_one("final_blow", "kill-miss", BOOT_ADULT), "NO-REF", "no final blow measured")


if __name__ == "__main__":
    unittest.main()
