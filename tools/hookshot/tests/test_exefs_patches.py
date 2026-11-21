"""Tests for hookshot/patches 0034 and 0035: Longshot's nx2/exefs_patches.py as the two patches make it (0034 adds the
file, 0035 changes it), written into a temporary folder from the patch files with `git apply` and imported. Made-up
data only: no Longshot source and no game needed.

    python tools/hookshot/tests/test_exefs_patches.py
"""
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
PATCHES = os.path.join(REPO, "hookshot", "patches")
FILE = "nx2/exefs_patches.py"
MAIN = 0x80020000   # main's link address, as snaildev99's crash lines have it


def load():
    """The module from patches 0034 + 0035 (their hunks for FILE only), or a SkipTest without git."""
    git = shutil.which("git")
    if git is None:
        raise unittest.SkipTest("needs git")
    work = tempfile.mkdtemp(prefix="exefs_patches_")
    subprocess.run([git, "init", "-q"], cwd=work, check=True)
    for name in sorted(os.listdir(PATCHES)):
        if name.startswith(("0034-", "0035-")):
            subprocess.run([git, "apply", "--include=" + FILE, os.path.join(PATCHES, name)], cwd=work, check=True)
    spec = importlib.util.spec_from_file_location("exefs_patches_under_test", os.path.join(work, *FILE.split("/")))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod, work


class ExefsPatchesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m, cls.work = load()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def test_the_functions_a_mod_patch_writes_into_keep_the_context(self):
        # The case of the crash: the mod's hook branches from the game's f_80c35c30 into the cave (lifted as that
        # function's code), and its helper f_83c662f0 is a function of its own in the cave; a game function the patch
        # does not touch keeps its locals.
        hook_site, helper, untouched = 0x80c35c30, 0x83c662f0, 0x81000000
        funcs = {
            hook_site: ({hook_site: 0, 0x80c35c64: 0, 0x83c66360: 0, 0x83c6637c: 0}, set()),
            helper: ({helper + 4 * i: 0 for i in range(27)}, set()),
            untouched: ({untouched + 4 * i: 0 for i in range(10)}, set()),
        }
        words = {0x80c35c64 - MAIN, 0x83c66360 - MAIN, helper + 8 - MAIN}
        self.assertEqual(self.m.functions_written(funcs, MAIN, words), {hook_site, helper})
        self.assertEqual(self.m.functions_written(funcs, MAIN, set()), set())       # no patches: as without them
        self.assertEqual(self.m.functions_written(funcs, MAIN, {0x2000000}), set())  # data only: no function

    def test_the_written_words_are_the_code_and_data_files_bytes_by_word(self):
        path = os.path.join(self.work, "patches.json")
        files = [{"mod": "A", "file": "x.ips", "kind": "code", "writes": [[0x100, 1], [0x101, 2], [0x107, 3]]},
                 {"mod": "B", "file": "y.ips", "kind": "data", "writes": [[0x2001, 9]]},
                 {"mod": "C", "file": "z.ips", "kind": "off", "writes": [[0x3000, 1]]},
                 {"mod": "D", "file": "w.ips", "kind": "other", "writes": [[0x4000, 1]]}]
        with open(path, "w", encoding="utf-8") as f:
            json.dump(files, f)
        self.assertEqual(self.m.written_words(path), {0x100, 0x104, 0x2000})
        old = os.environ.pop(self.m.ENV, None)
        try:
            self.assertEqual(self.m.written_words(), set())   # no patches' list: none
        finally:
            if old is not None:
                os.environ[self.m.ENV] = old

    def test_the_co_op_mods_tab_list_sets_the_switches_and_the_order(self):
        root = os.path.join(self.work, "mods")
        os.makedirs(root, exist_ok=True)
        self.assertIsNone(self.m.coop_list(root))   # no list: mods.json's rules, as Longshot
        with open(os.path.join(root, self.m.COOP_LIST), "w", encoding="utf-8") as f:
            f.write("coop-mods 1\non Second\noff Off Mod\non first\non FIRST\n")
        rows = self.m.coop_list(root)
        self.assertEqual(rows, [("Second", True), ("Off Mod", False), ("first", True)])   # (a name twice: its first)
        mods = [("zeta", "/z"), ("first", "/f"), ("Off Mod", "/o"), ("alpha", "/a"), ("Second", "/s")]
        got = self.m.coop_ordered(mods, rows)
        # host/mods.c's order: the mods the list does not name first, by name; then the list from its last row up, so
        # the first row is indexed last and its patch wins a byte two patches write. An off mod stays off.
        self.assertEqual([(n, on) for n, _, on in got],
                         [("alpha", True), ("Off Mod", False), ("zeta", True), ("first", True), ("Second", True)])
        with open(os.path.join(root, self.m.COOP_LIST), "w", encoding="utf-8") as f:
            f.write("coop-mods 2\non Second\n")
        self.assertIsNone(self.m.coop_list(root))   # another version: not ours


if __name__ == "__main__":
    unittest.main()
