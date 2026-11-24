"""Tests for the outside half of the play-test bridge, against a fake game behind a real 127.0.0.1 socket speaking the
bridge protocol (docs/playtest.md, "Bridge protocol"): the bridge client (playtest_client.py: one connection, the
hold, reconnects), walk_to's camera-relative steering, every tool of one game's MCP server (mcp_playtest.py --player)
and desync_check.py --clients. Run: python -I tools/autotest/tests/test_playtest_bridge.py"""
import io
import json
import math
import os
import socketserver
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import argparse  # noqa: E402
import desync_check  # noqa: E402
import live  # noqa: E402
import mcp_playtest  # noqa: E402
import playtest_client as pc  # noqa: E402


class FakeGame:
    """A game's play-test bridge: held, it runs only the frames a command asks for; a closed connection lets it run.
    Link walks 0.1 m a frame at full stick toward world yaw camera + bias + sign * stick angle (the real sign and
    bias are what walk_to has to learn). `camera` None: the state reports no camera."""

    def __init__(self, camera=0.0, sign=1, bias=0.0, pos=(0.0, 0.0, 0.0), scene="ForestTemple", dump_dir=None):
        self.camera, self.sign, self.bias = camera, sign, bias
        self.pos, self.scene, self.frame, self.paused = list(pos), scene, 1000, False
        self.flags, self.items, self.log = {"F1": 0, "F2": 0}, {}, []
        self.dump_dir = dump_dir
        self.wall_x = None      # Link cannot pass this x
        self.scene_at = None    # (frame, scene): the scene changes once the frame count reaches it
        self.connections = 0
        game = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                game.connections += 1
                for line in self.rfile:
                    msg = json.loads(line)
                    game.log.append(msg)
                    reply = dict(game.answer(msg), id=msg.get("id"), frame=game.frame, paused=game.paused)
                    self.wfile.write((json.dumps(reply) + "\n").encode())
                game.paused = False     # the hold lifts when the tester's connection closes

        self.server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, args=(0.05,), daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()

    def run(self, frames, stick=None):
        for _ in range(frames):
            self.frame += 1
            if stick and stick[1] > 0:
                yaw = math.radians((self.camera or 0.0) + self.bias + self.sign * stick[0])
                nx = self.pos[0] + 0.1 * stick[1] * math.sin(yaw)
                if self.wall_x is None or nx < self.wall_x:
                    self.pos[0] = nx
                self.pos[2] += 0.1 * stick[1] * math.cos(yaw)
            if self.scene_at and self.frame >= self.scene_at[0]:
                self.scene = self.scene_at[1]

    def state(self):
        return {"scene": self.scene, "room": None, "pos": [round(v, 4) for v in self.pos], "facing": 0.0,
                "camera": self.camera, "life": 12, "max_life": 12, "rupees": 5, "age": "child", "event": False,
                "loading": False, "prompt": None, "equips": {}, "inventory": dict(self.items),
                "coop": {"state": "connected", "players": 2}, "snapshot_frame": self.frame}

    def answer(self, m):
        cmd = m.get("cmd")
        if cmd == "pause":
            self.paused = True
            return {"ok": True}
        if cmd == "resume":
            self.paused = False
            return {"ok": True}
        if cmd == "state":
            return dict(self.state(), ok=True)
        if cmd in ("input", "step"):
            start = self.frame
            self.run(m.get("frames", 1), m.get("stick"))
            self.paused = True
            return {"ok": True, "from": start, "state": self.state()}
        if cmd == "warp":
            self.scene = m["scene"]
            self.paused = True
            return {"ok": True, "arrived": True, "state": self.state()}
        if cmd == "flag":
            was = self.flags.get(m["name"])
            self.flags[m["name"]] = m["value"]
            return {"ok": True, "was": was, "value": m["value"]}
        if cmd == "give":
            self.items[m["item"]] = self.items.get(m["item"], 0) + 1
            return {"ok": True, "given": m["item"]}
        if cmd == "dump":
            path = os.path.join(self.dump_dir, "%s.json" % m["tag"])
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"frame": self.frame, "tag": m["tag"], "state": self.state(), "flags": self.flags}, f)
            return {"ok": True, "path": path, "flags": len(self.flags)}
        if cmd == "shot":
            if "path" in m:
                return {"ok": False, "error": "bad_request", "message": "no client paths"}
            self.run(1)
            folder = os.path.join(self.dump_dir, "playtest", "shots")
            os.makedirs(folder, exist_ok=True)
            path = os.path.join(folder, "%s.png" % m.get("name", "shot"))
            with open(path, "wb") as f:
                f.write(b"PNG")
            return {"ok": True, "path": path, "width": 1280, "height": 720, "advanced": 1}
        return {"ok": False, "error": "bad_request", "message": "unknown cmd %s" % cmd}


class BridgeClient(unittest.TestCase):
    def setUp(self):
        self.game = FakeGame()
        self.c = pc.Client(self.game.port)

    def tearDown(self):
        self.c.close()
        self.game.close()

    def test_one_connection_held_from_the_first_call(self):
        a = self.c.state()
        self.assertTrue(self.game.paused)                    # held before the first command was answered
        self.assertEqual(self.game.log[0]["cmd"], "pause")
        self.assertEqual(self.c.state()["frame"], a["frame"])   # held: two looks, one frame
        r = self.c.step(60)
        self.assertEqual(r["frame"] - r["from"], 60)
        self.assertEqual(self.c.last_frame, a["frame"] + 60)
        self.assertEqual(self.game.connections, 1)
        ids = [m["id"] for m in self.game.log]
        self.assertEqual(ids, sorted(set(ids)))

    def test_closing_lets_the_game_run_and_a_reconnect_holds_it_again(self):
        self.c.state()
        self.c.close()
        deadline = time.monotonic() + 5
        while self.game.paused and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertFalse(self.game.paused)
        r = self.c.state()
        self.assertTrue(r.get("reconnected"))
        self.assertTrue(self.game.paused)
        self.assertEqual(self.game.connections, 2)

    def test_refusal_raises_with_the_code(self):
        with self.assertRaises(pc.BridgeError) as e:
            self.c.request("nope")
        self.assertEqual(e.exception.code, "bad_request")
        self.assertFalse(self.c.request("nope", check=False)["ok"])

    def test_bad_frames_refused_here(self):
        for n in (0, 3601):
            with self.assertRaises(ValueError):
                self.c.step(n)

    def test_local_only(self):
        with self.assertRaises(ValueError):
            pc.Client(5001, host="192.168.1.2")

    def test_ports(self):
        self.assertEqual([pc.port_for(p) for p in pc.PLAYERS], [5001, 5002, 5003, 5004])
        self.assertEqual(pc.port_for("P2", lane=1), 5012)
        self.assertEqual(pc.port_for("p4", lane=2), 5024)
        with self.assertRaises(ValueError):
            pc.port_for("p5")


class WalkTo(unittest.TestCase):
    def walk(self, game, x, z, **kw):
        c = pc.Client(game.port)
        try:
            return pc.walk_to(c, x, z, **kw)
        finally:
            c.close()
            game.close()

    def test_arrives_for_any_camera(self):
        for cam in (0.0, 90.0, 200.0, -135.0):
            r = self.walk(FakeGame(camera=cam), 6.0, -4.0)
            self.assertTrue(r["arrived"], (cam, r["reason"], r["distance"]))
            self.assertLessEqual(r["distance"], 0.5)

    def test_learns_a_mirrored_stick_and_an_offset(self):
        for sign, bias in ((-1, 0.0), (1, 40.0), (-1, -120.0), (1, 180.0)):
            r = self.walk(FakeGame(camera=30.0, sign=sign, bias=bias), -5.0, 7.0, tolerance=0.3, max_frames=1500)
            self.assertTrue(r["arrived"], (sign, bias, r["reason"], r["distance"]))

    def test_no_camera_in_the_state(self):
        self.assertTrue(self.walk(FakeGame(camera=None, bias=70.0), 4.0, 4.0, max_frames=900)["arrived"])

    def test_held_between_chunks_and_frames_counted(self):
        game = FakeGame()
        r = self.walk(game, 3.0, 0.0, tolerance=0.2)
        self.assertTrue(r["arrived"])
        inputs = [m for m in game.log if m["cmd"] == "input"]
        self.assertTrue(inputs and all(m["frames"] <= 10 for m in inputs))
        self.assertEqual(sum(m["frames"] for m in inputs), r["frames"])
        self.assertTrue(all(0.0 <= m["stick"][0] < 360.0 and 0.0 < m["stick"][1] <= 1.0 for m in inputs))

    def test_stops_on_a_scene_change(self):
        game = FakeGame()
        game.scene_at = (1030, "ForestTemple_Room2")
        r = self.walk(game, 50.0, 0.0)
        self.assertFalse(r["arrived"])
        self.assertEqual(r["reason"], "scene changed")

    def test_times_out_against_a_wall(self):
        game = FakeGame()
        game.wall_x = 1.0
        r = self.walk(game, 5.0, 0.0, max_frames=200)
        self.assertFalse(r["arrived"])
        self.assertEqual(r["reason"], "timed out")
        self.assertEqual(r["frames"], 200)


class PlayerServer(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.games = {p: FakeGame(dump_dir=self.tmp.name if p == "p1" else tempfile.mkdtemp(dir=self.tmp.name))
                      for p in ("p1", "p2")}
        self.tools = {p: mcp_playtest.Game(p, pc.Client(g.port), self.tmp.name) for p, g in self.games.items()}

    def tearDown(self):
        for t in self.tools.values():
            t.client.close()
        for g in self.games.values():
            g.close()
        self.tmp.cleanup()

    def call(self, p, tool, **args):
        return self.tools[p].call(tool, args)

    def test_tools_listed_and_named_per_player(self):
        reqs = [{"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                 "params": {"name": "step", "arguments": {"frames": 5}}},
                {"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "state", "arguments": {}}}]
        stdin, stdout = sys.stdin, sys.stdout
        sys.stdin = io.StringIO("".join(json.dumps(r) + "\n" for r in reqs))
        sys.stdout = io.StringIO()
        try:
            mcp_playtest.serve(None, self.tools["p2"])
            out = [json.loads(ln) for ln in sys.stdout.getvalue().splitlines()]
        finally:
            sys.stdin, sys.stdout = stdin, stdout
        self.assertEqual(out[0]["result"]["serverInfo"]["name"], "oot-p2")
        self.assertEqual([t["name"] for t in out[1]["result"]["tools"]],
                         ["get_state", "walk_to", "press", "step", "warp", "flag", "give", "checkpoint",
                          "desync_check", "pause", "resume", "log", "screenshot"])
        self.assertEqual(json.loads(out[2]["result"]["content"][0]["text"])["frame"], 1005)
        self.assertTrue(out[3]["result"]["isError"])          # the session tools are not a game's tools

    def test_press_warp_flag_give_step_resume(self):
        r = self.call("p1", "press", buttons=["a", "ZL"], stick_angle=90, stick_strength=1.0, frames=4)
        self.assertEqual(r["frame"], 1004)
        sent = self.games["p1"].log[-1]
        self.assertEqual((sent["cmd"], sent["buttons"], sent["stick"], sent["frames"]), ("input", ["A", "ZL"],
                                                                                       [90.0, 1.0], 4))
        self.assertEqual(self.call("p1", "press")["frame"], 1006)      # 2 frames by default, no stick
        self.assertNotIn("stick", self.games["p1"].log[-1])
        with self.assertRaises(ValueError):
            self.call("p1", "press", buttons=["Q"])
        r = self.call("p1", "warp", scene="KokiriForest_Child", entrance="Start_Mido")
        self.assertEqual(r["state"]["scene"], "KokiriForest_Child")
        self.assertEqual((self.games["p1"].log[-1]["entrance"], self.games["p1"].log[-1]["timeout"]), ("Start_Mido", 60))
        self.call("p1", "warp", scene="DekuTree")
        self.assertNotIn("entrance", self.games["p1"].log[-1])
        self.assertEqual(self.call("p1", "flag", name="F2", value=True)["was"], 0)
        self.assertIs(self.games["p1"].flags["F2"], True)
        self.call("p1", "give", item="bombs")
        self.assertEqual(self.games["p1"].items, {"bombs": 1})
        self.assertEqual(self.call("p1", "step", frames=30)["frame"], 1036)
        self.assertFalse(self.call("p1", "resume")["paused"])
        self.assertTrue(self.call("p1", "pause")["paused"])

    def test_walk_to_tool(self):
        self.assertTrue(self.call("p1", "walk_to", x=2.0, z=2.0)["arrived"])

    def test_checkpoint_every_game_then_desync_check(self):
        for p in ("p1", "p2"):
            self.assertEqual(self.call(p, "checkpoint", tag="start")["flags"], 2)
        self.games["p1"].flags["F1"] = 1                       # p1 opened the chest ...
        self.call("p1", "step", frames=10)
        for p in ("p1", "p2"):
            self.call(p, "checkpoint", tag="after_chest")
        r = self.call("p2", "desync_check")
        self.assertEqual(r["result"], "desync")
        self.assertIn("start: agree", r["report"])
        self.assertIn("after_chest: 1 flag(s) differ", r["report"])
        self.assertIn("F1: p1 1, p2 0", r["report"])
        self.assertIn("p1 frame 1010", r["report"])
        self.games["p2"].flags["F1"] = 1                       # ... and it reached p2
        for p in ("p1", "p2"):
            self.call(p, "checkpoint", tag="after_chest")
        self.assertIn("after_chest#2: agree", self.call("p1", "desync_check", all=True)["report"])
        with self.assertRaises(ValueError):
            self.call("p1", "checkpoint", tag="bad tag")

    def test_checkpoint_without_a_dump_is_not_compared(self):
        self.games["p2"].dump_dir = os.path.join(self.tmp.name, "missing")
        self.call("p1", "checkpoint", tag="t")
        with self.assertRaises(OSError):                       # the fake cannot write: the bridge would say failed
            self.call("p2", "checkpoint", tag="t")
        self.assertEqual(self.call("p1", "desync_check")["result"], "nothing to compare")

    def test_screenshot(self):
        r = self.call("p2", "screenshot", name="door")
        self.assertNotIn("path", self.games["p2"].log[-1])     # a name only: the game picks the folder
        self.assertTrue(os.path.exists(r["path"]))
        self.assertTrue(os.path.basename(r["path"]).startswith("p2-door-"))
        self.assertEqual(r["advanced"], 1)
        self.assertEqual(os.path.dirname(r["copy"]), os.path.join(self.tmp.name, "shots"))   # beside the logs
        self.assertTrue(os.path.exists(r["copy"]))
        with self.assertRaises(ValueError):
            self.call("p2", "screenshot", name="../x")

    def test_a_live_session_is_kept_awake_and_an_ended_one_refused(self):
        d = os.path.join(self.tmp.name, "session")
        os.makedirs(d)
        live.save(d, {"started": 1000.0, "games": {}, "offsets": {}})
        g = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d)
        live.touch(d)
        os.utime(live.active_file(d), (1000.0, 1000.0))          # the last command long ago
        g.call("step", {"frames": 1})
        self.assertGreater(live.activity(d, {}), 1000.0 + 3600)   # touched now: the watchdog's idle clock restarts
        live.write_json(live.watchdog_file(d), {"ended": "idle"})
        with self.assertRaises(SystemExit):
            g.call("step", {"frames": 1})

    def test_log_lines_since_the_last_call(self):
        d = os.path.join(self.tmp.name, "session")
        os.makedirs(d)
        logs = {x: os.path.join(d, "game-out-%s.txt" % x) for x in live.TAGS}
        with open(logs["A"], "w", encoding="utf-8", newline="") as f:
            f.write("boot line coop: before\n")
        live.save(d, {"started": 1000.0, "offsets": {"A": os.path.getsize(logs["A"]), "B": 0},
                      "games": {x: {"log": logs[x]} for x in live.TAGS}})
        g = mcp_playtest.Game("p1", self.tools["p1"].client, self.tmp.name, session=d)
        with open(logs["A"], "a", encoding="utf-8", newline="") as f:
            f.write("[x] coop: objects: rocks: 3 new falling rocks in our game since the last line (3 live)\n"
                    "[x] coop: other\nno co-op here\n")
        out = g.call("log", {"match": "rocks:"})
        self.assertEqual(len(out["lines"]), 1)
        self.assertIn("3 new falling rocks", out["lines"][0])
        self.assertEqual(g.call("log", {})["lines"], [])                 # nothing new since
        self.assertEqual(live.load(d)["offsets"]["A"], len("boot line coop: before\n"))   # session file untouched
        with self.assertRaises(ValueError):
            self.tools["p1"].call("log", {})                             # no --session
        with self.assertRaises(ValueError):
            mcp_playtest.Game("p3", self.tools["p1"].client, self.tmp.name, session=d).log_lines()

    def test_main_needs_session_or_player(self):
        err = sys.stderr
        sys.stderr = io.StringIO()
        try:
            for argv in ([], ["--player", "p9"], ["--player", "p1", "--lane", "x"]):   # (--session with --player is valid)
                with self.assertRaises(SystemExit):
                    mcp_playtest.main(argv)
        finally:
            sys.stderr = err


class DesyncClients(unittest.TestCase):
    def write(self, d, player, rows):
        with open(pc.checkpoint_file(d, player), "w", encoding="utf-8") as f:
            for r in rows:
                f.write(json.dumps(r) + "\n")

    def test_codes(self):
        out = []
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(desync_check.check_clients(d, out=out.append), 2)      # no logs
            self.write(d, "p1", [{"tag": "t", "frame": 5, "flags": {"F": 1}}])
            self.assertEqual(desync_check.check_clients(d, out=out.append), 2)      # one game only
            self.write(d, "p2", [{"tag": "t", "frame": 9, "flags": {"F": 1}}])
            self.write(d, "p3", [{"tag": "t", "frame": 7, "flags": {"F": 1, "G": 2}}])
            self.assertEqual(desync_check.check_clients(d, out=out.append), 0)      # G only p3 reads: no desync
            self.write(d, "p2", [{"tag": "t", "frame": 9, "flags": {"F": 1}}, {"tag": "u", "frame": 20, "flags": {}}])
            self.assertEqual(desync_check.check_clients(d, out=out.append), 3)      # u missing in p1 and p3
            self.write(d, "p2", [{"tag": "t", "frame": 9, "flags": None, "error": "no dump"}])
            self.assertEqual(desync_check.check_clients(d, out=out.append), 2)      # nothing compared

    def test_frame_order_and_command_line(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(d, "p1", [{"tag": "b", "frame": 20, "flags": {"F": 2}},
                                 {"tag": "a", "frame": 10, "flags": {"F": 1}}])
            self.write(d, "p2", [{"tag": "a", "frame": 11, "flags": {"F": 1}},
                                 {"tag": "b", "frame": 21, "flags": {"F": 3}}])
            out = []
            self.assertEqual(desync_check.check_clients(d, out=out.append), 1)
            self.assertTrue(out[0].startswith("a: agree"), out)
            self.assertIn("first desync", out[1])
            stdout = sys.stdout
            sys.stdout = io.StringIO()
            try:
                self.assertEqual(desync_check.main([d, "--clients"]), 1)
            finally:
                sys.stdout = stdout



class LiveStartPlaytest(unittest.TestCase):
    def args(self, **kw):
        a = dict(session="s", lane=None, game_a="ga", game_b="gb", server="coop_server.exe", port=None, boot_a=None,
                 boot_b=None, load=None, no_load=False, no_restore=False, env=[], boot_timeout=900, max_minutes=30,
                 idle_minutes=10, lock_wait=0, playtest=False)
        a.update(kw)
        return argparse.Namespace(**a)

    def test_off_unless_asked(self):
        self.assertNotIn("--playtest-ports", live.runner_argv(self.args(), "ga", "gb", 1, "p", "dir"))

    def test_ports_match_the_mcp_servers(self):
        for lane in (None, 1, 2):
            argv = live.runner_argv(self.args(playtest=True, lane=lane), "ga", "gb", 1, "p", "dir")
            base = int(argv[argv.index("--playtest-ports") + 1])
            self.assertEqual((base + 1, base + 2), (pc.port_for("p1", lane), pc.port_for("p2", lane)))


if __name__ == "__main__":
    unittest.main()
