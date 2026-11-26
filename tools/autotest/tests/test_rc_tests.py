"""rc_tests.py: one rc pass of the in-game bridge tests over both lanes (plan + one summary)."""
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import proven as pv  # noqa: E402
import rc_tests as rt  # noqa: E402

MOD = "tools/autotest/ingame/test_rows.py"


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.repo = os.path.join(self.tmp, "repo")
        write(os.path.join(self.repo, *MOD.split("/")), "def test_a(): pass\n")
        write(os.path.join(self.repo, "tools", "autotest", "ingame", "test_smoke.py"), "def test_boot(): pass\n")
        self.n = 0

    def summary(self, root, rows, key="k1", **extra):
        """A lane's summary.json as bt_plugin writes it (identity + per-row test_hash)."""
        self.n += 1
        d = os.path.join(self.tmp, root, "run%d" % self.n)
        doc = dict({"identity": {"key": key} if key else {}, "tests": rows, "verdict": "x", "minutes": 1}, **extra)
        write(os.path.join(d, "summary.json"), json.dumps(doc))
        return d

    def row(self, test, result="PASS", seconds=60, hashed=True):
        r = {"test": test, "result": result, "seconds": seconds}
        if hashed:
            r["test_hash"] = pv.test_hash(pv.base_nodeid(test), self.repo)
        return r


class Ids(Base):
    def test_collect_listing_gives_base_ids_once(self):
        p = os.path.join(self.tmp, "c.txt")
        write(p, MOD + "::test_a[r1]\n" + MOD + "::test_a[r2]\n" + MOD + "::test_b\n\n3 tests collected in 0.1s\n")
        self.assertEqual(rt.read_ids(p), [MOD + "::test_a", MOD + "::test_b"])

    def test_test_hash_follows_module_harness_and_refs(self):
        t = MOD + "::test_a"
        base = pv.test_hash(t, self.repo)
        self.assertTrue(base)
        self.assertEqual(pv.test_hash(MOD + "::test_other", self.repo), base)   # same file: same hash
        self.assertEqual(pv.test_hash("test_rows.py::test_a", self.repo), base)   # as pytest prints it (rootdir ingame/)
        self.assertEqual(pv.test_hash("tools/autotest/ingame/gone.py::t", self.repo), "")
        for rel in (MOD, "tools/autotest/ingame/pair.py", "tools/autotest/ingame/refs/x-A.png"):
            write(os.path.join(self.repo, *rel.split("/")), "changed %s\n" % rel)
            now = pv.test_hash(t, self.repo)
            self.assertNotEqual(base, now, rel)
            base = now


class Plan(Base):
    A, B, C, S = MOD + "::test_a", MOD + "::test_b", MOD + "::test_c", "tools/autotest/ingame/test_smoke.py::test_boot"

    def test_smoke_on_every_lane_rest_balanced_by_measured_seconds(self):
        h = self.summary("hist", [self.row(self.A, seconds=600), self.row(self.B, seconds=300),
                                  self.row(self.C, seconds=290), self.row(self.S, seconds=30),
                                  self.row(self.C, "SETUP-FAIL", seconds=2)])
        doc = rt.plan([self.S, self.A, self.B, self.C], [self.S], [1, 2], [os.path.dirname(h)], repo=self.repo)
        self.assertEqual(doc["lanes"], {"1": [self.S, self.A], "2": [self.S, self.B, self.C]})
        self.assertEqual(doc["lane_seconds"], {"1": 630.0, "2": 620.0})
        self.assertEqual(doc["estimates"][self.C]["seconds"], 290.0)   # the SETUP-FAIL row is not a timing

    def test_no_history_median_then_default(self):
        doc = rt.plan([self.A, self.B], [], [1, 2], [], repo=self.repo)
        self.assertEqual(doc["estimates"][self.A]["seconds"], rt.DEFAULT_S)
        h = self.summary("hist", [self.row(self.A, seconds=50)])
        doc = rt.plan([self.A, self.B], [], [1, 2], [os.path.dirname(h)], repo=self.repo)
        self.assertIn("median", doc["estimates"][self.B]["from"])

    def test_pass_on_every_key_is_skipped_with_its_proof(self):
        d1 = self.summary("runs", [self.row(self.A), self.row(self.S)], key="k1")
        d2 = self.summary("runs", [self.row(self.A + "[r2]")], key="k2")
        doc = rt.plan([self.S, self.A, self.B], [self.S], [1, 2], [], [os.path.join(self.tmp, "runs")],
                      ["k1", "k2"], repo=self.repo)
        self.assertEqual([s["test"] for s in doc["skipped"]], [self.A])
        self.assertEqual({x["folder"] for x in doc["skipped"][0]["proof"]}, {d1, d2})
        self.assertNotIn(self.A, sum(doc["lanes"].values(), []))
        self.assertIn(self.S, doc["lanes"]["1"])   # smoke is never skipped

    def test_flaky_fail_setup_fail_noref_other_key_old_hash_never_prove(self):
        runs = os.path.join(self.tmp, "runs")
        for bad in ("FLAKY", "FAIL", "SETUP-FAIL", "NO-REF"):
            shutil.rmtree(runs, True)
            self.summary("runs", [self.row(self.A), self.row(self.A, bad)])
            doc = rt.plan([self.A], [], [1], [], [runs], ["k1"], repo=self.repo)
            self.assertEqual(doc["skipped"], [], bad)
        shutil.rmtree(runs, True)
        self.summary("runs", [self.row(self.A)], key="other")
        self.summary("runs", [self.row(self.A)], key=None)
        self.summary("runs", [self.row(self.A, hashed=False)])
        self.assertEqual(rt.plan([self.A], [], [1], [], [runs], ["k1"], repo=self.repo)["skipped"], [])
        self.assertEqual(rt.plan([self.A], [], [1], [], [runs], ["k1", "k2"], repo=self.repo)["skipped"], [])

    def test_changed_test_module_unproves(self):
        runs = os.path.join(self.tmp, "runs")
        self.summary("runs", [self.row(self.A)])
        write(os.path.join(self.repo, *MOD.split("/")), "def test_a(): assert 1\n")
        self.assertEqual(rt.plan([self.A], [], [1], [], [runs], ["k1"], repo=self.repo)["skipped"], [])

    def test_skip_proven_needs_keys(self):
        with self.assertRaises(rt.Fail):
            rt.plan([self.A], [], [1], [], [self.tmp], [], repo=self.repo)


class Summary(Base):
    def test_one_summary_from_both_lanes_and_the_plan(self):
        l1 = self.summary("o", [{"test": "t::a", "result": "PASS"}], key="k1")
        l2 = self.summary("o", [{"test": "t::b", "result": "PASS"}, {"test": "t::s", "result": "SKIP"}], key="k1")
        plan = {"skipped": [{"test": "t::s", "proof": [{"result": "PASS", "folder": "F"}]},
                            {"test": "t::x", "proof": [{"result": "PASS", "folder": "G"}]}]}
        doc = rt.merge([l1, l2], plan)
        self.assertEqual(doc["verdict"], "PASS")
        self.assertEqual(doc["counts"]["SKIP"], 2)   # t::s once (the lane wrote it), t::x from the plan
        self.assertIn("G", [r for r in doc["tests"] if r["test"] == "t::x"][0]["why"])
        self.assertIn("| SKIP | t::x |", rt.summary_md(doc))

    def test_any_not_pass_or_a_missing_lane_is_not_pass(self):
        l1 = self.summary("o", [{"test": "t::a", "result": "PASS"}])
        for bad in ("FAIL", "FLAKY", "SETUP-FAIL", "NO-REF", "WEIRD"):
            l2 = self.summary("o", [{"test": "t::b", "result": bad}])
            self.assertEqual(rt.merge([l1, l2])["verdict"], "NOT PASS", bad)
        doc = rt.merge([l1, os.path.join(self.tmp, "nolane")])
        self.assertEqual((doc["verdict"], doc["counts"]["SETUP-FAIL"]), ("NOT PASS", 1))
        self.assertEqual(rt.merge([])["verdict"], "NOT PASS")

    def test_cli_plan_and_summary(self):
        c = os.path.join(self.tmp, "c.txt")
        write(c, Plan.A + "\n" + Plan.B + "\n")
        out = os.path.join(self.tmp, "plan")
        self.assertEqual(rt.main(["plan", "--collect", c, "--out-dir", out, "--lanes", "1,2"]), 0)
        with open(os.path.join(out, "plan.json"), encoding="utf-8") as f:
            self.assertEqual(sorted(sum(json.load(f)["lanes"].values(), [])), [Plan.A, Plan.B])
        l1 = self.summary("o", [{"test": Plan.A, "result": "PASS"}])
        self.assertEqual(rt.main(["summary", l1, "--plan", os.path.join(out, "plan.json"),
                                  "--out", os.path.join(self.tmp, "rc")]), 0)
        self.assertTrue(os.path.isfile(os.path.join(self.tmp, "rc", "summary.md")))


if __name__ == "__main__":
    unittest.main()
