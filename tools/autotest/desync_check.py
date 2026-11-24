"""Compares the two games' checkpoints of a live play-test session (tools/autotest/live.py checkpoint TAG).

Each checkpoint holds both games' shared dungeon and story flags (the module's `dump TAG`) and their bridge status.
A flag the two games disagree on is a desync: co-op shares every one of them. The checkpoints are read in the order
they were taken; the first one with a difference is named, with every flag that differs in it. Standard library only.

    python -I tools/autotest/desync_check.py SESSION_DIR [--all]

With --clients it reads a bridge run's log folder instead (mcp_playtest.py --player, one log per game, p1 to p4):
the checkpoints are matched by tag across every game (the second use of a tag goes with the other games' second),
each game's frame count at the tag is printed, and every game's flags are compared.

    python -I tools/autotest/desync_check.py LOG_DIR --clients [--all]

Exit status: 0 when every checkpoint was compared and agrees, 1 when one does not, 2 when there is nothing to compare
(no checkpoint, or none where both games wrote their flags), 3 when some agree but another could not be compared (a game
wrote no flags): never "agree" for a checkpoint that was not compared.
"""
import argparse
import glob
import json
import os
import sys


def compare(cp):
    """(differences, note): the flags whose values differ between A and B ({name: (a, b)}), and why not compared."""
    a, b = cp.get("A") or {}, cp.get("B") or {}
    fa, fb = a.get("flags"), b.get("flags")
    if fa is None or fb is None:
        return {}, "a game wrote no flags (%s)" % (a.get("error") or b.get("error") or "no dump")
    diff = {}
    for name in sorted(set(fa) | set(fb)):
        va, vb = fa.get(name), fb.get(name)
        if va is not None and vb is not None and va != vb:
            diff[name] = (va, vb)
    return diff, None


def check(session, show_all=False, out=print):
    paths = sorted(glob.glob(os.path.join(session, "checkpoints", "*.json")), key=os.path.getmtime)
    if not paths:
        out("no checkpoints in %s" % os.path.join(session, "checkpoints"))
        return 2
    bad = compared = skipped = 0
    for path in paths:
        with open(path, encoding="utf-8") as f:
            cp = json.load(f)
        tag = cp.get("tag") or os.path.splitext(os.path.basename(path))[0]
        maps = "A %s, B %s" % ((cp.get("A") or {}).get("status", {}).get("map"),
                               (cp.get("B") or {}).get("status", {}).get("map"))
        diff, note = compare(cp)
        if note:
            out("%s: not compared: %s" % (tag, note))
            skipped += 1
            continue
        compared += 1
        shots = [s for s in ((cp.get(x) or {}).get("shot") for x in ("A", "B")) if s]
        if shots:
            maps += "; shots " + ", ".join(shots)
        if not diff:
            out("%s: agree (%s)" % (tag, maps))
            continue
        bad += 1
        out("%s: %d flag(s) differ (%s)%s" % (tag, len(diff), maps, "" if bad > 1 else "  <- first desync"))
        for name, (va, vb) in diff.items():
            out("    %s: A %s, B %s" % (name, va, vb))
        if not show_all:
            break
    if bad:
        return 1
    if not compared:
        out("nothing compared: no checkpoint has both games' flags")
        return 2
    if skipped:
        out("incomplete: %d checkpoint(s) not compared" % skipped)
        return 3
    return 0


RESULTS = ["agree", "desync", "nothing to compare", "incomplete"]


def read_clients(log_dir):
    """{player: [checkpoint, ...]} from each game's checkpoint log (<player>.checkpoints.jsonl, one JSON per line,
    written by playtest_client.checkpoint), each game's in frame order. A line that is not JSON is skipped."""
    out = {}
    for path in sorted(glob.glob(os.path.join(log_dir, "*.checkpoints.jsonl"))):
        player = os.path.basename(path).split(".")[0]
        rows = []
        with open(path, encoding="utf-8") as f:
            for line in f:
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    continue
        rows.sort(key=lambda r: (r.get("frame") is None, r.get("frame") or 0))
        out[player] = rows
    return out


def align(clients):
    """[(key, {player: checkpoint})]: the n-th checkpoint of a tag in each game goes with the n-th of that tag in
    every other game (a tag taken twice is two keys, TAG and TAG#2), in the order of the first game's frames."""
    keyed, order = {}, []
    for player in sorted(clients):
        seen = {}
        for cp in clients[player]:
            tag = cp.get("tag")
            seen[tag] = seen.get(tag, 0) + 1
            key = tag if seen[tag] == 1 else "%s#%d" % (tag, seen[tag])
            if key not in keyed:
                keyed[key] = {}
                order.append(key)
            keyed[key][player] = cp
    return [(k, keyed[k]) for k in order]


def compare_many(cps):
    """(differences, note) for one tag across games: {flag: {player: value}} where the games disagree."""
    missing = [p for p, cp in sorted(cps.items()) if cp.get("flags") is None]
    if missing:
        return {}, "%s wrote no flags (%s)" % (", ".join(missing), cps[missing[0]].get("error") or "no dump")
    diff = {}
    names = set()
    for cp in cps.values():
        names |= set(cp["flags"])
    for name in sorted(names):
        vals = {p: cp["flags"].get(name) for p, cp in sorted(cps.items())}
        known = {v for v in vals.values() if v is not None}
        if len(known) > 1:
            diff[name] = vals
    return diff, None


def check_clients(log_dir, show_all=False, out=print):
    """Like check, for the per-game logs of a bridge run (mcp_playtest.py --player): the tags are matched across
    every game that wrote a log, and each game's frame at the tag is printed. Same exit codes."""
    clients = read_clients(log_dir)
    if len(clients) < 2:
        out("nothing to compare: %d game log(s) in %s (need two or more *.checkpoints.jsonl)" % (len(clients), log_dir))
        return 2
    bad = compared = skipped = 0
    for key, cps in align(clients):
        frames = ", ".join("%s frame %s %s" % (p, cp.get("frame"), cp.get("scene") or "") for p, cp in sorted(cps.items()))
        absent = sorted(set(clients) - set(cps))
        if absent:
            out("%s: not compared: no checkpoint in %s (%s)" % (key, ", ".join(absent), frames))
            skipped += 1
            continue
        diff, note = compare_many(cps)
        if note:
            out("%s: not compared: %s" % (key, note))
            skipped += 1
            continue
        compared += 1
        if not diff:
            out("%s: agree (%s)" % (key, frames))
            continue
        bad += 1
        out("%s: %d flag(s) differ (%s)%s" % (key, len(diff), frames, "" if bad > 1 else "  <- first desync"))
        for name, vals in diff.items():
            out("    %s: %s" % (name, ", ".join("%s %s" % (p, v) for p, v in vals.items())))
        if not show_all:
            break
    if bad:
        return 1
    if not compared:
        out("nothing compared: no tag has every game's flags")
        return 2
    if skipped:
        out("incomplete: %d tag(s) not compared" % skipped)
        return 3
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("session", help="a live.py session folder, or with --clients the bridge run's log folder")
    p.add_argument("--all", action="store_true", help="every checkpoint with a difference, not only the first")
    p.add_argument("--clients", action="store_true",
                   help="per-game logs (<player>.checkpoints.jsonl from mcp_playtest.py --player), matched by tag")
    a = p.parse_args(argv)
    return check_clients(a.session, a.all) if a.clients else check(a.session, a.all)


if __name__ == "__main__":
    sys.exit(main())
