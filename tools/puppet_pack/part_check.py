"""Row 280 guard: a mod part the kit made never names a file that is neither in the part nor in the game.

The kit installs part of a mod (Character Select's default part: owner, 0.11.9). Each rule that leaves files out can
leave a kept file naming one of them: row 242 (Link's models named textures of the NPC part), row 280 (Snail's adult
Link packs name Model/Player_Dark_Animation.Skeleton, which the NPC rule dropped; the game has only the .anim of that
name). The game then waits for that file for good: every load of it is a black world.

So, after the kit makes a part (both kits, Windows kit-paths.ps1 and linux build.sh, before the part goes in place):

  part_check.py list  --mod <part> [--recipes <tool's recipes.json>]
      writes <part>/coop-release-files.txt: every RomFS path the mod's release has (the files in <part>/romfs now,
      before the kit leaves any out, and every file its tool makes, the excluded ones too). A part made from another
      copy keeps the list it came with.
  part_check.py check --mod <part> --romfs <the game's RomFS> --name <mod name>
      a released file the part does not have and the game does not have is MISSING. A kept file that names one (its
      bytes, unpacked, hold the missing file's name: Model/<name>.<x>.bfres.zs, Tex/<name>.bntx.zs, ...) is left out,
      so the game's own file of that path is used (vanilla for that set); again until none is left (a left-out file
      can be one another kept file names). Each left-out file's size-table entry is raised to the game's own, so the
      game's file fits. <part>/coop-fallback.txt then holds one line for the player (the module shows it on the
      game's banner once a load and on the Mods tab); with nothing left out, no such file.

Exit 0 (and the lines it printed say what it did), 2 when it could not read what it needs (the kit says so; the part
is kept as it is). Logic in plan(); tests: tests/test_part_check.py.
"""
import argparse
import json
import os
import re
import sys

RELEASE_LIST = "coop-release-files.txt"
NOTE = "coop-fallback.txt"
LEFT_LIST = "coop-left-out.txt"  # (the paths the check left out, for a part made from this one)
# Files that name other files: packs, models, tables, sequences. Textures, sounds and cloth only hold data.
_NAMING = re.compile(r"^(Pack|Model|Sequence|RSDB|GameData|Mals|UI|AI|Logic|Component|Event|Effect)/", re.I)
_TOKEN = re.compile(rb"[A-Za-z0-9_]{4,}")
_BIG = 64 << 20


def stem(rel):
    """The name a file is named by: its base name up to the first '.' (Model/Player_Dark_Animation.Skeleton.bfres.zs
    -> Player_Dark_Animation)."""
    return rel.replace("\\", "/").rsplit("/", 1)[-1].split(".", 1)[0]


def top(rel):
    """The RomFS folder a path is in (Model, Phive, Tex, ...): a name is a file of one folder."""
    return rel.replace("\\", "/").split("/", 1)[0]


def files_under(root):
    out = set()
    for d, _, names in os.walk(root):
        for n in names:
            out.add(os.path.relpath(os.path.join(d, n), root).replace(os.sep, "/"))
    return out


def plan(released, kept, game, names_of, game_names_of=lambda p: set(), guarded=None):
    """(left_out: kept paths to leave out, missing: {left-out path: the missing paths it names}).

    released / kept / game: sets of RomFS paths ('/'); names_of(path): the set of name tokens in that kept file's
    bytes; game_names_of(path): the same in the game's own file of that path (empty: the game has none). A released
    path neither kept nor in the game is missing; a left-out path the game does not have is missing too (another
    kept file may name it). A name the game's own copy of the file names too, while the game has no file of that
    name in the missing file's folder, is not a file the game loads (the game's ActorInfo names Dark Link's cloth
    Player_Adult_Dark and the game has no Phive file of that name): it does not count. Row 280's does: the game's
    adult pack names Player_Dark_Animation too, but the game has a Model file of that name (the .anim), so the mod's
    pack may want another (his .Skeleton). guarded: a regular expression of missing paths the module guards itself
    (Character Select's character models, Model/Player_<Age>_C<nnn>: only a stored pick names one, and the module
    puts a pick of one that is not installed back, coop_char_slots.h): they do not count."""
    kept = set(kept)
    game_stems = {(top(p), stem(p)) for p in game}
    missing = {p for p in released if p not in kept and p not in game and not (guarded and re.search(guarded, p))}
    left, why, cache = [], {}, {}
    while True:
        stems = {}
        for p in missing:
            stems.setdefault(stem(p), set()).add(p)
        found = []
        for p in sorted(kept):
            if not _NAMING.match(p):
                continue
            if p not in cache:
                cache[p] = names_of(p)
            named = cache[p] & stems.keys()
            theirs = game_names_of(p) if named and p in game else set()
            hit = sorted(q for s in named for q in stems[s]
                         if q != p and not (s in theirs and (top(q), s) not in game_stems))
            if hit:
                found.append((p, hit))
        if not found:
            return left, why
        for p, hit in found:
            kept.discard(p)
            left.append(p)
            why[p] = hit
            if p not in game:
                missing.add(p)


def note_line(name, left):
    sets = sorted({stem(p) for p in left})
    shown = ", ".join(sets[:4]) + (f" and {len(sets) - 4} more" if len(sets) > 4 else "")
    return f"{name}: the game's own {shown} used: the mod's need a file it does not have."


def _names_reader(romfs, game_romfs):
    from .zs import Dicts, ZSTD_MAGIC
    dicts = []

    def names_of(rel):
        path = os.path.join(romfs, rel)
        try:
            if os.path.getsize(path) > _BIG:
                return set()
            with open(path, "rb") as f:
                raw = f.read()
            if raw[:4] == ZSTD_MAGIC:
                if not dicts:
                    dicts.append(Dicts.from_romfs(game_romfs))
                raw = dicts[0].decompress(raw)
        except (OSError, ValueError):
            return set()
        return {m.decode() for m in _TOKEN.findall(raw)}

    return names_of


def _write_new(path, data):
    """A new file in place of path: a part made from another copy holds hard links to that copy's files, so writing
    into one would change the copy the kit keeps to undo with (mods/_<folder>.prev)."""
    tmp = path + ".coop-new"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def _raise_sizes(mod_romfs, game_romfs, left, say):
    """Each left-out file's entry in the part's size table at least the game's own (the game's file is used now)."""
    res = os.path.join(mod_romfs, "System", "Resource")
    tables = sorted(n for n in os.listdir(res) if n.endswith(".rsizetable.zs")) if os.path.isdir(res) else []
    if not tables:
        return
    from .merge_tables import Codec
    c = Codec(game_romfs)
    for n in tables:
        theirs = os.path.join(game_romfs, "System", "Resource", n)
        if not os.path.isfile(theirs):
            continue
        with open(os.path.join(res, n), "rb") as f:
            raw = f.read()
        mine = c.load(raw)
        with open(theirs, "rb") as f:
            game = c.load(f.read())
        raised = 0
        for p in left:
            g = game.get(p.removesuffix(".zs"))
            if g is not None and (mine.get(p.removesuffix(".zs")) or 0) < g:
                mine.set(p.removesuffix(".zs"), g)
                raised += 1
        if raised:
            _write_new(os.path.join(res, n), c.save(mine, raw))
            say(f"part check: {n}: {raised} entries raised to the game's own")


def cmd_list(mod, recipes):
    have = os.path.join(mod, RELEASE_LIST)
    paths = files_under(os.path.join(mod, "romfs"))
    if recipes:
        with open(recipes, encoding="utf-8") as f:
            paths |= {e["path"].replace("\\", "/") for e in json.load(f).get("files", []) if e.get("path")}
    if os.path.isfile(have):  # (a part made from another copy: what that copy's release had)
        with open(have, encoding="utf-8") as f:
            paths |= {l.strip() for l in f if l.strip()}
    _write_new(have, "".join(p + "\n" for p in sorted(paths)).encode("utf-8"))
    print(f"part check: {len(paths)} release files listed")


def cmd_check(mod, game_romfs, name, say=print, guarded=None):
    mod_romfs = os.path.join(mod, "romfs")
    note = os.path.join(mod, NOTE)
    lst = os.path.join(mod, RELEASE_LIST)
    if not os.path.isfile(lst):
        say(f"part check: no {RELEASE_LIST} (nothing left out to check against)")
        return 0
    with open(lst, encoding="utf-8") as f:
        released = {l.strip() for l in f if l.strip()}
    kept = files_under(mod_romfs)
    game = files_under(game_romfs)
    if not game:
        say(f"part check: the game's RomFS {game_romfs} has no files")
        return 2
    left, why = plan(released, kept, game, _names_reader(mod_romfs, game_romfs),
                      _names_reader(game_romfs, game_romfs), guarded)
    # (a part made from a copy this check left files out of: those stay out, and the line still names them)
    gone = os.path.join(mod, LEFT_LIST)
    before = set()
    if os.path.isfile(gone):
        with open(gone, encoding="utf-8") as f:
            before = {l.strip() for l in f if l.strip()} - kept
    for f in (note, gone):
        if os.path.isfile(f):
            os.remove(f)
    if not left and not before:
        say("part check: every file the part names is in the part or the game")
        return 0
    for p in left:
        say(f"part check: {p} left out (the game's own used): it names {', '.join(why[p][:3])}, "
            "which neither the part nor the game has")
        os.remove(os.path.join(mod_romfs, p))
    _raise_sizes(mod_romfs, game_romfs, left, say)
    every = sorted(set(left) | before)
    _write_new(gone, "".join(p + "\n" for p in every).encode("utf-8"))
    line = note_line(name, every)
    _write_new(note, (line + "\n").encode("utf-8"))
    say(f"part check: {line}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("what", choices=("list", "check"))
    ap.add_argument("--mod", required=True)
    ap.add_argument("--recipes")
    ap.add_argument("--romfs")
    ap.add_argument("--name", default="A mod")
    ap.add_argument("--guarded", help="a regular expression of RomFS paths the module guards (not counted missing)")
    a = ap.parse_args(argv)
    try:
        if a.what == "list":
            cmd_list(a.mod, a.recipes)
            return 0
        if not a.romfs:
            print("part check: --romfs is needed")
            return 2
        return cmd_check(a.mod, a.romfs, a.name, guarded=a.guarded)
    except (OSError, ValueError, KeyError) as e:
        print(f"part check: could not check the part ({e})")
        return 2


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from puppet_pack import part_check as _self  # (relative imports: run as the package's module)
    sys.exit(_self.main())
