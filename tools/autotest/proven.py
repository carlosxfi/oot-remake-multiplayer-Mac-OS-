"""Which rc rows are already proven on the build under test, so one rc pass does not play them again.

    python -I tools/autotest/proven.py identity <pair A> <pair B> [--kit A,B] [--server EXE]   (the key, as JSON)
    python -I tools/autotest/proven.py identity --lane N [--kit-root DIR] [--kit A,B] [--server EXE]

Rule (B #128, 2026-11-26, zero-regressions rule): a row counts as proven only by a PASS run of the same row on the
identical build: the same module (sha256 of both games' hookshot_coop.dll), the same game (coop-built-from.txt of both
pair folders: release, pin commit, hashes of hookshot/patches, the code mods' line), the same mods pack (every file
under both pair folders' mods\\, by content), the same kits (--kit folders, every file by path, size and time), the
same server exe, the same save baseline (both pairs' autotest-save) and the same harness (HARNESS: run.ps1 and what it
runs), with the same scenario files (scenario_hash: A/B/steps scripts and load.txt, includes resolved), each taken
before AND after the run (a build changed during the run proves nothing). A FLAKY row (any other result of that row on that build, before or after the PASS), a FAIL,
SETUP-FAIL, UNPROVEN (NO-REF: a see= without a reference) or PLAYED row is never proven. Runs written before this file
(no "identity" in run.json) prove nothing.

The same row: run.json's scenario, switches, timeout, password (as a hash) and logonly= equal the row's, and its
expect= / never= / see= hold every one of the row's (a run that checked more still checked these).

groups.py writes the identity into every run.json (identity_of the pair, before the run starts); rc_plan.py split
--skip-proven <run roots> drops the rows proven on the lanes' identity and lists each one with its proof folders.
Reusable as is by a pytest runner: identity_of(), row_key(), proofs() take plain values and a judge callback.
"""
import argparse
import hashlib
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DLL = "hookshot_coop.dll"
BUILT_FROM = "coop-built-from.txt"
# Files under mods\ that a game start or a seed build WRITES (run output, not what the run plays), left out of the pack
# hash, else every run gets a new key and no row is ever proven (lane2 mods\coop-mods.applied is newer than its dll):
#   coop-mods.applied            hookshot/src/hos_coop_mods.h: "written by Hookshot at each start", line 1 = process id
#   coop-mods.starting           hookshot/src/coop_mod_library.h kStartingFile: written before a code mod starts
#   oot-remake-randomizer/last-build.txt   docs/mods.md "Randomizer" 2.: OOTRR's output of the last seed build
# What a run plays (the mods, coop-mods.list, coop-mods.settings.json, a seed's folder) stays in.
RUN_OUTPUTS = {"coop-mods.applied", "coop-mods.starting", "oot-remake-randomizer/last-build.txt"}
REPO = os.path.dirname(os.path.dirname(HERE))
SCENARIOS = os.path.join(HERE, "scenarios")
SAVE = "autotest-save"   # hookshot.py restore_save: the baseline each run copies over the game's saves
# What every run executes from the repo (review-c123-6 08:18): run.ps1 and what it dot-sources (kit-paths.ps1, :391),
# the Python it runs (run.ps1 :222: hookshot.py, lanes.py, placement.py, ocarina_notes.py, proof.py), the steps runner
# (hs_steps.py), and what those import from the repo (lanes.py -> tools/hookshot/hs.py; hookshot.py :1402 puppet_pack).
# A changed harness un-proves every row. A file missing here is hashed as missing.
HARNESS = ["tools/autotest/run.ps1", "tools/autotest/kit-paths.ps1", "tools/autotest/hookshot.py",
           "tools/autotest/hs_steps.py", "tools/autotest/lanes.py", "tools/autotest/placement.py",
           "tools/autotest/ocarina_notes.py", "tools/autotest/proof.py", "tools/hookshot/hs.py",
           "tools/puppet_pack/merge_tables.py", "tools/puppet_pack/self_tunic.py"]
# The in-game test system (docs/ingame-tests.md): what every bridge test runs besides its own module. refs/ is in
# whole (a test names its references in its body, pair.check_shot(game, name): not known from outside), so a changed
# or added reference un-proves every test (strict side).
INGAME = "tools/autotest/ingame"
INGAME_HARNESS = [INGAME + "/conftest.py", INGAME + "/bt_plugin.py", INGAME + "/pair.py", INGAME + "/imagecheck.py",
                  INGAME + "/pytest.ini", "tools/autotest/live.py", "tools/autotest/lanes.py",
                  "tools/autotest/ocarina_notes.py", "tools/autotest/desync_check.py", "tools/hookshot/hs.py"]
_hash_cache = {}


class Fail(Exception):
    pass


def file_sha(path):
    """sha256 of a file, cached on (path, size, mtime_ns) for the life of the process (a lane plays many runs)."""
    st = os.stat(path)
    k = (os.path.abspath(path), st.st_size, st.st_mtime_ns)
    if k not in _hash_cache:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        _hash_cache[k] = h.hexdigest()
    return _hash_cache[k]


def game_dll(pair):
    """The pair's module, where lib/lane-pair.ps1 Get-LaneGameDll looks: <pair>\\hookshot_coop.dll, else bin\\."""
    for p in (os.path.join(pair, DLL), os.path.join(pair, "bin", DLL)):
        if os.path.isfile(p):
            return p
    return None


def tree_hash(folder, content=True, leave_out=()):
    """One sha256 over every file under folder (relative path + content sha, or + size + mtime with content=False);
    '' when the folder is missing. Paths in lower case with '/' so two lanes' equal packs give equal hashes."""
    if not os.path.isdir(folder):
        return ""
    h = hashlib.sha256()
    for top, dirs, files in os.walk(folder):
        dirs.sort()
        for name in sorted(files):
            p = os.path.join(top, name)
            rel = os.path.relpath(p, folder).replace("\\", "/").lower()
            if rel in leave_out:
                continue
            if content:
                v = file_sha(p)
            else:
                st = os.stat(p)
                v = "%d:%d" % (st.st_size, st.st_mtime_ns)
            h.update(("%s\0%s\n" % (rel, v)).encode("utf-8"))
    return h.hexdigest()


def built_from(pair):
    """coop-built-from.txt of a pair, line endings and trailing space dropped (hs.py key_of's normal form)."""
    p = os.path.join(pair, BUILT_FROM)
    if not os.path.isfile(p):
        return ""
    with open(p, encoding="utf-8-sig", errors="replace") as f:
        return "\n".join(line.rstrip() for line in f.read().strip().splitlines())


def harness_hash(repo=None):
    """One sha256 over the HARNESS files (path + content sha, or 'missing')."""
    repo = repo or REPO
    h = hashlib.sha256()
    for rel in HARNESS:
        p = os.path.join(repo, *rel.split("/"))
        h.update(("%s\0%s\n" % (rel, file_sha(p) if os.path.isfile(p) else "missing")).encode("utf-8"))
    return h.hexdigest()


def base_nodeid(nodeid):
    """A pytest nodeid without bt_plugin's --repeat parameter ([r3], or r3 inside [a-r3]): the same test."""
    m = re.match(r"^(.*?)\[(.*)\]$", nodeid)
    if not m:
        return nodeid
    rest = [p for p in m.group(2).split("-") if not re.match(r"^r\d+$", p)]
    return m.group(1) + ("[%s]" % "-".join(rest) if rest else "")


def test_hash(nodeid, repo=None):
    """24 hex digits over what a bridge test runs: its own module file, INGAME_HARNESS and every file under
    ingame/refs. The nodeid's path is as pytest prints it: relative to its rootdir, which is tools/autotest/ingame
    (its pytest.ini; `test_rows.py::test_x`), or relative to the repo (`tools/autotest/ingame/test_rows.py::...`):
    both name the same file and give the same hash. '' when the module is not there (such a test proves nothing)."""
    repo = repo or REPO
    path = nodeid.split("::", 1)[0].replace("\\", "/")
    for rel in (INGAME + "/" + path, path):
        if os.path.isfile(os.path.join(repo, *rel.split("/"))):
            break
    else:
        return ""
    h = hashlib.sha256()
    for rel in [rel] + INGAME_HARNESS:
        q = os.path.join(repo, *rel.split("/"))
        h.update(("%s\0%s\n" % (rel, file_sha(q) if os.path.isfile(q) else "missing")).encode("utf-8"))
    h.update(("refs\0%s\n" % tree_hash(os.path.join(repo, *INGAME.split("/"), "refs"))).encode("utf-8"))
    return h.hexdigest()[:24]


def env_key(env):
    """The game switches a boot gives each game, as one comparable text: {"A": {NAME: VALUE}, "B": {...}} from a
    list of NAME=VALUE / A:NAME=VALUE / B:NAME=VALUE (bt_plugin boot_env, live.py start --env's form; the last setting
    of a name on a game wins, as bt_plugin env_has reads it). None when env is not a list (not known)."""
    if not isinstance(env, list):
        return None
    per = {"A": {}, "B": {}}
    for e in env:
        e = str(e).strip()
        m = re.match(r"^([AB]):(.*)$", e)
        tag, kv = (m.group(1), m.group(2)) if m else ("", e)
        k, _, v = kv.partition("=")
        for g in ([tag] if tag else ["A", "B"]):
            per[g][k] = v
    return json.dumps(per, sort_keys=True)


def ingame_base_env(repo=None):
    """bt_plugin.BASE_ENV (the switches every in-game boot gets), read from its source: the one table, never copied.
    Raises Fail when bt_plugin.py or the line is not there."""
    p = os.path.join(repo or REPO, *INGAME.split("/"), "bt_plugin.py")
    try:
        with open(p, encoding="utf-8") as fh:
            text = fh.read()
    except OSError as e:
        raise Fail("no %s: %s" % (p, e))
    m = re.search(r"^BASE_ENV\s*=\s*(\[[^\]\n]*\])", text, re.M)
    if not m:
        raise Fail("%s: no BASE_ENV = [...] line" % p)
    import ast
    return list(ast.literal_eval(m.group(1)))


def boot_key(boot):
    """The lane boot (bt_plugin boot_identity: per game the boot script, its sha256 and its '#! switches'), as one
    comparable text; None when it is not a dict holding both games (not known: proves nothing)."""
    if not isinstance(boot, dict) or not all(isinstance(boot.get(g), dict) for g in ("A", "B")):
        return None
    norm = {g: {"script": str(boot[g].get("script") or "").replace("\\", "/"), "sha256": boot[g].get("sha256") or "",
                "switches": sorted(str(x) for x in (boot[g].get("switches") or []))} for g in ("A", "B")}
    if not all(norm[g]["script"] and norm[g]["sha256"] for g in ("A", "B")):
        return None
    return json.dumps(norm, sort_keys=True)


def hs():
    """tools/hookshot/hs.py: run_quiet starts a console child with no window (tools/hookshot/tests/test_no_window.py)."""
    p = os.path.join(REPO, "tools", "hookshot")
    if p not in sys.path:
        sys.path.insert(0, p)
    import hs as _hs
    return _hs


def ingame_boot_identity(repo=None, name=None):
    """bt_plugin.boot_identity(name) of the repo's in-game tests (name None: its default boot): the boot this rc pass
    makes, from bt_plugin's own code (never copied), in a Python process of its own (bt_plugin imports pair, pytest).
    Raises Fail when it cannot (an unknown boot name too)."""
    ingame = os.path.join(repo or REPO, *INGAME.split("/"))
    code = ("import sys, json; sys.path.insert(0, sys.argv[1]); import bt_plugin; "
            "a = sys.argv[2:]; print(json.dumps(bt_plugin.boot_identity(*a)))")
    import subprocess
    try:
        p = hs().run_quiet([sys.executable, "-I", "-c", code, ingame] + ([name] if name else []),
                           capture_output=True, text=True, timeout=120)
        boot = json.loads(p.stdout.strip().splitlines()[-1]) if p.returncode == 0 and p.stdout.strip() else None
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        raise Fail("bt_plugin.boot_identity: %s" % e)
    if boot_key(boot) is None:
        raise Fail("bt_plugin.boot_identity in %s gave no boot (exit %s): %s" % (
            ingame, p.returncode, (p.stderr or p.stdout or "")[-400:]))
    return boot


_PLAY = re.compile(r"^\s*play\s+(.*)$", re.I)


def scenario_hash(name, folder=None):
    """24 hex digits over what a row's scenario plays (review-c123-6 08:18): load.txt, <name>.A.txt, <name>.B.txt,
    <name>.steps.txt and every script a steps file's `play A f [B f]` lines name, each with its includes resolved by
    hookshot.read_script (the runner's own reader; run.ps1 :142-144 and :282 name the files). '' when the scenario
    has no file of its own (a run of it proves nothing)."""
    folder = folder or SCENARIOS
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    import hookshot
    files = ["load.txt", name + ".A.txt", name + ".B.txt", name + ".steps.txt"]
    steps = os.path.join(folder, name + ".steps.txt")
    if os.path.isfile(steps):
        for line in hookshot.read_script(steps).split("\n"):
            m = _PLAY.match(line.split("#", 1)[0])
            if m:
                w = m.group(1).split()
                files += [w[i] for i in range(1, len(w), 2)]
    h, own = hashlib.sha256(), False
    for rel in files:
        p = os.path.join(folder, rel)
        if os.path.isfile(p):
            own = own or rel != "load.txt"
            text = hookshot.read_script(p)
        else:
            text = "\0missing"
        h.update(("%s\0%s\n" % (rel, hashlib.sha256(text.encode("utf-8")).hexdigest())).encode("utf-8"))
    return h.hexdigest()[:24] if own else ""


def identity_of(a, b, kits=None, server=None, repo=None):
    """The build a run plays, as a dict with "key" (24 hex digits over everything else): both games' module, game,
    mods pack and save baseline (<pair>/autotest-save), the kits, the server and the harness (HARNESS). Raises Fail
    when a pair has no module or no coop-built-from.txt: such a run can never prove a row."""
    parts = {}
    for side, pair in (("a", a), ("b", b)):
        dll = game_dll(pair)
        if not dll:
            raise Fail("%s: no %s" % (pair, DLL))
        game = built_from(pair)
        if not game:
            raise Fail("%s: no %s" % (pair, BUILT_FROM))
        parts["module_" + side] = file_sha(dll)
        parts["game_" + side] = hashlib.sha256(game.encode("utf-8")).hexdigest()
        parts["mods_" + side] = tree_hash(os.path.join(pair, "mods"), leave_out=RUN_OUTPUTS)
        parts["save_" + side] = tree_hash(os.path.join(pair, SAVE))
    parts["kits"] = [tree_hash(k, content=False) for k in (kits or [])]
    if kits and any(not h for h in parts["kits"]):
        raise Fail("a --kit folder is missing: %s" % ", ".join(kits))
    parts["server"] = file_sha(server) if server and os.path.isfile(server) else ""
    parts["harness"] = harness_hash(repo)
    parts["key"] = hashlib.sha256(json.dumps(parts, sort_keys=True).encode("utf-8")).hexdigest()[:24]
    return parts


def secret_hash(v):
    return hashlib.sha256(v.encode("utf-8")).hexdigest()[:12] if v else ""


def _checks(seq):
    return {tuple(x) if isinstance(x, (list, tuple)) else ("", x) for x in (seq or [])}


def row_key(run):
    """(command, checks) of a parsed groups.py run or a run.json: command = what run.ps1 plays, checks = what is
    judged on it. A run.json holds the password as password_hash (never the password)."""
    pw = run.get("password_hash")
    if pw is None:
        pw = secret_hash(run.get("password") or "")
    cmd = (run.get("scenario") or "", run.get("switches") or "", str(run.get("timeout") or ""), pw,
           run.get("logonly") or "", run.get("scenario_hash") or "")
    checks = (_checks(run.get("expects")), _checks(run.get("never")), set(run.get("see") or []))
    return cmd, checks


def same_row(row, meta):
    """meta (a run.json) played row: the same command and scenario files, and checked at least the row's checks.
    A row whose scenario files are not known (no scenario_hash) is never the same as any run."""
    if not row.get("scenario_hash"):
        return False
    rc, (re_, rn, rs) = row_key(row)
    mc, (me, mn, ms) = row_key(meta)
    return rc == mc and re_ <= me and rn <= mn and rs <= ms


def runs_with_identity(roots):
    """[(folder, run.json dict)] of every run under the roots whose run.json has an identity key."""
    out = []
    for root in roots or []:
        for d, _, files in os.walk(root):
            if "run.json" not in files:
                continue
            try:
                with open(os.path.join(d, "run.json"), encoding="utf-8") as f:
                    meta = json.load(f)
            except (OSError, ValueError):
                continue
            if isinstance(meta, dict) and (meta.get("identity") or {}).get("key"):
                out.append((d, meta))
    return out


def proofs(row, keys, runs, judge):
    """(proven, {key: [(folder, result)]}): the row is proven when, for EVERY key (each lane's build), it has at least
    one run on that build and every run of it on that build judged PASS (judge(folder, meta) -> result)."""
    seen = {k: [] for k in keys}
    for folder, meta in runs:
        k = meta["identity"]["key"]
        if k in seen and same_row(row, meta):
            seen[k].append((folder, judge(folder, meta)))
    ok = bool(keys) and all(seen[k] and all(r == "PASS" for _, r in seen[k]) for k in keys)
    return ok, seen


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("identity")
    i.add_argument("pairs", nargs="*", help="pair A and pair B folders (or --lane)")
    i.add_argument("--lane", type=int)
    i.add_argument("--kit-root")
    i.add_argument("--kit", help="A,B kit folders (groups.py --kit)")
    i.add_argument("--server")
    a = ap.parse_args(argv)
    try:
        if a.lane:
            sys.path.insert(0, HERE)
            import lanes
            L = lanes.lane(a.lane, a.kit_root)
            pa, pb = L["a"], L["b"]
        elif len(a.pairs) == 2:
            pa, pb = a.pairs
        else:
            raise Fail("identity: two pair folders or --lane N")
        kits = [os.path.abspath(k.strip()) for k in a.kit.split(",") if k.strip()] if a.kit else None
        print(json.dumps(identity_of(pa, pb, kits, a.server), indent=1))
        return 0
    except Exception as e:   # lanes.Fail too
        print("proven.py: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
