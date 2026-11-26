"""Tests for tools/autotest/rc_plan.py: the rc's group files split over the test lanes by minutes, and the runs left
after a lane ended early. No game is started."""
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import groups as g  # noqa: E402
import rc_plan as p  # noqa: E402

ONE = """# file one
[g1]
walk  expect="A:coop: connected"  logonly="smoke"
teleport-same
[g2]
chest  timeout=900
"""
TWO = """[g1]
boss  label="big one"
[g3]
wear
turn
time
"""


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def fake_run(root, group, scenario, minutes, stamp="20261126-010000"):
    d = os.path.join(root, group, scenario, stamp)
    write(os.path.join(d, "run.json"), json.dumps({"group": group, "scenario": scenario}))
    end = int(minutes * 60)
    write(os.path.join(d, "steps.txt"), "23:59:00 A: started pid 1\n%02d:%02d:%02d scenario played\n" % (
        ((86340 + end) // 3600) % 24, ((86340 + end) // 60) % 60, (86340 + end) % 60))


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.one = os.path.join(self.tmp, "one.txt")
        self.two = os.path.join(self.tmp, "two.txt")
        write(self.one, ONE)
        write(self.two, TWO)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)


class Split(Base):
    def test_every_run_once_and_files_parse(self):
        doc = p.split([self.one, self.two], os.path.join(self.tmp, "out"), 2)
        seen = []
        for lane in doc["lanes"]:
            with open(lane["file"], encoding="utf-8") as f:
                for _, runs in g.parse_groups(f.read()):
                    seen += [r["scenario"] for r in runs]
        self.assertEqual(sorted(seen), sorted(["walk", "teleport-same", "chest", "boss", "wear", "turn", "time"]))

    def test_name_twice_gets_file_stem(self):
        names = [u[0] for u in p.units([self.one, self.two])]
        self.assertEqual(names, ["g1", "g2", "two-g1", "g3"])

    def test_lines_kept_as_written(self):
        doc = p.split([self.one], os.path.join(self.tmp, "out"), 1)
        with open(doc["lanes"][0]["file"], encoding="utf-8") as f:
            text = f.read()
        self.assertIn('walk  expect="A:coop: connected"  logonly="smoke"', text)
        self.assertIn("chest  timeout=900", text)

    def test_balanced_by_history(self):
        hist = os.path.join(self.tmp, "hist")
        fake_run(hist, "x", "boss", 30)
        for n in ("o1", "o2", "o3"):   # the median of all measured runs is the default: 4.0 + 0.5 here
            fake_run(hist, "x", n, 4)
        doc = p.split([self.one, self.two], os.path.join(self.tmp, "out"), 2, [hist])
        lanes = {l["lane"]: [u["name"] for u in l["groups"]] for l in doc["lanes"]}
        boss_lane = [n for n, gs in lanes.items() if "two-g1" in gs][0]
        self.assertEqual(lanes[boss_lane], ["two-g1"])   # 30.5 min alone; the rest (6 x 4.5) on the other lane
        self.assertAlmostEqual(doc["longest_lane_minutes"], 30.5, places=1)
        est = [e for l in doc["lanes"] for u in l["groups"] for e in u["estimates"] if e["scenario"] == "boss"][0]
        self.assertIn("earlier run", est["from"])

    def test_default_minutes_without_history(self):
        doc = p.split([self.one], os.path.join(self.tmp, "out"), 1)
        self.assertAlmostEqual(doc["total_minutes"], 3 * p.DEFAULT_MIN)

    def test_one_lane_holds_everything(self):
        doc = p.split([self.one, self.two], os.path.join(self.tmp, "out"), 1)
        self.assertEqual(len(doc["lanes"]), 1)
        self.assertEqual(len(doc["lanes"][0]["groups"]), 4)

    def test_more_lanes_than_groups(self):
        doc = p.split([self.one], os.path.join(self.tmp, "out"), 3)
        self.assertEqual([bool(l["file"]) for l in doc["lanes"]], [True, True, False])

    def test_bad_file_refused(self):
        bad = os.path.join(self.tmp, "bad.txt")
        write(bad, "walk\n")
        with self.assertRaises(p.Fail):
            p.split([bad], os.path.join(self.tmp, "out"), 2)

    def test_midnight_steps(self):
        hist = os.path.join(self.tmp, "hist")
        fake_run(hist, "x", "walk", 3)
        self.assertAlmostEqual(p.history([hist])["walk"][0], 3 + p.CLOSE_MIN)

    def test_cli(self):
        self.assertEqual(p.main(["split", self.one, "--out-dir", os.path.join(self.tmp, "o")]), 0)
        self.assertTrue(os.path.isfile(os.path.join(self.tmp, "o", "plan.json")))


    def test_default_is_median_of_measured_runs(self):
        hist = os.path.join(self.tmp, "hist")
        for n, m in (("a", 1), ("b", 2), ("c", 9)):
            fake_run(hist, "x", n, m)
        doc = p.split([self.one], os.path.join(self.tmp, "out"), 1, [hist])
        self.assertAlmostEqual(doc["total_minutes"], 3 * 2.5, places=1)
        self.assertIn("median", doc["lanes"][0]["groups"][0]["estimates"][0]["from"])


class History(Base):
    def test_run_ps1_folder_without_run_json(self):
        # run.ps1 alone writes <shots>/<scenario>/<stamp>/steps.txt and no run.json
        d = os.path.join(self.tmp, "shots", "chest", "20261126-010000")
        write(os.path.join(d, "steps.txt"), "01:00:00 A: started pid 1\n01:03:00 scenario played\n")
        write(os.path.join(self.tmp, "shots", "chest", "notastamp", "steps.txt"), "01:00:00 a\n01:30:00 b\n")
        h = p.history([os.path.join(self.tmp, "shots")])
        self.assertEqual(h, {"chest": [3.0 + p.CLOSE_MIN]})


class Merge(Base):
    def _split(self, text):
        f = os.path.join(self.tmp, "m.txt")
        write(f, text)
        return p.split([f], os.path.join(self.tmp, "mo"), 1)

    def test_same_run_merged_checks_kept(self):
        doc = self._split(r"""[a]
hs-ladder  expect='A:get_bool\(0xb3\) = 1'  label="deku 3"  see=hs-ladder-A
[b]
walk
hs-ladder  expect='A:get_bool\(0xb3\) = 1'  expect='B:result: played'  label="#177 ladder"  see=hs-ladder-B
""")
        self.assertEqual(doc["merged"], [{"into": "a", "from": "b", "scenario": "hs-ladder"}])
        with open(doc["lanes"][0]["file"], encoding="utf-8") as f:
            runs = dict(g.parse_groups(f.read()))
        lad = runs["a"][0]
        self.assertEqual(lad["expects"], [("A", r"get_bool\(0xb3\) = 1"), ("B", "result: played")])
        self.assertEqual(lad["see"], ["hs-ladder-A", "hs-ladder-B"])
        self.assertEqual(lad["label"], "deku 3 + #177 ladder")
        self.assertEqual([r["scenario"] for r in runs["b"]], ["walk"])

    def test_group_emptied_by_merge_dropped(self):
        doc = self._split("[a]\nwalk  label=x\n[b]\nwalk  label=x\n")
        self.assertEqual([u["name"] for u in doc["lanes"][0]["groups"]], ["a"])
        self.assertEqual(len(doc["merged"]), 1)

    def test_different_runs_not_merged(self):
        doc = self._split("""[a]
hs-hud  label="HUD blocks"
hs-tunic  switches=X=1
hs-sign  logonly="log only"
chest  timeout=900
[b]
hs-hud  switches=LICHII_COOP_HUD=0  label="HUD off"
hs-tunic  switches=X=2
hs-sign  see=sign-A
chest
""")
        self.assertEqual(doc["merged"], [])
        self.assertEqual(sum(len(u["lines"]) for u in doc["lanes"][0]["groups"]), 8)


class Remaining(Base):
    def test_only_runs_without_folder(self):
        lane = os.path.join(self.tmp, "lane2.txt")
        write(lane, "[g3]\nwear\nturn\nwear\n[g4]\ntime\n")
        root = os.path.join(self.tmp, "runs")
        fake_run(root, "g3", "wear", 2)
        text = p.remaining(lane, root)
        runs = g.parse_groups(text)
        self.assertEqual([(n, [r["scenario"] for r in rs]) for n, rs in runs], [("g3", ["turn", "wear"]), ("g4", ["time"])])

    def test_nothing_left(self):
        lane = os.path.join(self.tmp, "lane2.txt")
        write(lane, "[g3]\nwear\n")
        root = os.path.join(self.tmp, "runs")
        fake_run(root, "g3", "wear", 2)
        self.assertIsNone(p.remaining(lane, root))
        out = os.path.join(self.tmp, "left.txt")
        self.assertEqual(p.main(["remaining", lane, root, "--out", out]), 0)
        self.assertFalse(os.path.exists(out))

    def test_older_runs_in_reused_root_not_done(self):
        # review-c124-2: a run root reused across rcs must not count the older rc's runs as done
        root = os.path.join(self.tmp, "runs")
        fake_run(root, "g3", "wear", 2)
        old = os.path.join(root, "g3", "wear", "20261126-010000", "run.json")
        t = os.path.getmtime(old) - 3600
        os.utime(old, (t, t))
        lane = os.path.join(self.tmp, "lane2.txt")
        write(lane, "[g3]\nwear\n")
        self.assertIn("wear", p.remaining(lane, root))
        fake_run(root, "g3", "wear", 2, stamp="20261126-020000")
        self.assertIsNone(p.remaining(lane, root))

    def test_no_run_root_all_left(self):
        lane = os.path.join(self.tmp, "lane2.txt")
        write(lane, "[g3]\nwear\n")
        self.assertIn("wear", p.remaining(lane, os.path.join(self.tmp, "none")))


class SmokeFile(unittest.TestCase):
    def test_rc_smoke_parses_and_has_the_crash_path(self):
        path = os.path.join(os.path.dirname(__file__), "..", "groups", "hookshot-rc-smoke.txt")
        with open(path, encoding="utf-8") as f:
            runs = [r for _, rs in g.parse_groups(f.read()) for r in rs]
        names = [r["scenario"] for r in runs]
        self.assertIn("hs-party-hud-probe", names)   # menu -> load, warp, back to the title, load again (C #126)
        for r in runs:
            self.assertTrue(os.path.isfile(os.path.join(os.path.dirname(__file__), "..", "scenarios",
                                                        r["scenario"] + ".A.txt")), r["scenario"])


if __name__ == "__main__":
    unittest.main()
