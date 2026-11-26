"""Selftests of Pair.rejoin (regress step 4, tools/autotest/ingame/test_rejoin.py): no game, a fake live.py + bridge."""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
sys.path.insert(0, INGAME)
import pair as P          # noqa: E402


class FakeClient:
    made = []

    def __init__(self, port, timeout=30.0):
        self.port, self.calls, self.closed = port, [], False
        FakeClient.made.append(self)

    def request(self, cmd, **args):
        self.calls.append(cmd)
        return {"ok": True, "frame": 1}

    def hello(self):
        self.calls.append("hello")
        return {"ok": True, "control": True}

    def resume(self):
        self.calls.append("resume")

    def pause(self):
        self.calls.append("pause")

    def close(self):
        self.closed = True


class Rejoin(unittest.TestCase):
    def setUp(self):
        FakeClient.made = []
        self.tmp = tempfile.mkdtemp()
        self.logs = {x: os.path.join(self.tmp, "game-out-%s.txt" % x) for x in "AB"}
        for p in self.logs.values():
            with open(p, "w") as f:
                f.write("old line\n")
        games = {x: {"port": 40000 + i, "folder": self.tmp, "log": self.logs[x]} for i, x in enumerate("AB")}
        self.pair = P.Pair(games, client=FakeClient)
        self.pair.session = self.tmp
        self.asked = []

    def live(self, answers):
        def run(args, timeout):
            self.asked.append((args, timeout))
            if args[0] == "relaunch":       # relaunch: the old log moved aside, a new one from line 1
                os.replace(self.logs["B"], self.logs["B"] + ".1")
                with open(self.logs["B"], "w") as f:
                    f.write("coop: save: our own 5 rupees/ammo/bottles/life/magic values kept in it\n")
            return answers[args[0]]
        return run

    def test_quit_relaunch_new_client_and_log_from_its_start(self):
        self.pair.live = self.live({"quit": {"player": "B", "how": "quit asked"}, "relaunch": {"ok": True, "boot": "played"}})
        old_b, a = self.pair.c["B"], self.pair.c["A"]
        self.pair.begin(600)
        r = self.pair.rejoin("B", boot="boot.B.txt")
        self.assertTrue(r["ok"])
        self.assertEqual([x[0] for x in self.asked], [["quit", "--session", self.tmp, "B"],
                                                     ["relaunch", "--session", self.tmp, "B", "--boot", "boot.B.txt"]])
        self.assertTrue(old_b.closed)
        self.assertIsNot(self.pair.c["B"], old_b)
        self.assertEqual(self.pair.c["B"].port, old_b.port)
        self.assertEqual(a.calls, ["resume", "pause"])          # A runs in real time while B is away, held after
        self.assertTrue(self.pair.log["B"].find(r"values kept in it"))   # the new log is read from its first line
        self.assertLessEqual(self.asked[0][1], P.Pair.REJOIN_QUIT_S)
        self.assertLessEqual(self.asked[1][1], P.Pair.REJOIN_BOOT_S)

    def test_a_failed_relaunch_fails_and_still_holds_the_other_game(self):
        self.pair.live = self.live({"quit": {"player": "B", "how": "quit asked"},
                                    "relaunch": {"ok": False, "boot": "FAILED (see steps.txt): the game stays down"}})
        a = self.pair.c["A"]
        self.pair.begin(600)
        with self.assertRaises(AssertionError) as e:
            self.pair.rejoin("B")
        self.assertIn("relaunch", str(e.exception))
        self.assertEqual(a.calls, ["resume", "pause"])

    def test_a_quit_error_fails_before_any_relaunch(self):
        self.pair.live = self.live({"quit": {"error": "game B is not in this session"}, "relaunch": {"ok": True}})
        self.pair.begin(600)
        with self.assertRaises(AssertionError):
            self.pair.rejoin("B")
        self.assertEqual([x[0][0] for x in self.asked], ["quit"])

    def test_no_session_is_setup_fail(self):
        self.pair.session = None
        with self.assertRaises(P.SetupFail):
            self.pair.rejoin("B")


class RejoinTestCollects(unittest.TestCase):
    def test_deselected_without_the_guest_switch(self):
        import subprocess
        out = tempfile.mkdtemp()
        p = subprocess.run([sys.executable, "-m", "pytest", "-p", "no:cacheprovider", "-q", "--fake", "--out", out,
                            "-k", "test_own_values_kept_over_rejoin", "."], cwd=INGAME, capture_output=True, text=True,
                           timeout=300)
        self.assertIn("needs_env B:LICHII_COOP_TEST_OWN_SAVE=0", p.stdout + p.stderr)


if __name__ == "__main__":
    unittest.main()
