"""Selftests of ingame/test_dbno_crawl.py (row 44 / #337, crawl-337-9's file) against the fake games
(ingame/fakes/dbno_crawl.py): the row PASSes only when the downed Link lies and crawls slower than a walk, is revived
by A's held A and walks again; each known way of failing (BT_FAKE_DBNO_BUG) FAILs it; a FAIL leaves the lane as the
next rows expect it (no SETUP-FAIL after it). Run by tests/test_ingame.py's load_tests in CI."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INGAME = os.path.join(os.path.dirname(HERE), "ingame")
CHILD = "test_downed_child_crawls_and_is_revived"


def run(out, k, refs=None, bug=None):
    env = dict(os.environ)
    env.pop("BT_FAKE_DBNO_BUG", None)
    if bug:
        env["BT_FAKE_DBNO_BUG"] = bug
    if refs:
        env["BT_FAKE_REFS"] = refs
    p = subprocess.run([sys.executable, "-m", "pytest", ".", "--fake", "--out", out, "-q", "-p", "no:cacheprovider",
                        "--no-smoke-gate", "-k", k], cwd=INGAME, capture_output=True, text=True, timeout=600, env=env)
    with open(os.path.join(out, "summary.json"), encoding="utf-8") as f:
        s = json.load(f)
    return p, {r["test"].split("::")[-1]: r for r in s["tests"]}


class DbnoCrawl(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        """References from a first (NO-REF) run, approved here wholesale, so the runs below judge state, not looks."""
        cls.tmp = tempfile.mkdtemp()
        cls.refs = os.path.join(cls.tmp, "refs")
        os.makedirs(cls.refs)
        out = os.path.join(cls.tmp, "first")
        p, res = run(out, "dbno", refs=cls.refs)
        cls.first = res
        new = os.path.join(out, "lane1", "new")
        for f in os.listdir(new):
            shutil.copyfile(os.path.join(new, f), os.path.join(cls.refs, f))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def out(self, name):
        return os.path.join(self.tmp, name)

    def test_first_run_is_no_ref_not_fail(self):
        self.assertEqual({t: r["result"] for t, r in self.first.items()},
                         {CHILD: "NO-REF", "test_downed_charsel_puppet_lies": "NO-REF",
                          "test_downed_adult_lies_bleeds_out_respawns": "NO-REF"})
        for r in self.first.values():
            self.assertEqual({sh["game"] for sh in r["shots"]}, {"A", "B"})
            self.assertLessEqual(len(r["shots"]), 8)

    def test_passes_with_refs(self):
        p, res = run(self.out("pass"), "dbno", refs=self.refs)
        self.assertEqual({r["result"] for r in res.values()}, {"PASS"}, p.stdout[-2000:])

    def test_each_bug_fails_the_child_row(self):
        for bug in ("stand", "norevive", "stuck"):
            with self.subTest(bug=bug):
                p, res = run(self.out("bug-" + bug), CHILD, refs=self.refs, bug=bug)
                self.assertEqual(res[CHILD]["result"], "FAIL", p.stdout[-2000:])

    def test_a_fail_leaves_the_lane_for_the_next_rows(self):
        """After the child row FAILs mid-down, the rows after it still set up (the restore: /dbno off, 45 s, child,
        the Kakariko boot's spot)."""
        p, res = run(self.out("after"), "%s or test_puppets_spawn or test_teleport_to_player" % CHILD, refs=self.refs,
                     bug="norevive")
        self.assertEqual(res[CHILD]["result"], "FAIL", p.stdout[-2000:])
        self.assertEqual(res["test_puppets_spawn"]["result"], "PASS", p.stdout[-2000:])
        self.assertEqual(res["test_teleport_to_player"]["result"], "PASS", p.stdout[-2000:])


if __name__ == "__main__":
    unittest.main()
