"""Selftests of the short gate's kit smoke on the bridge (tools/autotest/ingame/test_kit.py, --kit-ls, lane_record.py):
no game."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
sys.path.insert(0, INGAME)
import pair as P          # noqa: E402
import bt_plugin as B     # noqa: E402


def make_pair(kit_ls, lane=1, patches="patches=0001 0002"):
    server = os.path.join(kit_ls, "coop_server.exe")
    with open(server, "wb") as f:
        f.write(b"server")
    for x in ("A", "B"):
        d = os.path.join(kit_ls, "lane%d-%s" % (lane, x))
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "hookshot_coop.dll"), "wb") as f:
            f.write(b"module")
        with open(os.path.join(d, "coop-built-from.txt"), "w", encoding="utf-8") as f:
            f.write("pin=v3.2.0\n%s\n" % patches)
    return server


class LaneRecord(unittest.TestCase):
    def setUp(self):
        self.ls = tempfile.mkdtemp()
        self.server = make_pair(self.ls)

    def test_written_record_passes_lane_check(self):
        for x in ("A", "B"):
            P.write_lane_server(os.path.join(self.ls, "lane1-%s" % x), "abcdef0123", self.server, "16", "t")
        info = P.lane_check(self.ls, 1)
        self.assertEqual(info["commit"], "abcdef0123")
        self.assertEqual(info["server"], os.path.abspath(self.server))

    def test_no_patches_line_is_setup_fail(self):
        os.remove(os.path.join(self.ls, "lane1-A", "coop-built-from.txt"))
        with self.assertRaises(P.SetupFail):
            P.write_lane_server(os.path.join(self.ls, "lane1-A"), "abc", self.server)

    def test_cli_stdout_is_one_json_object(self):
        # (C #132: a tool read as JSON prints only the JSON; one REAL run through the same reader as the gate's)
        p = subprocess.run([sys.executable, "-I", os.path.join(INGAME, "lane_record.py"), self.ls, "--lane", "1",
                            "--commit", "abcdef0123", "--server", self.server, "--protocol", "16"],
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)
        v = json.loads(p.stdout)
        self.assertEqual((v["ok"], v["commit"]), (True, "abcdef0123"))

    def test_cli_setup_fail_exit_2(self):
        os.remove(os.path.join(self.ls, "lane1-B", "hookshot_coop.dll"))
        p = subprocess.run([sys.executable, "-I", os.path.join(INGAME, "lane_record.py"), self.ls, "--commit", "abc",
                            "--server", self.server], capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 2)
        self.assertFalse(json.loads(p.stdout)["ok"])


class KitBoot(unittest.TestCase):
    def cfg(self, kit_ls):
        class Cfg:
            def getoption(self, name):
                return {"env": [], "session_minutes": 60, "boot": B.DEFAULT_BOOT, "kit_ls": kit_ls}[name]
        return Cfg()

    def test_kit_ls_boots_its_own_folders(self):
        args = B.boot_args(self.cfg(r"C:\kit ls"), 1, "srv.exe", "sess")
        self.assertEqual(args[args.index("--game-a") + 1], os.path.join(r"C:\kit ls", "lane1-A"))
        self.assertEqual(args[args.index("--game-b") + 1], os.path.join(r"C:\kit ls", "lane1-B"))
        self.assertIn("--lane", args)   # (the lane's ports, pipe and lock stay the lane's)

    def test_no_kit_ls_boots_the_lane(self):
        args = B.boot_args(self.cfg(None), 1, "srv.exe", "sess")
        self.assertNotIn("--game-a", args)
        self.assertNotIn("--game-b", args)


class KitTestsCollect(unittest.TestCase):
    def test_deselected_without_kit_ls(self):
        out = tempfile.mkdtemp()
        p = subprocess.run([sys.executable, "-m", "pytest", "-p", "no:cacheprovider", "-q", "--fake", "--out", out,
                            "-k", "test_kit", "."], cwd=INGAME, capture_output=True, text=True, timeout=300)
        self.assertIn("deselected: needs_kit (no --kit-ls)", p.stdout + p.stderr)
        self.assertIn("test_kit_overlay_game_look", p.stdout + p.stderr)


if __name__ == "__main__":
    unittest.main()
