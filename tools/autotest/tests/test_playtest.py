"""Tests for the live play-test kit's pieces that need no game: the desync checker, the MCP server's protocol, the
script parser's `dump` line, and live.py's lane set-up, watchdog (lane lock, maximum and idle end, clean-up), save
restore default and checkpoint screenshots. Run: python -I tools/autotest/tests/test_playtest.py"""
import argparse
import io
import json
import os
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import desync_check  # noqa: E402
import hookshot  # noqa: E402
import lanes  # noqa: E402
import live  # noqa: E402
import mcp_playtest  # noqa: E402


def write_checkpoint(d, tag, a_flags, b_flags, when):
    os.makedirs(os.path.join(d, "checkpoints"), exist_ok=True)
    path = os.path.join(d, "checkpoints", tag + ".json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"tag": tag, "A": {"status": {"map": "ForestTemple"}, "flags": a_flags},
                   "B": {"status": {"map": "ForestTemple"}, "flags": b_flags}}, f)
    os.utime(path, (when, when))


class DesyncCheck(unittest.TestCase):
    def test_agree_then_first_desync_named(self):
        with tempfile.TemporaryDirectory() as d:
            now = time.time()
            write_checkpoint(d, "t1", {"F1": 0, "F2": 1}, {"F1": 0, "F2": 1}, now - 30)
            write_checkpoint(d, "t2", {"F1": 1, "F2": 1}, {"F1": 0, "F2": 1}, now - 20)
            write_checkpoint(d, "t3", {"F1": 1, "F2": 0}, {"F1": 0, "F2": 1}, now - 10)
            lines = []
            self.assertEqual(desync_check.check(d, out=lines.append), 1)
            self.assertEqual(lines[0], "t1: agree (A ForestTemple, B ForestTemple)")
            self.assertIn("t2: 1 flag(s) differ", lines[1])
            self.assertIn("<- first desync", lines[1])
            self.assertEqual(lines[2], "    F1: A 1, B 0")
            self.assertEqual(len(lines), 3)  # stops at the first desync
            lines = []
            self.assertEqual(desync_check.check(d, show_all=True, out=lines.append), 1)
            self.assertTrue(any(ln.startswith("t3: 2 flag(s) differ") for ln in lines))

    def test_a_flag_one_game_cannot_read_is_no_desync(self):
        diff, note = desync_check.compare({"A": {"flags": {"F": 1, "G": 0}}, "B": {"flags": {"G": 0}}})
        self.assertEqual(diff, {})
        self.assertIsNone(note)
        diff, note = desync_check.compare({"A": {"flags": None, "error": "no dump"}, "B": {"flags": {}}})
        self.assertIn("no dump", note)

    def test_nothing_to_compare(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(desync_check.check(d, out=lambda s: None), 2)

    def test_no_agree_without_a_comparison(self):
        # A checkpoint where a game wrote no flags is not compared: alone it is "nothing compared" (2), never "agree".
        with tempfile.TemporaryDirectory() as d:
            now = time.time()
            write_checkpoint(d, "t1", None, {"F1": 0}, now - 20)
            lines = []
            self.assertEqual(desync_check.check(d, out=lines.append), 2)
            self.assertIn("t1: not compared", lines[0])
            self.assertIn("nothing compared", lines[-1])
            # One agrees, another was not compared: incomplete (3), not agree.
            write_checkpoint(d, "t2", {"F1": 0}, {"F1": 0}, now - 10)
            lines = []
            self.assertEqual(desync_check.check(d, out=lines.append), 3)
            self.assertIn("incomplete: 1 checkpoint(s) not compared", lines[-1])

    def test_shots_named_with_the_checkpoint(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "checkpoints"))
            with open(os.path.join(d, "checkpoints", "t1.json"), "w", encoding="utf-8") as f:
                json.dump({"tag": "t1", "A": {"status": {"map": "M"}, "flags": {"F": 1}, "shot": "t1-A.png"},
                           "B": {"status": {"map": "M"}, "flags": {"F": 1}, "shot": "t1-B.png"}}, f)
            lines = []
            self.assertEqual(desync_check.check(d, out=lines.append), 0)
            self.assertEqual(lines, ["t1: agree (A M, B M; shots t1-A.png, t1-B.png)"])


class Mcp(unittest.TestCase):
    def run_server(self, requests):
        stdin, stdout = sys.stdin, sys.stdout
        sys.stdin = io.StringIO("".join(json.dumps(r) + "\n" for r in requests))
        sys.stdout = io.StringIO()
        try:
            mcp_playtest.serve("no-session")
            return [json.loads(ln) for ln in sys.stdout.getvalue().splitlines()]
        finally:
            sys.stdin, sys.stdout = stdin, stdout

    def test_handshake_tools_and_errors(self):
        out = self.run_server([
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "nope", "arguments": {}}},
            {"jsonrpc": "2.0", "id": 4, "method": "nope"},
        ])
        self.assertEqual([m["id"] for m in out], [1, 2, 3, 4])  # the notification is not answered
        self.assertEqual(out[0]["result"]["protocolVersion"], "2025-06-18")
        self.assertEqual([t["name"] for t in out[1]["result"]["tools"]],
                         ["state", "act", "shot", "checkpoint", "desync_check"])
        self.assertTrue(out[2]["result"]["isError"])
        self.assertEqual(out[3]["error"]["code"], -32601)

    def test_desync_check_tool_says_incomplete(self):
        with tempfile.TemporaryDirectory() as d:
            now = time.time()
            write_checkpoint(d, "t1", None, {"F1": 0}, now - 20)
            write_checkpoint(d, "t2", {"F1": 0}, {"F1": 0}, now - 10)
            self.assertEqual(mcp_playtest.call(d, "desync_check", {})["result"], "incomplete")


class Parse(unittest.TestCase):
    def test_dump_is_a_module_step(self):
        steps = hookshot.parse("dump after_chest\n")
        self.assertEqual(steps[0].kind, "dump")
        self.assertIn("dump", hookshot.MODULE_ONLY)
        for bad in ("dump\n", "dump a b\n", "dump ../x\n"):
            with self.assertRaises(hookshot.ScriptError):
                hookshot.parse(bad)


def start_args(**kw):
    a = dict(session="s", lane=None, game_a=None, game_b=None, server="coop_server.exe", port=None, boot_a=None,
             boot_b=None, load=None, no_load=False, no_restore=False, env=[], boot_timeout=900, max_minutes=30,
             idle_minutes=10, lock_wait=0)
    a.update(kw)
    return argparse.Namespace(**a)


def write_session(d, **kw):
    s = {"started": 1000.0, "dir": d, "lane": None, "port": 27593, "booted": True, "max_s": 1800, "idle_s": 600,
         "lock_wait": 0, "offsets": {},
         "games": {x: {"pipe": "p-" + x, "commands": os.path.join(d, "commands-%s.txt" % x),
                       "log": os.path.join(d, "game-out-%s.txt" % x)} for x in live.TAGS}}
    s.update(kw)
    live.save(d, s)
    return s


class LiveLane(unittest.TestCase):
    def test_a_lane_gives_its_pair_port_and_pipes(self):
        L = lanes.lane(1)
        self.assertEqual(live.resolve(start_args(lane=1)), (L["a"], L["b"], L["port"], "coop-lane1"))
        self.assertEqual(live.resolve(start_args(lane=1, port=27999))[2], 27999)
        self.assertEqual(live.resolve(start_args(game_a="ga", game_b="gb")), ("ga", "gb", live.PORT, live.PIPE))
        with self.assertRaises(SystemExit):
            live.resolve(start_args(game_a="ga"))

    def test_runner_restores_the_saves_and_never_takes_the_lane_itself(self):
        argv = live.runner_argv(start_args(lane=1), "ga", "gb", 27520, "coop-lane1", "dir")
        self.assertNotIn("--no-restore", argv)   # the baseline save, as lane runs
        self.assertNotIn("--lane", argv)         # the watchdog holds the lock for the whole session
        self.assertIn("--keep-open", argv)
        self.assertEqual(argv[argv.index("--pipe") + 1], "coop-lane1")
        self.assertEqual(argv[argv.index("--port") + 1], "27520")
        self.assertIn("--no-restore", live.runner_argv(start_args(no_restore=True), "ga", "gb", 1, "p", "dir"))

    def test_the_runner_keeps_pids_for_the_watchdog(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "pids.json"), "w", encoding="utf-8") as f:
                json.dump({"games": {"A": 11, "B": 12}, "server": 13}, f)
            self.assertEqual(live.game_pids(d), ({"A": 11, "B": 12}, 13))
            os.remove(os.path.join(d, "pids.json"))
            self.assertEqual(live.game_pids(d), ({}, None))


class FakeClock:
    def __init__(self, t):
        self.t = t

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


class LiveWatchdog(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def pids(self, games, server=None):
        with open(os.path.join(self.d, "pids.json"), "w", encoding="utf-8") as f:
            json.dump({"games": games, "server": server}, f)

    def test_maximum_duration_ends_the_session(self):
        write_session(self.d, idle_s=0)
        c = FakeClock(1000.0)
        self.assertIn("maximum duration (30 min)", live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: True))
        self.assertGreaterEqual(c.t, 1000.0 + 1800)

    def test_idle_ends_only_after_the_boot(self):
        write_session(self.d, booted=False)
        live.touch(self.d)
        os.utime(live.active_file(self.d), (1000.0, 1000.0))
        c = FakeClock(1000.0)
        # still booting: no idle end, only the maximum
        self.assertIn("maximum", live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: True))
        write_session(self.d, booted=True)
        c = FakeClock(1000.0)
        self.assertEqual(live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: True),
                         "no tester command for 10 min")

    def test_stop_request_and_a_game_that_ended(self):
        write_session(self.d)
        live.touch(self.d)
        self.pids({"A": 11, "B": 12})
        c = FakeClock(time.time())
        write_session(self.d, started=c.t)
        self.assertEqual(live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: p != 12), "game B ended")
        open(live.stop_file(self.d), "w").close()
        self.assertEqual(live.watch(self.d, clock=c, sleep=c.sleep, alive=lambda p: True), "stop asked")

    def test_close_quits_kills_what_stays_and_lets_the_lane_go(self):
        s = write_session(self.d, lane=1)
        self.pids({"A": 11, "B": 12}, server=13)
        quits, kills, released = [], [], []
        c = FakeClock(0.0)
        out = live.close(self.d, s, alive=lambda p: p in (12, 13) and p not in kills,
                         quit=lambda s, x: quits.append(x), killer=kills.append,
                         release=lambda n, o: released.append((n, o)) or True, clock=c, sleep=c.sleep)
        self.assertEqual(quits, ["A", "B"])
        self.assertEqual(kills, [12, 13])   # B did not quit; the server always goes
        self.assertEqual(released, [(1, live.lock_owner(self.d))])
        self.assertEqual(out["B"], "quit asked, killed")
        self.assertEqual(out["lock"], "lane1.lock released")

    def test_close_without_a_lane_releases_nothing(self):
        s = write_session(self.d)
        released = []

        def unreachable(s, x):
            raise OSError("no pipe")
        out = live.close(self.d, s, alive=lambda p: False, quit=unreachable, killer=lambda p: None,
                         release=lambda n, o: released.append(n), wait=0)
        self.assertEqual(released, [])
        self.assertIn("not reached", out["A"])

    def test_lane_lock_taken_held_and_released_also_on_an_error(self):
        write_session(self.d, lane=1, lock_wait=5)
        taken, closed = [], []

        def boom(d):
            w = live.read_json(live.watchdog_file(d))
            self.assertEqual((w["lock"], w["pid"], w["ended"]), ("held", 4242, None))
            raise RuntimeError("bridge gone")
        code = live.run_watchdog(self.d, acquire=lambda n, o, wait: taken.append((n, o, wait)) or True,
                                 watcher=boom, closer=lambda d, s: closed.append(s["lane"]) or {"lock": "released"},
                                 pid=4242)
        self.assertEqual(code, 0)
        self.assertEqual(taken, [(1, live.lock_owner(self.d), 5)])
        self.assertEqual(closed, [1])
        w = live.read_json(live.watchdog_file(self.d))
        self.assertIn("watchdog error: RuntimeError: bridge gone", w["ended"])
        self.assertEqual(w["closed"], {"lock": "released"})

    def test_a_signal_still_cleans_up(self):
        write_session(self.d)
        closed = []

        def signalled(d):
            raise SystemExit("signal 15")
        live.run_watchdog(self.d, watcher=signalled, closer=lambda d, s: closed.append(1) or {}, pid=1)
        self.assertEqual(closed, [1])
        self.assertIn("watchdog stopped (signal 15)", live.ended(self.d))

    def test_a_busy_lane_refuses_the_session(self):
        write_session(self.d, lane=1)
        closed = []
        code = live.run_watchdog(self.d, acquire=lambda n, o, wait: False, watcher=lambda d: "x",
                                 closer=lambda d, s: closed.append(1), pid=1)
        self.assertEqual(code, 1)
        self.assertEqual(closed, [])   # nothing started, nothing to close
        w = live.read_json(live.watchdog_file(self.d))
        self.assertEqual(w["lock"], "refused")
        self.assertIn("lane 1 is not free", w["ended"])

    def test_commands_refused_after_the_end(self):
        write_session(self.d)
        live.write_json(live.watchdog_file(self.d),
                        {"pid": 1, "lock": "none", "ended": "maximum duration (30 min) reached"})
        with self.assertRaises(SystemExit) as e:
            live.state(argparse.Namespace(session=self.d, player=None))
        self.assertIn("maximum duration", str(e.exception))

    def test_stop_without_a_watchdog_closes_here(self):
        write_session(self.d)
        live.write_json(live.watchdog_file(self.d), {"pid": 999999, "lock": "none", "ended": None})
        old = live.close
        live.close = lambda d, s, alive=None: {"A": "quit asked"}
        try:
            out = live.stop(argparse.Namespace(session=self.d), alive=lambda p: False)
        finally:
            live.close = old
        self.assertIn("without the watchdog", out["ended"])
        self.assertTrue(os.path.exists(live.stop_file(self.d)))


class FakeBridge:
    def __init__(self, x, shots):
        self.x, self.shots = x, shots

    def request(self, cmd, **kw):
        if cmd == "shot":
            self.shots.append((self.x, kw["path"]))
            return {}
        return {"map": "ForestTemple", "game": {}}

    def close(self):
        pass


class LiveCheckpoint(unittest.TestCase):
    def test_a_checkpoint_has_a_shot_of_each_game(self):
        with tempfile.TemporaryDirectory() as d:
            write_session(d)
            shots = []

            class Player:
                def __init__(self, b, log, commands=None, **kw):
                    self.commands = commands

                def play(self, steps):
                    with open(self.commands + ".dump-%s.json" % steps[0].name, "w", encoding="utf-8") as f:
                        json.dump({"flags": {"F": 1}}, f)
            saved = live.bridge, hookshot.Player, hookshot.Log
            live.bridge = lambda s, x: FakeBridge(x, shots)
            hookshot.Player = Player
            hookshot.Log = lambda path: None
            try:
                out = live.checkpoint(argparse.Namespace(session=d, tag="after_chest"))
            finally:
                live.bridge, hookshot.Player, hookshot.Log = saved
            cps = os.path.join(d, "checkpoints")
            self.assertEqual(shots, [("A", os.path.join(cps, "after_chest-A.png")),
                                     ("B", os.path.join(cps, "after_chest-B.png"))])
            self.assertEqual(out["A"]["shot"], os.path.join(cps, "after_chest-A.png"))
            cp = live.read_json(os.path.join(cps, "after_chest.json"))
            self.assertEqual(cp["B"]["shot"], os.path.join(cps, "after_chest-B.png"))
            self.assertEqual(cp["A"]["flags"], {"F": 1})


if __name__ == "__main__":
    unittest.main()
