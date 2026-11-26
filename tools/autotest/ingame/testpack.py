"""Pack files for one boot of the in-game tests (bt_plugin --pack NAME, marker needs_pack(NAME)): files of the co-op
pack that the TESTED sha's tools make (e.g. the HUD's map arrow, coop_look/cap.bin), put into BOTH lane games' co-op
pack before live.py start and taken out again after live.py stop, so the next job finds the lane as it was.

A pack maker is ingame/packs/NAME.py (or $BT_PACKS_DIR/NAME.py) with

    def make(out, ctx):   # -> {rel: src}: rel under the game's coop-puppet mod folder, '/' separated (e.g.
        ...               #    'coop_hud/map-arrow.png'); src a file the maker wrote (in out, a fresh folder)

ctx: ctx.tools (the tested sha's tools folder, on sys.path), ctx.romfs (lane A's extracted romfs: the base game,
read only), ctx.game_nup (the game's .nup, '' when none was found), ctx.lane, ctx.say(line).

Per game folder: a file a pack replaces is MOVED to <game>/coop-test-pack-kept/<rel> (moved, never written into: a
lane file may be a hard link), the pack's file is copied in, and <game>/coop-test-pack.json names what was added and
replaced. It is written BEFORE any file moves, so a run that dies at any point leaves a manifest the next boot undoes
(undo() at every lane_pair start; lanes.py check reports one left). Every file is checked by sha256 on each game.
"""
import hashlib
import importlib.util
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PACKS = os.path.join(HERE, "packs")
# the co-op pack's folder in a game folder (tools/hookshot/hs.py PACK_DIR; tests/test_ingame.py checks they are equal)
PACK_REL = os.path.join("mods", "contents", "0400486024DEC000", "coop-puppet")
MANIFEST = "coop-test-pack.json"
KEPT = "coop-test-pack-kept"


class PackError(Exception):
    """A pack could not be made or put in (the boot's set-up: SETUP-FAIL)."""


class Ctx:
    def __init__(self, tools, romfs, game_nup, lane, say=print):
        self.tools, self.romfs, self.game_nup, self.lane, self.say = tools, romfs, game_nup, lane, say


def names_of(opt):
    """--pack values (repeatable, each NAME or NAME,NAME) as a sorted list without repeats."""
    out = set()
    for v in opt or []:
        out.update(n.strip() for n in str(v).split(",") if n.strip())
    return sorted(out)


def maker_path(name, packs_dir=None):
    return os.path.join(packs_dir or os.environ.get("BT_PACKS_DIR") or PACKS, name + ".py")


def load(name, packs_dir=None):
    path = maker_path(name, packs_dir)
    if not os.path.isfile(path):
        raise PackError("--pack %s: no %s" % (name, path))
    spec = importlib.util.spec_from_file_location("btpack_" + name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    if not callable(getattr(mod, "make", None)):
        raise PackError("--pack %s: %s has no make(out, ctx)" % (name, path))
    return mod


def _check_rel(name, rel):
    parts = rel.split("/")
    if not rel or "\\" in rel or rel.startswith("/") or ":" in rel or any(p in ("", ".", "..") for p in parts):
        raise PackError("--pack %s: %r is not a path under the co-op pack ('/' separated, no '..')" % (name, rel))


def make(names, out, ctx, packs_dir=None):
    """{name: {rel: src}} from each maker, every src an existing file and no rel made twice."""
    if ctx.tools and ctx.tools not in sys.path:
        sys.path.insert(0, ctx.tools)
    made, seen = {}, {}
    for name in names:
        d = os.path.join(out, "pack", name)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        try:
            files = load(name, packs_dir).make(d, ctx)
        except PackError:
            raise
        except Exception as e:
            raise PackError("--pack %s: make failed: %s: %s" % (name, type(e).__name__, e))
        if not files:
            raise PackError("--pack %s: make gave no files" % name)
        for rel, src in files.items():
            _check_rel(name, rel)
            if not os.path.isfile(src):
                raise PackError("--pack %s: %s: no file %s" % (name, rel, src))
            if rel in seen:
                raise PackError("--pack %s and %s both make %s" % (seen[rel], name, rel))
            seen[rel] = name
        made[name] = dict(files)
    return made


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def _dst(folder, rel):
    return os.path.join(folder, PACK_REL, *rel.split("/"))


def put_in(folders, made, say=print):
    """Each made file into each game folder's co-op pack; {name: {rel: sha256}} once all are checked on every game."""
    want = {rel: (name, src, sha256(src)) for name, files in made.items() for rel, src in files.items()}
    for folder in folders:
        if not os.path.isdir(os.path.join(folder, PACK_REL)):
            raise PackError("%s: no co-op pack (%s) to put pack files into" % (folder, PACK_REL))
        if os.path.exists(os.path.join(folder, MANIFEST)):
            raise PackError("%s: a test pack is still in (%s): undo it first" % (folder, MANIFEST))
        rels = sorted(want)
        plan = {"packs": sorted(made), "added": [r for r in rels if not os.path.lexists(_dst(folder, r))],
                "replaced": [r for r in rels if os.path.lexists(_dst(folder, r))]}
        with open(os.path.join(folder, MANIFEST), "w", encoding="utf-8") as f:
            json.dump(plan, f, indent=1)          # (first: whatever happens next, undo() knows what to put back)
        for rel in rels:
            dst = _dst(folder, rel)
            if rel in plan["replaced"]:
                kept = os.path.join(folder, KEPT, *rel.split("/"))
                os.makedirs(os.path.dirname(kept), exist_ok=True)
                os.replace(dst, kept)             # (the directory entry moves: a hard link stays the other folder's)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(want[rel][1], dst)
        say("pack: %s into %s (%d added, %d replaced)" % (", ".join(plan["packs"]), folder, len(plan["added"]),
                                                          len(plan["replaced"])))
    for folder in folders:
        for rel, (name, src, h) in want.items():
            got = sha256(_dst(folder, rel)) if os.path.isfile(_dst(folder, rel)) else "missing"
            if got != h:
                raise PackError("%s: pack %s %s is %s, not %s" % (folder, name, rel, got[:12], h[:12]))
    out = {}
    for rel, (name, src, h) in want.items():
        out.setdefault(name, {})[rel] = h
    return out


def undo(folder, say=print):
    """A test pack out of a game folder (its manifest's added files removed, the replaced ones put back); [] when
    there was none. Safe to run again after a partial undo (each step checks what is there)."""
    mf = os.path.join(folder, MANIFEST)
    if not os.path.isfile(mf):
        return []
    try:
        with open(mf, encoding="utf-8") as f:
            plan = json.load(f)
    except (OSError, ValueError) as e:
        raise PackError("%s: %s unreadable (%s): put the pack back by hand" % (folder, MANIFEST, e))
    done = []
    for rel in plan.get("added", []):
        dst = _dst(folder, rel)
        if os.path.lexists(dst):
            os.unlink(dst)
            done.append("removed " + rel)
    for rel in plan.get("replaced", []):
        dst, kept = _dst(folder, rel), os.path.join(folder, KEPT, *rel.split("/"))
        if os.path.lexists(kept):
            if os.path.lexists(dst):
                os.unlink(dst)
            os.replace(kept, dst)
            done.append("put back " + rel)
        elif not os.path.lexists(dst):
            raise PackError("%s: %s was replaced but its kept copy is gone" % (folder, rel))
    shutil.rmtree(os.path.join(folder, KEPT), ignore_errors=True)
    os.remove(mf)
    say("pack: %s out of %s (%s)" % (", ".join(plan.get("packs", [])), folder, "; ".join(done) or "nothing left"))
    return done
