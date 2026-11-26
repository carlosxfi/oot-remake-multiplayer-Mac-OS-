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


import proven as pv  # noqa: E402
import types  # noqa: E402


def pair(root, side, dll=b"module-1", built="release=3.1.1\ncommit=abc\npatches=11,22", mods=None):
    d = os.path.join(root, "pair-" + side)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "hookshot_coop.dll"), "wb") as f:
        f.write(dll)
    write(os.path.join(d, "coop-built-from.txt"), built + "\n")
    for rel, text in (mods or {"Squadala/romfs/a.bin": "pack"}).items():
        write(os.path.join(d, "mods", rel), text)
    return d


class SkipProven(unittest.TestCase):
    """step (f), B #128 2026-11-26: a row is skipped only on a PASS of the same row on the identical build."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.a, self.b = pair(self.tmp, "A"), pair(self.tmp, "B")
        self.key = pv.identity_of(self.a, self.b)["key"]
        self.runs = os.path.join(self.tmp, "runs")
        self.results = {}
        self.n = 0

    def proof(self, group, line, result, ident=None):
        """A run folder of `line` judged `result`, as groups.py writes its run.json."""
        r = p.one_run(line)
        self.n += 1
        d = os.path.join(self.runs, group, r["scenario"], "20261126-%06d" % self.n)
        meta = {"group": group, "scenario": r["scenario"], "label": r["label"], "expects": r["expects"],
                "never": r["never"], "see": r["see"], "logonly": r["logonly"], "switches": r["switches"],
                "timeout": r["timeout"], "password_hash": pv.secret_hash(r["password"]),
                "identity": ident if ident is not None else {"key": self.key}, "exit": 0}
        write(os.path.join(d, "run.json"), json.dumps(meta))
        self.results[d] = result
        return d

    def judge(self, folder, meta):
        return self.results[folder]

    def split(self, text, keys=None):
        src = os.path.join(self.tmp, "rc.txt")
        write(src, text)
        return p.split([src], os.path.join(self.tmp, "out"), 2, [], [self.runs], keys or [self.key], self.judge)

    def played(self, doc):
        return [x for lane in doc["lanes"] for u in lane["groups"] for x in u["lines"]]

    def test_pass_on_same_build_is_skipped_and_listed(self):
        d = self.proof("g1", 'walk expect="A:coop: connected" logonly="smoke"', "PASS")
        doc = self.split('[g1]\nwalk expect="A:coop: connected" logonly="smoke"\nchest\n')
        self.assertEqual(self.played(doc), ["chest"])
        self.assertEqual(doc["skipped"][0]["proof"], [{"folder": d, "result": "PASS"}])
        with open(os.path.join(self.tmp, "out", "plan.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f)["skipped"][0]["scenario"], "walk")

    def test_flaky_fail_setup_fail_unproven_played_never_prove(self):
        for i, other in enumerate(("FAIL", "UNPROVEN", "PLAYED", "FAIL")):   # FAIL = SETUP-FAIL in judge too
            self.proof("g1", "walk", "PASS")
            self.proof("g1", "walk", other)   # a PASS and another result on one build = FLAKY
            self.assertEqual(self.played(self.split("[g1]\nwalk\n")), ["walk"], other)
            self.results = {k: "FAIL" for k in self.results}
        self.assertEqual(self.split("[g1]\nwalk\n")["skipped"], [])

    def test_other_build_or_no_identity_proves_nothing(self):
        self.proof("g1", "walk", "PASS", ident={"key": "000000000000000000000000"})
        self.proof("g1", "walk", "PASS", ident={"error": "no hookshot_coop.dll"})
        self.assertEqual(self.played(self.split("[g1]\nwalk\n")), ["walk"])

    def test_other_row_proves_nothing_but_more_checks_do(self):
        self.proof("g1", "walk switches=LICHII_COOP_CHEATS=1", "PASS")
        self.proof("g1", "walk timeout=900", "PASS")
        self.proof("g1", 'walk logonly="x"', "PASS")
        self.assertEqual(self.played(self.split('[g1]\nwalk expect="A:x"\n')), ['walk expect="A:x"'])
        self.proof("g9", 'walk expect="A:x" expect="B:y"', "PASS")   # another group, more checks: still this row
        self.assertEqual(self.played(self.split('[g1]\nwalk expect="A:x"\nchest\n')), ["chest"])

    def test_every_lane_key_must_hold_a_pass(self):
        self.proof("g1", "walk", "PASS")
        doc = self.split("[g1]\nwalk\nchest\n", keys=[self.key, "111111111111111111111111"])
        self.assertEqual(self.played(doc), ["walk", "chest"])

    def test_all_proven_is_a_clear_error(self):
        self.proof("g1", "walk", "PASS")
        with self.assertRaises(p.Fail):
            self.split("[g1]\nwalk\n")

    def test_no_identity_key_is_refused(self):
        src = os.path.join(self.tmp, "rc.txt")
        write(src, "[g1]\nwalk\n")
        with self.assertRaises(p.Fail):
            p.split([src], os.path.join(self.tmp, "out"), 2, [], [self.runs], [], self.judge)


class Identity(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def key(self, sub, **kw):
        root = os.path.join(self.tmp, sub)
        return pv.identity_of(pair(root, "A", **kw), pair(root, "B", **kw))["key"]

    def test_same_content_other_folder_same_key(self):
        self.assertEqual(self.key("lane1"), self.key("lane2"))

    def test_module_game_and_pack_each_change_the_key(self):
        base = self.key("x0")
        self.assertNotEqual(base, self.key("x1", dll=b"module-2"))
        self.assertNotEqual(base, self.key("x2", built="release=3.1.1\ncommit=abc\npatches=11,23"))
        self.assertNotEqual(base, self.key("x3", mods={"Squadala/romfs/a.bin": "pack2"}))

    def test_files_a_game_start_writes_do_not_change_the_key(self):
        base = self.key("y0", mods={"Squadala/romfs/a.bin": "pack1"})
        out = {"Squadala/romfs/a.bin": "pack1", "coop-mods.applied": "coop-mods-applied 1 5792\n",
               "coop-mods.starting": "x", "oot-remake-randomizer/last-build.txt": "seed"}
        self.assertEqual(base, self.key("y1", mods=out))
        self.assertNotEqual(base, self.key("y2", mods=dict(out, **{"coop-mods.settings.json": "{}"})))

    def test_a_pair_without_module_or_stamp_fails(self):
        a = pair(self.tmp, "A")
        b = os.path.join(self.tmp, "empty")
        os.makedirs(b)
        with self.assertRaises(pv.Fail):
            pv.identity_of(a, b)

    def test_groups_writes_identity_into_run_json(self):
        a, b = pair(self.tmp, "A"), pair(self.tmp, "B")
        setup = {"name": "g1", "index": 1, "a": a, "b": b, "port": 27560, "pipe": "p", "shots": self.tmp,
                 "kits": [], "server": None}
        run = p.one_run('walk password=hunter2 timeout=60')
        folder = os.path.join(self.tmp, "run1")
        os.makedirs(folder)
        res = g.run_groups([(setup, [run], [["x"]])], say=lambda *_: None,
                           runner=lambda cmd: types.SimpleNamespace(returncode=0, stdout=folder + "\n", stderr=""))
        self.assertEqual(res[0][2], folder)
        with open(os.path.join(folder, "run.json"), encoding="utf-8") as f:
            meta = json.load(f)
        self.assertEqual(meta["identity"]["key"], pv.identity_of(a, b)["key"])
        self.assertEqual((meta["timeout"], meta["password_hash"]), (60, pv.secret_hash("hunter2")))
        self.assertNotIn("hunter2", json.dumps(meta))
        self.assertTrue(pv.same_row(run, meta))


if __name__ == "__main__":
    unittest.main()
