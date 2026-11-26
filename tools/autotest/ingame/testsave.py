"""Save profiles for one boot of the in-game tests (bt_plugin --save NAME, marker needs_save(NAME)): flags set in BOTH
lane games' save baseline (<game>/autotest-save/slot_NN/progress.sav, which hookshot.py restore_save copies over the
game's saves at its start) before live.py start, and the baseline put back after live.py stop, so the next job finds
the lane as it was.

A profile is ingame/saves/NAME.py (or $BT_SAVES_DIR/NAME.py) with

    FLAGS = ["enum:0x7d776b61=0xb5df43cb", "bool:0x6b093b51=0", ...]   # save_flags.py set's form, each cited

Only flags already in the save are changed (save_flags.set_values; a missing one is a SETUP-FAIL). Per game folder:
<game>/coop-test-save.json names the profile and the files. It is written BEFORE any save changes, so a run that dies
at any point leaves a manifest the next boot undoes (undo() at every lane_pair start). Each save is copied to
<game>/coop-test-save-kept/<rel> first, then replaced whole (a temp file and os.replace, never written into). Each
flag is read back on every save (SETUP-FAIL on a miss).
"""
import hashlib
import importlib.util
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import save_flags as SF  # noqa: E402

SAVES = os.path.join(HERE, "saves")
BASE = "autotest-save"
MANIFEST = "coop-test-save.json"
KEPT = "coop-test-save-kept"


class SaveProfileError(Exception):
    """A profile could not be read or put in (the boot's set-up: SETUP-FAIL)."""


def profile_path(name, saves_dir=None):
    return os.path.join(saves_dir or os.environ.get("BT_SAVES_DIR") or SAVES, name + ".py")


def load(name, saves_dir=None):
    """[(kind, hash, value)] of profile NAME."""
    if not name or not all(c.isalnum() or c in "-_" for c in name):
        raise SaveProfileError("--save %r: a profile name (ingame/saves/NAME.py)" % name)
    path = profile_path(name, saves_dir)
    if not os.path.isfile(path):
        raise SaveProfileError("--save %s: no %s" % (name, path))
    spec = importlib.util.spec_from_file_location("btsave_" + name.replace("-", "_"), path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    flags = getattr(mod, "FLAGS", None)
    if not flags or not isinstance(flags, (list, tuple)):
        raise SaveProfileError("--save %s: %s has no FLAGS list" % (name, path))
    out = []
    for f in flags:
        try:
            kind, h, value = SF.parse_flag(f)
        except (SF.SaveError, ValueError) as e:
            raise SaveProfileError("--save %s: %r: %s" % (name, f, e))
        if value is None:
            raise SaveProfileError("--save %s: %r has no value (type:hash=value)" % (name, f))
        out.append((kind, h, value))
    return out


def saves_of(folder):
    """The baseline's progress.sav files, relative to the game folder ('/' separated)."""
    base = os.path.join(folder, BASE)
    if not os.path.isdir(base):
        return []
    out = []
    for d in sorted(os.listdir(base)):
        if SF.SLOT.match(d) and os.path.isfile(os.path.join(base, d, "progress.sav")):
            out.append("%s/%s/progress.sav" % (BASE, d))
    return out


def _p(folder, rel):
    return os.path.join(folder, *rel.split("/"))


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def put_in(folders, name, changes, say=print):
    """Profile NAME's flags into every save of each game's baseline. {folder name: {rel: sha256}} once every flag
    reads back on every save."""
    want = {(k, h): (v if k != SF.TYPES["bool"] else (1 if v else 0)) for k, h, v in changes}
    got = {}
    for folder in folders:
        rels = saves_of(folder)
        if not rels:
            raise SaveProfileError("%s: no %s/slot_NN/progress.sav to set --save %s in" % (folder, BASE, name))
        if os.path.exists(os.path.join(folder, MANIFEST)):
            raise SaveProfileError("%s: a save profile is still in (%s): undo it first" % (folder, MANIFEST))
        with open(os.path.join(folder, MANIFEST), "w", encoding="utf-8") as f:
            json.dump({"profile": name, "files": rels}, f, indent=1)   # (first: undo() knows what to put back)
        for rel in rels:
            src, kept = _p(folder, rel), _p(os.path.join(folder, KEPT), rel)
            old = SF.read_file(src)
            data = bytearray(old)
            try:
                SF.set_values(data, changes)
                SF.index(bytes(data))
            except SF.SaveError as e:
                raise SaveProfileError("%s: --save %s: %s" % (src, name, e))
            os.makedirs(os.path.dirname(kept), exist_ok=True)
            if not os.path.exists(kept):            # (a kept copy already there is the true baseline: never replaced)
                shutil.copyfile(src, kept)
            tmp = src + ".tmp-test-save"
            with open(tmp, "wb") as f:
                f.write(bytes(data))
            os.replace(tmp, src)
        say("save: %s into %s (%d save(s))" % (name, folder, len(rels)))
    for folder in folders:
        for rel in saves_of(folder):
            data = SF.read_file(_p(folder, rel))
            for (k, h), v in want.items():
                have = SF.get(data, k, h)
                if have is None or (have & 0xff if k == SF.TYPES["bool"] else have) != v:
                    raise SaveProfileError("%s %s: flag %#010x is %s, not %s after --save %s"
                                           % (folder, rel, h, have, v, name))
            got.setdefault(os.path.basename(folder), {})[rel] = sha256(_p(folder, rel))
    return got


def undo(folder, say=print):
    """The baseline back in a game folder (each kept save over the profile's); [] when none was in. Safe to run again
    after a partial undo."""
    mf = os.path.join(folder, MANIFEST)
    if not os.path.isfile(mf):
        return []
    try:
        with open(mf, encoding="utf-8") as f:
            plan = json.load(f)
    except (OSError, ValueError) as e:
        raise SaveProfileError("%s: %s unreadable (%s): put %s back by hand from %s" % (folder, MANIFEST, e, BASE, KEPT))
    done = []
    for rel in plan.get("files", []):
        kept, dst = _p(os.path.join(folder, KEPT), rel), _p(folder, rel)
        if os.path.isfile(kept):
            os.replace(kept, dst)
            done.append(rel)
    shutil.rmtree(os.path.join(folder, KEPT), ignore_errors=True)
    os.remove(mf)
    say("save: %s out of %s (%d put back)" % (plan.get("profile"), folder, len(done)))
    return done
