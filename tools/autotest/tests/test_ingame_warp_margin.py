"""Pair.warp_one sizes the bridge's warp wait under the deadline with a margin (row-215-confirm, CI 45885309573
windows 3.10: test_twinrova_wake_late SETUP-FAIL 'warp not sent (30.0 s left, its reply needs 30.0 s)'). No game.
Run by test_ingame.py's load_tests (CI: python tools/autotest/tests/test_ingame.py)."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "ingame"))
import pair as P   # noqa: E402


class Sent(Exception):
    pass


class Ticking:
    """A coarse monotonic clock (Windows' ~15.6 ms tick), as in the CI failure: the deadline's own start, warp_one's
    sizing read and its check() fall in one tick (left reads exactly the set-up's 30.0), then the clock ticks before
    _wait's read and on every read after it."""
    SAME = 3   # reads in the first tick: Deadline(), warp_one's left(), deadline.check()

    def __init__(self, tick=0.0156):
        self.now, self.tick, self.reads = 1000.0, tick, 0

    def __call__(self):
        self.reads += 1
        if self.reads > self.SAME:
            self.now += self.tick
        return self.now


class Client:
    last_frame = 0


class WarpMargin(unittest.TestCase):
    def pair(self, seconds):
        p = P.Pair.__new__(P.Pair)
        p.deadline = P.Deadline(seconds, clock=Ticking())   # (left reads exactly `seconds` first, then less)
        p.c = {"A": Client(), "B": Client()}
        p.notes = []
        got = {}

        def request(x, cmd, wait, check=True, **kw):
            got.update(x=x, cmd=cmd, wait=wait, **kw)
            raise Sent()
        p._request = request
        return p, got

    def test_a_fresh_set_up_deadline_sends_the_warp(self):
        for seconds in (30.0, 29.0, 12.0, 120.0):
            p, got = self.pair(seconds)
            with self.assertRaises(Sent, msg="%s s: the warp was not sent" % seconds):
                p.warp_one("A", "TwinrovaStage", entrance="TwinrovaStage", arrive_in_event=False)
            self.assertEqual(got["cmd"], "warp")
            self.assertLessEqual(got["timeout"] + P.REPLY_MIN_S, seconds - p.WARP_MARGIN_S)

    def test_the_arrival_wait_keeps_its_own_bound(self):
        p, got = self.pair(120.0)
        with self.assertRaises(Sent):
            p.warp_one("A", "TwinrovaStage", entrance="TwinrovaStage", arrive_in_event=True)
        self.assertEqual(got["timeout"], p.ARRIVE_IN_EVENT_WAIT)

    def test_too_little_left_is_a_timeout_not_a_short_warp(self):
        p, got = self.pair(2.5)
        with self.assertRaises(P.TestTimeout):
            p.warp_one("A", "TwinrovaStage", arrive_in_event=False)
        self.assertEqual(got, {})


if __name__ == "__main__":
    unittest.main()
