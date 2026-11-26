"""Tests for tools/autotest/lanes.py (two test lanes on one PC), groups.py --lane and hs.py's lane-safe copies and
locks. Fake game folders in a temp dir; no game is started."""
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import lanes  # noqa: E402
import groups  # noqa: E402
hs = lanes.hs


def write(path, text="x"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def nlink(path):
    return os.stat(path).st_nlink


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.kit = os.path.join(self.tmp, "coop-kit")
        self.out = io.StringIO()
        self.say = lambda *a: self.out.write(" ".join(str(x) for x in a) + "\n")
        self.idle = lambda folder: []

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def game(self, name):
        g = os.path.join(self.tmp, name)
        for f in ("longshot.exe", "SDL3.dll", "image.bin", "longshot_pipeline_states_shipped.bin",
                  "hookshot_coop.dll", "longshot_programs.bin", "mkw_vk_pipelines.bin", "longshot_pipelines.bin.built",
                  "coop-options.ini", "longshot_log.txt", "longshot_pipelines.bin.built.warming"):
            write(os.path.join(g, f), name + ":" + f)
        write(os.path.join(g, "romfs", "Pack", "a.pack"), "pack")
        write(os.path.join(g, "user", "save", "u", "slot_00", "game.sav"), "save")
        write(os.path.join(g, "autotest-save", "account.sav"), "base")
        return g


class LaneTable(Base):
    def test_two_lanes_share_nothing_and_avoid_the_owner_and_groups_ports(self):
        ls = [lanes.lane(n, self.kit) for n in lanes.LANES]
        lanes.check_distinct(ls)
        for L in ls:
            self.assertNotIn(lanes.OWNER_PORT, L["ports"])
            self.assertTrue(all(p < 27560 for p in L["ports"]))   # groups.py starts at 27560
        self.assertEqual(ls[0]["a"], os.path.join(self.kit, "ls", "lane1-A"))
        self.assertEqual((ls[0]["port"], ls[1]["port"]), (27520, 27530))
        self.assertNotEqual(ls[0]["pipe"], ls[1]["pipe"])

    def test_clash_is_refused(self):
        L = lanes.lane(1, self.kit)
        with self.assertRaises(lanes.Fail):
            lanes.check_distinct([L, dict(L, lane=2, a="x", b="y", shots="z", pipe="p")])   # same ports
        with self.assertRaises(lanes.Fail):
            lanes.check_distinct([dict(L, ports=[27510])])
        with self.assertRaises(lanes.Fail):
            lanes.lane(3, self.kit)

    def test_run_ps1_and_hookshot_py_take_the_lane(self):
        with open(os.path.join(HERE, "..", "run.ps1"), encoding="utf-8") as f:
            ps = f.read()
        self.assertIn("lanes.py\" info --lane $Lane --json", ps)
        self.assertIn("lock acquire --lane $Lane", ps)
        with open(os.path.join(HERE, "..", "hookshot.py"), encoding="utf-8") as f:
            self.assertIn('"--lane"', f.read())


class Fill(Base):
    def test_read_only_files_are_hard_links_everything_else_copies(self):
        src = self.game("A1")
        L = lanes.setup(1, src, self.game("B1"), say=self.say, kit=self.kit, busy=self.idle)
        a = L["a"]
        for f in ("longshot.exe", "SDL3.dll", "image.bin", "longshot_pipeline_states_shipped.bin",
                  os.path.join("romfs", "Pack", "a.pack")):
            self.assertTrue(os.path.samefile(os.path.join(src, f), os.path.join(a, f)), f)
        for f in ("hookshot_coop.dll", "longshot_programs.bin", "mkw_vk_pipelines.bin", "longshot_pipelines.bin.built",
                  "coop-options.ini", os.path.join("user", "save", "u", "slot_00", "game.sav"),
                  os.path.join("autotest-save", "account.sav")):
            self.assertEqual(nlink(os.path.join(a, f)), 1, f)
            self.assertEqual(read(os.path.join(a, f)), read(os.path.join(src, f)))
        # per run: never taken over (a stale marker made a game prepare every shader at each start)
        self.assertFalse(os.path.exists(os.path.join(a, "longshot_log.txt")))
        self.assertFalse(os.path.exists(os.path.join(a, "longshot_pipelines.bin.built.warming")))
        self.assertEqual(lanes.problems(1, self.kit, busy=self.idle), [])

    def test_refill_never_writes_into_a_shared_inode(self):
        src = self.game("A1")
        lanes.setup(1, src, self.game("B1"), say=self.say, kit=self.kit, busy=self.idle)
        dst = os.path.join(lanes.lane(1, self.kit)["a"], "coop-options.ini")
        os.unlink(dst)
        os.link(os.path.join(src, "coop-options.ini"), dst)   # someone hard-linked a written file
        self.assertTrue(any("hard-linked but the game writes it" in p for p in lanes.problems(1, self.kit, busy=self.idle)))
        write(os.path.join(src, "coop-options.ini"), "changed in the source")
        time.sleep(0.01)
        lanes.fill(src, os.path.dirname(dst), say=self.say)
        self.assertEqual(nlink(dst), 1)
        self.assertEqual(read(os.path.join(src, "coop-options.ini")), "changed in the source")

    def test_check_finds_a_stale_marker_and_a_missing_game(self):
        lanes.setup(1, self.game("A1"), self.game("B1"), say=self.say, kit=self.kit, busy=self.idle)
        write(os.path.join(lanes.lane(1, self.kit)["b"], "longshot_pipelines.bin.built.warming"), "1")
        ps = lanes.problems(1, self.kit, busy=self.idle)
        self.assertEqual(len(ps), 1)
        self.assertIn("stale shader-preparation marker", ps[0])
        self.assertIn("no game", lanes.problems(2, self.kit, busy=self.idle)[0])

    def test_setup_refuses_a_running_lane(self):
        with self.assertRaises(lanes.Fail):
            lanes.setup(1, self.game("A1"), self.game("B1"), say=self.say, kit=self.kit, busy=lambda f: ["game.exe"])

    def test_install_replaces_only_the_module_and_never_through_a_link(self):
        src = self.game("A1")
        L = lanes.setup(2, src, self.game("B1"), say=self.say, kit=self.kit, busy=self.idle)
        dll = os.path.join(self.tmp, "new", "hookshot_coop.dll")
        write(dll, "new module")
        exe_before = os.path.getmtime(os.path.join(L["a"], "longshot.exe"))
        lanes.install(2, dll, say=self.say, kit=self.kit, busy=self.idle)
        for g in (L["a"], L["b"]):
            self.assertEqual(read(os.path.join(g, "hookshot_coop.dll")), "new module")
        self.assertEqual(read(os.path.join(src, "hookshot_coop.dll")), "A1:hookshot_coop.dll")
        self.assertEqual(os.path.getmtime(os.path.join(L["a"], "longshot.exe")), exe_before)
        with self.assertRaises(lanes.Fail):
            lanes.install(2, dll, say=self.say, kit=self.kit, busy=lambda f: ["running"])


class Locks(Base):
    def setUp(self):
        super().setUp()
        self.root = os.path.join(self.tmp, "root")
        os.makedirs(self.root)

    def test_two_lanes_run_at_once_a_game_build_stops_both(self):
        q = lambda *a: None
        self.assertTrue(lanes.lock_acquire(1, "run one", root=self.root, say=q))
        self.assertTrue(lanes.lock_acquire(2, "run two", root=self.root, say=q))
        self.assertFalse(lanes.lock_acquire(1, "run three", root=self.root, say=q))   # lane 1 is busy
        hs.lock_release(self.root, "lane1", say=q)
        hs.lock_release(self.root, "lane2", say=q)
        self.assertTrue(hs.lock_acquire(self.root, "game", "a game build", say=q))
        self.assertFalse(lanes.lock_acquire(1, "run", root=self.root, say=q))
        self.assertFalse(lanes.lock_acquire(2, "run", root=self.root, say=q))
        hs.lock_release(self.root, "game", say=q)
        self.assertTrue(lanes.lock_acquire(2, "run", root=self.root, say=q))

    def test_each_game_of_a_lane_is_a_lane_of_its_own(self):
        """Two workers drive A and B of one lane at once; a pair run and either of its games exclude each other."""
        q = lambda *a: None
        self.assertTrue(lanes.lock_acquire(1, "worker A", root=self.root, say=q, game="A"))
        self.assertTrue(lanes.lock_acquire(1, "worker B", root=self.root, say=q, game="B"))
        self.assertFalse(lanes.lock_acquire(1, "pair run", root=self.root, say=q))
        self.assertFalse(lanes.lock_acquire(1, "worker A2", root=self.root, say=q, game="A"))
        self.assertTrue(lanes.lock_acquire(2, "lane 2 pair", root=self.root, say=q))
        hs.lock_release(self.root, "lane1-A", say=q)
        hs.lock_release(self.root, "lane1-B", say=q)
        self.assertTrue(lanes.lock_acquire(1, "pair run", root=self.root, say=q))
        self.assertFalse(lanes.lock_acquire(1, "worker B", root=self.root, say=q, game="B"))
        self.assertIn("lane1-A", hs.LANE_LOCKS)   # a game build waits for one-game sessions too

    def test_a_build_starting_between_take_and_check_wins(self):
        """The lane takes its lock, then a game build takes build-game.lock before the lane looks: the lane lets its lock
        go again, so the build (which looks for lane locks after taking its own) and the run never both go."""
        q = lambda *a: None

        def build_slips_in(root):
            hs.lock_acquire(root, "game", "a game build", say=q)
            return lanes.blockers(root)
        self.assertFalse(lanes.lock_acquire(1, "run", root=self.root, say=q, check=build_slips_in))
        self.assertFalse(os.path.isdir(os.path.join(self.root, "lane1.lock")))
        self.assertTrue(os.path.isdir(os.path.join(self.root, "build-game.lock")))
        # and the other order: the lane's lock is there when the build looks
        hs.lock_release(self.root, "game", say=q)
        self.assertTrue(lanes.lock_acquire(2, "run", root=self.root, say=q))
        self.assertFalse(hs.wait_free(os.path.join(self.root, "lane2.lock"), 0, say=q))

    def test_module_build_lock_blocks_no_lane_but_a_hand_made_one_does(self):
        q = lambda *a: None
        self.assertTrue(hs.lock_acquire(self.root, "hs", "compile-all", say=q))   # hs.py's JSON owner
        self.assertEqual(lanes.blockers(self.root), [])
        hs.lock_release(self.root, "hs", say=q)
        os.makedirs(os.path.join(self.root, "build-hs.lock"))
        write(os.path.join(self.root, "build-hs.lock", "owner.txt"), "someone 23:20 UTC game run")
        self.assertEqual(len(lanes.blockers(self.root)), 1)
        self.assertFalse(lanes.lock_acquire(1, "run", root=self.root, say=q))

    def test_build_waits_for_running_lanes(self):
        q = lambda *a: None
        hs.lock_acquire(self.root, "lane1", "a run", say=q)
        t = [0.0]
        ok = hs.wait_free(os.path.join(self.root, "lane1.lock"), 30, say=q, sleep=lambda s: t.__setitem__(0, t[0] + s),
                          clock=lambda: t[0])
        self.assertFalse(ok)
        hs.lock_release(self.root, "lane1", say=q)
        self.assertTrue(hs.wait_free(os.path.join(self.root, "lane1.lock"), 30, say=q))


class PidAlive(unittest.TestCase):
    def test_a_probe_that_cannot_tell_counts_as_alive(self):
        # A busy PC once made one tasklist call come back empty: the watchdog read "game B ended" and quit both games.
        self.assertTrue(hs.pid_alive(1234, probe=lambda pid: "unknown"))
        def broken(pid):
            raise OSError("the API failed")
        self.assertTrue(hs.pid_alive(1234, probe=broken))
        self.assertTrue(hs.pid_alive(1234, probe=lambda pid: "alive"))
        self.assertFalse(hs.pid_alive(1234, probe=lambda pid: "gone"))
        self.assertFalse(hs.pid_alive(0, probe=lambda pid: "alive"))

    def test_the_real_probe(self):
        self.assertTrue(hs.pid_alive(os.getpid()))
        p = subprocess.Popen([sys.executable, "-I", "-c", "pass"])
        p.wait()
        self.assertFalse(hs.pid_alive(p.pid))       # (exited; Popen still holds its handle on Windows: signalled)
        p = subprocess.Popen([sys.executable, "-I", "-c", "import time; time.sleep(30)"])
        try:
            self.assertTrue(hs.pid_alive(p.pid))
        finally:
            p.kill()
            p.wait()

    def test_a_busy_probe_does_not_end_a_live_session_or_free_a_live_lock(self):
        import live
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        with open(os.path.join(d, "session.json"), "w", encoding="utf-8") as f:
            json.dump({"started": time.time(), "max_s": 0, "booted": True, "idle_s": 0}, f)
        with open(os.path.join(d, "pids.json"), "w", encoding="utf-8") as f:
            json.dump({"games": {"A": 11, "B": 22}}, f)
        looks = []
        def sleep(_s):
            looks.append(1)
            if len(looks) == 3:
                open(os.path.join(d, "stop-request"), "w").close()
        alive = lambda pid: hs.pid_alive(pid, probe=lambda p: "unknown")
        self.assertEqual(live.watch(d, sleep=sleep, alive=alive, every=0), "stop asked")
        lock = os.path.join(d, "lock")
        os.makedirs(lock)
        write(os.path.join(lock, "owner.txt"), json.dumps({"host": hs.host_id(), "pid": 22, "time": time.time()}))
        self.assertEqual(hs.lock_stale(lock, alive=alive), "")


class CopyTreeLinks(Base):
    def test_cache_copy_unlinks_a_hard_link_before_writing(self):
        src, dst, other = (os.path.join(self.tmp, n) for n in ("cache", "lane", "other"))
        write(os.path.join(other, "SDL3.dll"), "old")
        os.makedirs(dst)
        os.link(os.path.join(other, "SDL3.dll"), os.path.join(dst, "SDL3.dll"))
        write(os.path.join(src, "SDL3.dll"), "new build")
        hs.copy_tree(src, dst, say=self.say)
        self.assertEqual(read(os.path.join(dst, "SDL3.dll")), "new build")
        self.assertEqual(read(os.path.join(other, "SDL3.dll")), "old")


class Orphans(Base):
    """C #133 (11:12 STOP-OFFLINE-QUEUE): a lane game up while no lock of its lane is live is an orphan; a job start
    closes it (main window first, kill only if still up) and never touches a game its lane's or game's lock covers."""

    def setUp(self):
        super().setUp()
        self.root = os.path.join(self.tmp, "root")
        os.makedirs(self.root)

    def procs(self, live):
        L = {x: lanes.lane(1, self.kit)[x] for x in ("a", "b")}
        table = {L["a"]: [(101, os.path.join(L["a"], "longshot.exe"))], L["b"]: [(202, os.path.join(L["b"], "longshot.exe"))]}
        return lambda folder: [p for p in table.get(folder, []) if p[0] in live]

    def test_no_lock_both_games_are_orphans(self):
        found, hold = lanes.orphans(1, self.kit, self.root, procs=self.procs({101, 202}))
        self.assertEqual(sorted((o["game"], o["pid"]) for o in found), [("A", 101), ("B", 202)])
        self.assertEqual(hold, "")

    def test_a_live_lock_covers_its_games(self):
        q = lambda *a: None
        self.assertTrue(lanes.lock_acquire(1, "a running job", root=self.root, say=q))
        found, _ = lanes.orphans(1, self.kit, self.root, procs=self.procs({101, 202}))
        self.assertEqual(found, [])
        hs.lock_release(self.root, "lane1", "a running job", say=q)
        self.assertTrue(lanes.lock_acquire(1, "B alone", root=self.root, say=q, game="B"))
        found, _ = lanes.orphans(1, self.kit, self.root, procs=self.procs({101, 202}))
        self.assertEqual([(o["game"], o["pid"]) for o in found], [("A", 101)])     # B's own lock covers B only

    def test_a_stale_lock_does_not_cover(self):
        q = lambda *a: None
        self.assertTrue(lanes.lock_acquire(1, "died", root=self.root, say=q))
        found, _ = lanes.orphans(1, self.kit, self.root, procs=self.procs({101}), now=time.time() + 5 * 3600)
        self.assertEqual([o["pid"] for o in found], [101])

    def test_a_lock_whose_owner_pid_is_alive_covers_at_any_age(self):
        old = time.time() - 6 * 3600          # (past hs.lock_stale's 4 h: a 240+ min bt_plugin session)
        self.assertTrue(hs.lock_acquire(self.root, "lane1", "long run", pid=os.getpid(), clock=lambda: old,
                                        say=lambda *a: None))
        found, _ = lanes.orphans(1, self.kit, self.root, procs=self.procs({101, 202}))
        self.assertEqual(found, [])
        found, _ = lanes.orphans(1, self.kit, self.root, procs=self.procs({101, 202}), alive=lambda pid: False)
        self.assertEqual(len(found), 2)          # (its owner gone: stale, both orphans)

    def test_a_game_build_holds_the_close(self):
        self.assertTrue(hs.lock_acquire(self.root, "game", "a game build", say=lambda *a: None))
        found, hold = lanes.orphans(1, self.kit, self.root, procs=self.procs({101}))
        self.assertEqual(len(found), 1)
        self.assertIn("build-game.lock", hold)

    def test_close_window_first_kill_if_still_up(self):
        up, asked, killed = {101, 202}, [], []
        def closer(pid):
            asked.append(pid)
            if pid == 101:
                up.discard(101)          # (A closes on its window)
        def killer(pid):
            killed.append(pid)
            up.discard(pid)
        t = [0.0]
        def clock():
            return t[0]
        def sleep(s):
            t[0] += s
        res = lanes.close_orphans([{"pid": 101}, {"pid": 202}], wait=5, closer=closer, killer=killer,
                                  alive=lambda pid: pid in up, clock=clock, sleep=sleep)
        self.assertEqual(res, {101: "closed", 202: "killed"})
        self.assertEqual(sorted(asked), [101, 202])
        self.assertEqual(killed, [202])


class GroupsInALane(Base):
    def test_every_group_on_the_lane_one_after_another(self):
        gs = groups.parse_groups("[g1]\nwalk\n[g2]\nchest\n")
        planned = groups.plan(gs, self.kit, os.path.join(self.tmp, "runs"), "server.exe", lane=2)
        L = lanes.lane(2, self.kit)
        for s, runs, cmds in planned:
            self.assertEqual((s["a"], s["port"], s["pipe"]), (L["a"], L["port"], L["pipe"]))
            self.assertEqual(cmds[0][-2:], ["-Lane", "2"])
        order = []
        groups.run_groups(planned, say=lambda *a: None,
                          runner=lambda cmd: order.append(cmd[cmd.index("-Scenario") + 1]) or
                          type("P", (), {"returncode": 0, "stdout": "", "stderr": ""})())
        self.assertEqual(order, ["walk", "chest"])

    def test_without_a_lane_groups_keep_their_own_pairs(self):
        gs = groups.parse_groups("[g1]\nwalk\n[g2]\nchest\n")
        planned = groups.plan(gs, self.kit, os.path.join(self.tmp, "runs"), "server.exe")
        self.assertNotEqual(planned[0][0]["port"], planned[1][0]["port"])
        self.assertNotIn("-Lane", planned[0][2][0])


class Nocs(Base):
    """lanes.py nocs: lane<N>-B-nocs from lane<N>-B (fakes for git, the pack tool, the kit's code-patch line and the
    hs cache; the stamp is the repo's real one)."""
    COMMIT = "fa63e90f19fa616203b13a67fcb83563545af406"
    NATIVE = "\nexefs=contents/0400486024DEC000/coop-puppet/exefs/E0F3.ips:e9e9;list=none\n"

    def setUp(self):
        super().setUp()
        self.L = lanes.lane(1, self.kit)
        b = self.L["b"]
        self.dll = b"module-fa63"
        for f in ("longshot.exe", "image.bin", "SDL3.dll", "coop-options.ini"):
            write(os.path.join(b, f), "B:" + f)
        with open(os.path.join(b, "hookshot_coop.dll"), "wb") as f:
            f.write(self.dll)
        write(os.path.join(b, "romfs", "Pack", "a.pack"), "pack")
        write(os.path.join(b, "mods", lanes.CS_MOD, "exefs", "E0F3.ips"), "his code")
        write(os.path.join(b, "mods", "_" + lanes.CS_MOD + ".prev", "x"), "kept aside")
        write(os.path.join(b, "mods", "oot-remake-randomizer", "x"), "seed")
        write(os.path.join(b, "mods", "coop-mods.list"), "coop-mods 1\non oot-remake-randomizer\non %s\n" % lanes.CS_MOD)
        write(os.path.join(b, hs.PACK_DIR, "coop_puppet.json"), "{\"old\": 1}")
        self.base = hs.stamp(lanes.REPO)
        write(os.path.join(b, "coop-built-from.txt"), self.base + "\nexefs=OoT Character Select/exefs/E0F3.ips:a35e;list=none\n")
        write(os.path.join(b, "lane-server.txt"), "# x\nby=lane-prep\ncommit=%s\nprotocol=18\nserver=C:\\s.exe\n"
              "dll_sha256=%s\npatches_line_sha256=AB\n" % (self.COMMIT, hashlib.sha256(self.dll).hexdigest().upper()))
        self.calls = []

    def pack(self, tree, romfs, pack):
        self.calls.append(("pack", romfs))
        self.assertFalse(os.path.exists(os.path.join(pack, "coop_puppet.json")))   # (lane B's pack never taken)
        write(os.path.join(pack, "exefs", "E0F3.ips"), "native tab")
        write(os.path.join(pack, "coop_puppet.json"), "{\"charsel_native\": [1]}")

    def restore(self, dst, repo, say, want):
        self.calls.append(("restore", want))
        exe = os.path.join(dst, "longshot.exe")
        os.unlink(exe)                                   # (hs.copy_tree: a hard link is unlinked, never written into)
        write(exe, "no-CS game")
        write(os.path.join(dst, "coop-built-from.txt"), want)
        write(os.path.join(dst, "exefs_patches.txt"), "# list\ncoop-puppet\tE0F3.ips\t1\tcode\t\n")
        return self.hit

    def run_nocs(self, **kw):
        self.hit = kw.pop("hit", True)
        args = dict(say=self.say, kit=self.kit, busy=self.idle, pack=self.pack, restore=self.restore,
                    head=lambda tree: self.COMMIT, mods_line=lambda mods, tree: self.NATIVE, mismatch=lambda d, t: None,
                    tree=lanes.REPO)
        args.update(kw)
        return lanes.nocs(1, **args)

    def test_made_from_lane_b_without_character_select(self):
        d = self.run_nocs()
        self.assertEqual(d, self.L["b_nocs"])
        b = self.L["b"]
        self.assertTrue(os.path.samefile(os.path.join(b, "image.bin"), os.path.join(d, "image.bin")))   # hard link
        self.assertEqual(read(os.path.join(d, "longshot.exe")), "no-CS game")
        self.assertEqual(read(os.path.join(b, "longshot.exe")), "B:longshot.exe")          # lane B untouched
        self.assertFalse(os.path.exists(os.path.join(d, "mods", lanes.CS_MOD)))
        self.assertFalse(os.path.exists(os.path.join(d, "mods", "_" + lanes.CS_MOD + ".prev")))
        self.assertTrue(os.path.isdir(os.path.join(b, "mods", lanes.CS_MOD)))
        self.assertTrue(os.path.isfile(os.path.join(d, "mods", "oot-remake-randomizer", "x")))
        self.assertEqual(read(os.path.join(d, "mods", "coop-mods.list")), "coop-mods 1\non oot-remake-randomizer\n")
        self.assertEqual(read(os.path.join(b, hs.PACK_DIR, "coop_puppet.json")), "{\"old\": 1}")
        self.assertEqual(self.calls[1], ("restore", self.base + self.NATIVE))
        srv = read(os.path.join(d, "lane-server.txt"))
        self.assertIn("nocs_of=lane1-B\n", srv)
        self.assertIn("commit=%s\n" % self.COMMIT, srv)
        self.assertIn("dll_sha256=%s\n" % hashlib.sha256(self.dll).hexdigest().upper(), srv)
        patches = next(l for l in self.base.split("\n") if l.startswith("patches="))
        self.assertIn("patches_line_sha256=%s\n" % hashlib.sha256(patches.encode()).hexdigest().upper(), srv)
        self.run_nocs()                                              # (again: same result, lane B still untouched)
        self.assertEqual(read(os.path.join(b, "longshot.exe")), "B:longshot.exe")

    def test_a_cache_miss_or_wrong_tree_fails_and_leaves_no_lane_server_txt(self):
        with self.assertRaisesRegex(lanes.Fail, "no game for the no-CS key"):
            self.run_nocs(hit=False)
        self.assertFalse(os.path.exists(os.path.join(self.L["b_nocs"], "lane-server.txt")))
        with self.assertRaisesRegex(lanes.Fail, "not lane 1's module commit"):
            self.run_nocs(head=lambda tree: "1234567")
        with self.assertRaisesRegex(lanes.Fail, "not co-op's own tab's"):
            self.run_nocs(mods_line=lambda m, t: "exefs=OoT Character Select/exefs/E0F3.ips:a;list=none\n")
        self.assertFalse(os.path.exists(os.path.join(self.L["b_nocs"], "lane-server.txt")))

    def test_a_lane_b_rebuilt_from_other_patches_is_refused(self):
        write(os.path.join(self.L["b"], "coop-built-from.txt"), "release=v0\ncommit=1\npatches=zz\n")
        with self.assertRaisesRegex(lanes.Fail, "not the build of"):
            self.run_nocs()

    def test_dry_run_writes_nothing_and_a_running_game_is_refused(self):
        self.run_nocs(dry=True)
        self.assertFalse(os.path.exists(self.L["b_nocs"]))
        with self.assertRaisesRegex(lanes.Fail, "a game is running"):
            self.run_nocs(busy=lambda f: ["x"] if f == self.L["b_nocs"] else [])

    def test_orphans_cover_the_nocs_game(self):
        procs = lambda folder: [(7, os.path.join(folder, "longshot.exe"))] if folder == self.L["b_nocs"] else []
        found, _ = lanes.orphans(1, self.kit, root=self.tmp, procs=procs, alive=lambda p: False)
        self.assertEqual([(o["game"], o["pid"]) for o in found], [("B", 7)])


class Cli(Base):
    def test_info_json(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(lanes.main(["info", "--lane", "1", "--json"]), 0)
        self.assertEqual(json.loads(out.getvalue())["port"], 27520)


if __name__ == "__main__":
    unittest.main()
