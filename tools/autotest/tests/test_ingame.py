"""Selftests of the in-game test system (tools/autotest/ingame, docs/ingame-tests.md), no game: fake bridges.

Needs pytest, pytest-xdist, pytest-timeout, Pillow and scikit-image (the tools venv and CI's python-tools job have them).
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
sys.path.insert(0, INGAME)
import pair as P          # noqa: E402
import imagecheck         # noqa: E402


def pytest_run(args, cwd=INGAME, env=None):
    e = dict(os.environ)
    e.update(env or {})
    p = subprocess.run([sys.executable, "-m", "pytest"] + args, cwd=cwd, capture_output=True, text=True, timeout=600,
                       env=e)
    return p


def summary(out):
    with open(os.path.join(out, "summary.json"), encoding="utf-8") as f:
        return json.load(f)


def results(s):
    return {r["test"].split("::")[-1]: r["result"] for r in s["tests"]}


class Suite(unittest.TestCase):
    """The real suite (smoke + pilot rows) against two fake games."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_no_refs_then_refs(self):
        out1, refs = os.path.join(self.tmp, "o1"), os.path.join(self.tmp, "refs")
        os.makedirs(refs)
        p = pytest_run([".", "--fake", "--out", out1, "-q"], env={"BT_FAKE_REFS": refs})
        s = summary(out1)
        self.assertEqual(s["counts"]["NO-REF"], 3, p.stdout[-2000:])
        self.assertEqual(s["counts"]["PASS"], 8, p.stdout[-2000:])
        self.assertEqual(s["verdict"], "NOT PASS")
        self.assertNotEqual(p.returncode, 0)
        new = os.path.join(out1, "lane1", "new")
        for f in os.listdir(new):                  # (a person approves the candidates: here, all of them)
            shutil.copyfile(os.path.join(new, f), os.path.join(refs, f))
        out2 = os.path.join(self.tmp, "o2")
        p = pytest_run([".", "--fake", "--out", out2, "-q"], env={"BT_FAKE_REFS": refs})
        s = summary(out2)
        self.assertEqual(s["counts"]["PASS"], 11, p.stdout[-2000:])
        self.assertEqual(s["verdict"], "PASS")
        self.assertEqual(p.returncode, 0)
        shots = [sh for r in s["tests"] for sh in r["shots"]]
        self.assertEqual(len(shots), 10)            # pause 2 + masks 4 + HUD 4: a few per rc, not hundreds
        self.assertTrue(all(sh["ok"] and sh["score"] >= sh["tolerance"] for sh in shots))

    def test_repeat_keeps_one_boot_and_orders_by_round(self):
        out = os.path.join(self.tmp, "o")
        pytest_run([".", "--fake", "--out", out, "-q", "--repeat", "2", "--no-smoke-gate"])
        s = summary(out)
        names = [r["test"] for r in s["tests"]]
        self.assertEqual(len(names), 22)
        self.assertTrue(all("[r1]" in n for n in names[:11]) and all("[r2]" in n for n in names[11:]), names)
        self.assertIn("test_boot", names[0])     # smoke first in each round
        self.assertIn("test_boot", names[11])

    def test_two_lanes_at_once(self):
        out = os.path.join(self.tmp, "o")
        p = pytest_run([".", "--fake", "--out", out, "-q", "-n", "2", "--lanes", "1,2", "-m", "not look"])
        s = summary(out)
        self.assertEqual(s["counts"]["PASS"], len(s["tests"]), p.stdout[-2000:])
        self.assertEqual(len(s["tests"]), 8)
        self.assertTrue(os.path.isdir(os.path.join(out, "lane1")) and os.path.isdir(os.path.join(out, "lane2")))


LABEL_TESTS = '''
import os, pytest, pair as P

def _count(name):
    f = os.path.join(os.environ["BT_COUNT_DIR"], name)
    n = int(open(f).read()) if os.path.exists(f) else 0
    open(f, "w").write(str(n + 1))
    return n

def test_ok(pair):
    pair.step_both(5)

def test_flaky(pair):
    assert _count("flaky") > 0, "fails the first time only"

def test_always_fails(pair):
    _count("always")
    assert False, "never holds"

def test_setup_fails(pair):
    with pair.setup():
        raise RuntimeError("no such scene")

@pytest.mark.budget(1)
def test_times_out(pair):
    pair.until("never", lambda: False, max_frames=10 ** 9)

@pytest.mark.smoke
def test_smoke_ok(pair):
    pass
'''


class Labels(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        d = os.path.join(self.tmp, "t")
        os.makedirs(d)
        with open(os.path.join(d, "conftest.py"), "w", encoding="utf-8") as f:
            f.write("import sys\nsys.path.insert(0, %r)\npytest_plugins = ['bt_plugin']\n" % INGAME)
        with open(os.path.join(d, "test_labels.py"), "w", encoding="utf-8") as f:
            f.write(LABEL_TESTS)
        self.dir, self.out = d, os.path.join(self.tmp, "o")
        self.env = {"BT_COUNT_DIR": self.tmp}

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_every_label(self):
        p = pytest_run([".", "--fake", "--out", self.out, "-q", "-p", "no:cacheprovider"], cwd=self.dir, env=self.env)
        r = results(summary(self.out))
        self.assertEqual(r, {"test_smoke_ok": "PASS", "test_ok": "PASS", "test_flaky": "FLAKY",
                             "test_always_fails": "FAIL", "test_setup_fails": "SETUP-FAIL",
                             "test_times_out": "FAIL"}, p.stdout[-3000:])
        rows = {x["test"].split("::")[-1]: x for x in summary(self.out)["tests"]}
        self.assertIn("timeout", rows["test_times_out"]["why"])
        self.assertIn("set-up: RuntimeError: no such scene", rows["test_setup_fails"]["why"])
        self.assertEqual(open(os.path.join(self.tmp, "always")).read(), "2")   # rerun once, no more
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(summary(self.out)["tests"][0]["test"].split("::")[-1], "test_smoke_ok")   # smoke first

    def test_smoke_gate_stops_the_run(self):
        with open(os.path.join(self.dir, "test_labels.py"), "a", encoding="utf-8") as f:
            f.write("\n@pytest.mark.smoke\ndef test_smoke_bad(pair):\n    assert False\n")
        pytest_run([".", "--fake", "--out", self.out, "-q"], cwd=self.dir, env=self.env)
        r = results(summary(self.out))
        self.assertEqual(r, {"test_smoke_ok": "PASS", "test_smoke_bad": "FAIL"})


class LaneCheck(unittest.TestCase):
    def make(self, root, x, dll=b"module", patches="patches=abc", server=None, **over):
        d = os.path.join(root, "lane1-%s" % x)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "hookshot_coop.dll"), "wb") as f:
            f.write(dll)
        with open(os.path.join(d, "coop-built-from.txt"), "w", encoding="utf-8") as f:
            f.write("commit=1\n%s\n" % patches)
        fields = {"server": server, "dll_sha256": hashlib.sha256(dll).hexdigest().upper(), "commit": "ae97cc772",
                  "patches_line_sha256": hashlib.sha256(patches.encode()).hexdigest().upper()}
        fields.update(over)
        with open(os.path.join(d, "lane-server.txt"), "w", encoding="utf-8") as f:
            f.write("".join("%s=%s\n" % kv for kv in fields.items()))

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.server = os.path.join(self.tmp, "coop_server.exe")
        open(self.server, "w").close()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_good_lane(self):
        for x in "AB":
            self.make(self.tmp, x, server=self.server)
        self.assertEqual(P.lane_check(self.tmp, 1)["commit"], "ae97cc772")

    def test_module_changed_after_install(self):
        for x in "AB":
            self.make(self.tmp, x, server=self.server)
        with open(os.path.join(self.tmp, "lane1-B", "hookshot_coop.dll"), "wb") as f:
            f.write(b"other")
        with self.assertRaisesRegex(P.SetupFail, "lane1-B hookshot_coop.dll .* is not lane-server.txt's"):
            P.lane_check(self.tmp, 1)

    def test_game_rebuilt_after_install(self):
        self.make(self.tmp, "A", server=self.server)
        self.make(self.tmp, "B", server=self.server, patches="patches=new",
                  patches_line_sha256=hashlib.sha256(b"patches=abc").hexdigest().upper())
        with self.assertRaisesRegex(P.SetupFail, "game patches line"):
            P.lane_check(self.tmp, 1)

    def test_no_server(self):
        for x in "AB":
            self.make(self.tmp, x, server=os.path.join(self.tmp, "gone.exe"))
        with self.assertRaisesRegex(P.SetupFail, "no server"):
            P.lane_check(self.tmp, 1)

    def test_not_installed(self):
        with self.assertRaisesRegex(P.SetupFail, "not installed by lane-game-for"):
            P.lane_check(self.tmp, 1)


class ImageCompare(unittest.TestCase):
    def setUp(self):
        from PIL import Image, ImageDraw
        self.tmp = tempfile.mkdtemp()
        self.paths = {}
        for name, box in (("a", (40, 40, 120, 100)), ("a2", (41, 40, 121, 100)), ("b", (200, 20, 300, 160))):
            im = Image.new("RGB", (640, 360), (30, 80, 30))
            ImageDraw.Draw(im).rectangle(box, fill=(220, 30, 30))
            self.paths[name] = os.path.join(self.tmp, name + ".png")
            im.save(self.paths[name])

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_same_close_and_different(self):
        p = self.paths
        self.assertTrue(imagecheck.compare(p["a"], p["a"], 0.99)["ok"])
        self.assertTrue(imagecheck.compare(p["a2"], p["a"], 0.90)["ok"])      # a pixel of animation
        r = imagecheck.compare(p["b"], p["a"], 0.90)
        self.assertFalse(r["ok"])
        self.assertIn("<", r["why"])

    def test_no_reference_is_never_ok(self):
        keep = os.path.join(self.tmp, "keep")
        r = imagecheck.compare(self.paths["a"], os.path.join(self.tmp, "refs", "x-A.png"), 0.5, keep_dir=keep)
        self.assertFalse(r["ok"])
        self.assertTrue(r["why"].startswith("NO-REF"))
        self.assertTrue(os.path.exists(os.path.join(keep, "new", "x-A.png")))


if __name__ == "__main__":
    unittest.main()
