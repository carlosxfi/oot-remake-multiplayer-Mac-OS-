"""Tests for tools/hookshot/hs.py's shared game files: romfs hard-linked by copy_tree (one copy of the ~26 GB for every
pin, cache entry and test pair), and a new pin's game files taken from an older pin (Longshot's --romfs-from).
Made-up folders in a temp folder; no build, no game.
    python tools/hookshot/tests/test_hs_share.py -v"""
import io
import os
import shutil
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import hs  # noqa: E402

COMMIT = "c544828ff2d545829505cd894bb7968c7c2c5fcb"


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.cache = os.path.join(self.tmp, "cache")
        os.environ["OOT_COOP_HS_CACHE"] = self.cache
        hs.LINKS_REFUSED.clear()   # (one run's memory: each test is a run of its own)
        self.out = io.StringIO()
        self.say = lambda *a: self.out.write(" ".join(str(x) for x in a) + "\n")

    def tearDown(self):
        os.environ.pop("OOT_COOP_HS_CACHE", None)
        shutil.rmtree(self.tmp, ignore_errors=True)


class ShareTest(Base):
    """copy_tree: the top-level romfs is hard-linked file by file; everything else is still copied."""

    def game(self):
        g = os.path.join(self.tmp, "src")
        write(os.path.join(g, "longshot.exe"), "exe")
        write(os.path.join(g, "romfs", "Scene", "a.zs"), "scene a")
        write(os.path.join(g, "romfs", "b.bin"), "b")
        write(os.path.join(g, "data", "romfs", "c.txt"), "not the game files: a deeper romfs is copied")
        return g

    def test_romfs_is_linked_the_rest_copied(self):
        src, dst = self.game(), os.path.join(self.tmp, "dst")
        hs.copy_tree(src, dst, say=self.say)
        for rel in (("romfs", "Scene", "a.zs"), ("romfs", "b.bin")):
            self.assertTrue(os.path.samefile(os.path.join(src, *rel), os.path.join(dst, *rel)), rel)
            self.assertGreaterEqual(os.stat(os.path.join(dst, *rel)).st_nlink, 2)
        for rel in (("longshot.exe",), ("data", "romfs", "c.txt")):
            self.assertFalse(os.path.samefile(os.path.join(src, *rel), os.path.join(dst, *rel)), rel)
            self.assertEqual(read(os.path.join(dst, *rel)), read(os.path.join(src, *rel)))

    def test_again_changes_nothing_and_an_old_full_copy_becomes_a_link(self):
        src, dst = self.game(), os.path.join(self.tmp, "dst")
        hs.copy_tree(src, dst, say=self.say)
        self.assertEqual(hs.copy_tree(src, dst, say=self.say), (0, 0))
        # (a folder made before: its own copy of a game file, the same size and time) is linked now, freeing its space
        old = os.path.join(dst, "romfs", "b.bin")
        os.unlink(old)
        shutil.copy2(os.path.join(src, "romfs", "b.bin"), old)
        self.assertFalse(os.path.samefile(old, os.path.join(src, "romfs", "b.bin")))
        copied, _ = hs.copy_tree(src, dst, say=self.say)
        self.assertEqual(copied, 1)
        self.assertTrue(os.path.samefile(old, os.path.join(src, "romfs", "b.bin")))

    def test_a_changed_game_file_replaces_the_link_and_never_writes_through_it(self):
        src, dst = self.game(), os.path.join(self.tmp, "dst")
        other = os.path.join(self.tmp, "other")
        hs.copy_tree(src, dst, say=self.say)
        hs.copy_tree(dst, other, say=self.say)   # (a third folder sharing the same file)
        new = os.path.join(self.tmp, "new.zs")
        write(new, "scene a, changed")
        os.unlink(os.path.join(src, "romfs", "Scene", "a.zs"))
        shutil.copy2(new, os.path.join(src, "romfs", "Scene", "a.zs"))
        hs.copy_tree(src, dst, say=self.say)
        self.assertEqual(read(os.path.join(dst, "romfs", "Scene", "a.zs")), "scene a, changed")
        self.assertEqual(read(os.path.join(other, "romfs", "Scene", "a.zs")), "scene a")   # the other folder kept its own

    def test_where_links_are_refused_the_copies_are_not_made_again(self):
        # (a cache on another drive, a FAT disk: every link refused) the first run copies, the second copies nothing
        src, dst = self.game(), os.path.join(self.tmp, "dst")
        real = os.link
        try:
            def refuse(*_a, **_k):
                raise OSError("no hard links here")
            os.link = refuse
            first, _ = hs.copy_tree(src, dst, say=self.say)
            hs.LINKS_REFUSED.clear()   # (a new run: it finds out again, with one file at most)
            second, _ = hs.copy_tree(src, dst, say=self.say)
            third, _ = hs.copy_tree(src, dst, say=self.say)
        finally:
            os.link = real
        self.assertEqual(first, 4)   # (the exe, two game files, data/romfs/c.txt)
        self.assertLessEqual(second, 1)
        self.assertEqual(third, 0)
        self.assertEqual(read(os.path.join(dst, "romfs", "Scene", "a.zs")), "scene a")

    def test_can_link(self):
        class St:
            def __init__(self, dev, nlink=1):
                self.st_dev, self.st_nlink = dev, nlink
        self.assertTrue(hs.can_link(St(1), St(1)))
        self.assertFalse(hs.can_link(St(1), St(2)))   # (another drive: a copy stays a copy)
        self.assertFalse(hs.can_link(St(1, hs.NTFS_MAX_LINKS), St(1)))   # (NTFS: no more names for that file)
        hs.LINKS_REFUSED.add((1, 1))
        self.assertFalse(hs.can_link(St(1), St(1)))   # (refused once in this run)

    def test_a_link_that_cannot_be_made_is_a_copy(self):
        src = os.path.join(self.tmp, "s.bin")
        write(src, "x")
        d = os.path.join(self.tmp, "d.bin")
        real = os.link
        try:
            def refuse(*_a, **_k):
                raise OSError("another drive")
            os.link = refuse
            self.assertFalse(hs.link_or_copy(src, d))
        finally:
            os.link = real
        self.assertEqual(read(d), "x")
        self.assertFalse(os.path.samefile(src, d))
        self.assertTrue(hs.link_or_copy(src, d))   # (an old d is unlinked first, then linked)
        self.assertTrue(os.path.samefile(src, d))


class RomfsFromTest(Base):
    """A new pin's game folder takes the game files of the newest older pin that unpacked its own."""

    def pin(self, name, own=True, age=0):
        g = os.path.join(self.cache, "pins", name, "game")
        os.makedirs(g, exist_ok=True)
        if own:
            write(os.path.join(g, "romfs", "a.zs"), "a")
            write(os.path.join(g, ".longshot_romfs"), "game.nup 123\n")
            t = time.time() - age
            os.utime(os.path.join(g, ".longshot_romfs"), (t, t))
        return g

    def test_the_newest_other_pin_with_its_own_game_files(self):
        old = self.pin("v3.0.0-rc.4-53f96d780588", age=200)
        newer = self.pin("v3.0.0-rc.5-e4d035e9e71a", age=100)
        self.pin("v3.0.0-rc.3-aaaaaaaaaaaa", own=False)   # (no stamp: not a source)
        out = os.path.join(self.cache, "pins", "v3.0.0-rc.6-181571bdaa66", "game")
        self.assertEqual(hs.romfs_source(out), newer)
        self.assertNotEqual(hs.romfs_source(out), old)

    def test_none_when_the_pin_has_its_own_or_there_is_no_other(self):
        out = os.path.join(self.cache, "pins", "v3.0.0-rc.6-181571bdaa66", "game")
        self.assertIsNone(hs.romfs_source(out))   # (no pins at all)
        self.pin("v3.0.0-rc.5-e4d035e9e71a")
        mine = self.pin("v3.0.0-rc.6-181571bdaa66")
        self.assertIsNone(hs.romfs_source(mine))   # (its own unpack comes first, as in Longshot)

    def kit(self):
        k = os.path.join(self.tmp, "kit")
        write(os.path.join(k, "scripts", "hookshot-build.ps1"),
              "param([string]$Game = '', [string]$Out = '', [int]$Jobs = 0, [string]$Tools = '', [string]$Work = '',\n"
              "      [string]$RomfsFrom = '')\n")
        return k

    def repo(self):
        r = os.path.join(self.tmp, "repo")
        write(os.path.join(r, "hookshot", "PIN"), "repo=https://example.invalid/Longshot\ncommit=%s\n" % COMMIT)
        write(os.path.join(r, "tools", "release", "kit", "hookshot", "TOOLS.txt"),
              "# comment=ignored\nrelease=v3.0.0-rc.6\nsetup_sha256=21a0\nsource_commit=%s\n" % COMMIT)
        write(os.path.join(r, "hookshot", "patches", "0001-a.patch"), "patch one\n")
        return r

    def build_line(self, game):
        hs.build([os.path.join(self.tmp, "A1")], os.path.join(self.tmp, "b"), kit=self.kit(), game=game,
                 repo=self.repo(), dry=True, say=self.say)
        return [ln for ln in self.out.getvalue().splitlines() if ln.startswith("$ ") and "hookshot-build.ps1" in ln][0]

    def test_a_miss_passes_the_older_pin_for_a_nup(self):
        self.assertEqual(hs.build_script_args(os.path.join(self.kit(), "scripts", "hookshot-build.ps1")),
                         {"Tools", "Work", "RomfsFrom"})
        older = self.pin("v3.0.0-rc.5-e4d035e9e71a")
        line = self.build_line("game.nup")
        self.assertIn("-RomfsFrom", line)
        self.assertIn(older if " " not in older else '"%s"' % older, line)
        self.assertIn("the new pin's game files come from", self.out.getvalue())

    def test_not_for_romfs_and_exefs_folders(self):
        self.pin("v3.0.0-rc.5-e4d035e9e71a")
        self.assertNotIn("-RomfsFrom", self.build_line(os.path.join(self.tmp, "romfs")))


class RestoreSharedTest(Base):
    """cache_restore: a pair whose romfs already is the cache's shared copy (its mark + file ids) is not walked again;
    any doubt (no mark, another list, a missing or replaced file, a walk that did not end) = the full restore."""
    STAMP = "release=v3.2.0\ncommit=%s\npatches=abc" % COMMIT

    def setUp(self):
        super().setUp()
        self.entry = os.path.join(self.cache, hs.key_of(self.STAMP))
        g = os.path.join(self.entry, hs.CACHE_GAME)
        write(os.path.join(g, "longshot.exe"), "exe")
        write(os.path.join(g, "coop-built-from.txt"), self.STAMP + "\n")
        write(os.path.join(g, "romfs", "top.bin"), "top")
        for i in range(30):
            write(os.path.join(g, "romfs", "Scene", "s%02d.zs" % i), "scene %d" % i)
        write(os.path.join(self.entry, "stamp.txt"), self.STAMP + "\n")
        self.pair = os.path.join(self.tmp, "lane1-A")

    def restore(self):
        self.out.truncate(0)
        self.out.seek(0)
        self.assertTrue(hs.cache_restore(self.pair, want=self.STAMP, say=self.say))
        return self.out.getvalue()

    def same(self, *rel):
        return os.path.samefile(os.path.join(self.entry, hs.CACHE_GAME, "romfs", *rel), os.path.join(self.pair, "romfs", *rel))

    def test_first_restore_walks_and_marks_the_second_does_not_walk(self):
        log = self.restore()
        self.assertIn("full romfs walk (no %s" % hs.ROMFS_MARK, log)
        self.assertTrue(os.path.isfile(os.path.join(self.pair, hs.ROMFS_MARK)))
        digest, rows = hs.romfs_manifest(self.entry)
        self.assertEqual(len(rows), 31)
        self.assertIn("digest=%s" % digest, read(os.path.join(self.pair, hs.ROMFS_MARK)))
        self.assertTrue(self.same("Scene", "s07.zs"))
        walked = []
        real = hs.same_file

        def watch(a, b):
            walked.append(a)
            return real(a, b)
        hs.same_file = watch   # (copy_tree asks it for every romfs file it walks)
        try:
            log = self.restore()
        finally:
            hs.same_file = real
        self.assertIn("romfs already the cache's shared copy", log)
        self.assertIn("not walked", log)
        self.assertEqual([w for w in walked if os.sep + "romfs" + os.sep in w], [])
        os.remove(os.path.join(self.pair, hs.ROMFS_MARK))   # (without the mark the same pair is walked: the probe works)
        hs.same_file = watch
        try:
            self.assertIn("full romfs walk (no %s" % hs.ROMFS_MARK, self.restore())
        finally:
            hs.same_file = real
        self.assertEqual(len([w for w in walked if os.sep + "romfs" + os.sep in w]), 31)

    def test_a_missing_top_level_file_means_the_full_restore(self):
        self.restore()
        os.unlink(os.path.join(self.pair, "romfs", "top.bin"))
        log = self.restore()
        self.assertIn("full romfs walk (the top-level romfs names differ)", log)
        self.assertTrue(self.same("top.bin"))

    def test_a_sampled_file_replaced_by_a_copy_means_the_full_restore(self):
        self.restore()
        hs.ROMFS_SAMPLE, old = 1000, hs.ROMFS_SAMPLE   # (every file is sampled in this small romfs)
        try:
            f = os.path.join(self.pair, "romfs", "Scene", "s13.zs")
            os.unlink(f)
            shutil.copy2(os.path.join(self.entry, hs.CACHE_GAME, "romfs", "Scene", "s13.zs"), f)
            log = self.restore()
        finally:
            hs.ROMFS_SAMPLE = old
        self.assertIn("Scene/s13.zs in %s is not the cache's file" % os.path.join(self.pair, "romfs"), log)
        self.assertTrue(self.same("Scene", "s13.zs"))   # (the full walk made it a link again)

    def test_a_missing_sampled_file_means_the_full_restore(self):
        self.restore()
        hs.ROMFS_SAMPLE, old = 1000, hs.ROMFS_SAMPLE
        try:
            os.unlink(os.path.join(self.pair, "romfs", "Scene", "s02.zs"))
            log = self.restore()
        finally:
            hs.ROMFS_SAMPLE = old
        self.assertIn("Scene/s02.zs missing in", log)
        self.assertTrue(self.same("Scene", "s02.zs"))

    def test_a_walk_that_does_not_end_leaves_no_mark(self):
        self.restore()
        os.unlink(os.path.join(self.pair, "romfs", "top.bin"))   # (forces a walk)
        real = hs.copy_tree

        def broken(*a, **k):
            raise OSError("stopped in the middle")
        hs.copy_tree = broken
        try:
            with self.assertRaises(OSError):
                hs.cache_restore(self.pair, want=self.STAMP, say=self.say)
        finally:
            hs.copy_tree = real
        self.assertFalse(os.path.exists(os.path.join(self.pair, hs.ROMFS_MARK)))
        self.assertIn("full romfs walk (no %s" % hs.ROMFS_MARK, self.restore())

    def test_a_mark_of_another_list_or_a_broken_list_means_the_full_restore(self):
        self.restore()
        write(os.path.join(self.pair, hs.ROMFS_MARK), "digest=%s\n" % ("0" * 64))
        self.assertIn("the pair's mark names another romfs list", self.restore())
        with open(os.path.join(self.entry, hs.ROMFS_MANIFEST), "a", encoding="utf-8") as f:
            f.write("Scene/extra.zs\t1\t1\n")   # (the list no longer matches its digest: not trusted)
        self.assertIn("the cache entry has no %s yet" % hs.ROMFS_MANIFEST, self.restore())
        self.assertIn("not walked", self.restore())   # (the walk made the list again)

    def test_progress_lines_while_walking(self):
        hs.PROGRESS_EVERY, old = 10, hs.PROGRESS_EVERY
        try:
            log = self.restore()
        finally:
            hs.PROGRESS_EVERY = old
        self.assertIn("%s: 10 files looked at..." % self.pair, log)
        self.assertIn("%s: 30 files looked at..." % self.pair, log)

    def test_the_mark_is_never_stored_into_the_cache(self):
        self.restore()
        self.assertIn(hs.ROMFS_MARK, hs.PER_PLAYER)
        other = os.path.join(self.tmp, "stored")
        hs.copy_tree(self.pair, other, say=self.say)
        self.assertFalse(os.path.exists(os.path.join(other, hs.ROMFS_MARK)))


if __name__ == "__main__":
    unittest.main()
