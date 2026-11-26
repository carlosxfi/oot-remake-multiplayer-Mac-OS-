"""Which rc rows are already proven on the build under test, so one rc pass does not play them again.

    python -I tools/autotest/proven.py identity <pair A> <pair B> [--kit A,B] [--server EXE]   (the key, as JSON)
    python -I tools/autotest/proven.py identity --lane N [--kit-root DIR] [--kit A,B] [--server EXE]

Rule (B #128, 2026-11-26, zero-regressions rule): a row counts as proven only by a PASS run of the same row on the
identical build: the same module (sha256 of both games' hookshot_coop.dll), the same game (coop-built-from.txt of both
pair folders: release, pin commit, hashes of hookshot/patches, the code mods' line), the same mods pack (every file
under both pair folders' mods\\, by content), the same kits (--kit folders, every file by path, size and time) and the
same server exe. A FLAKY row (any other result of that row on that build, before or after the PASS), a FAIL,
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


def identity_of(a, b, kits=None, server=None):
    """The build a run plays, as a dict with "key" (24 hex digits over everything else). Raises Fail when a pair has
    no module or no coop-built-from.txt: such a run can never prove a row."""
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
    parts["kits"] = [tree_hash(k, content=False) for k in (kits or [])]
    if kits and any(not h for h in parts["kits"]):
        raise Fail("a --kit folder is missing: %s" % ", ".join(kits))
    parts["server"] = file_sha(server) if server and os.path.isfile(server) else ""
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
           run.get("logonly") or "")
    checks = (_checks(run.get("expects")), _checks(run.get("never")), set(run.get("see") or []))
    return cmd, checks


def same_row(row, meta):
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
