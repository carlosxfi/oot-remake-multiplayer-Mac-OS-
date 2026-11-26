"""One rc run on both test lanes: split the rc's group files into one group file per lane, balanced by minutes.

    python -I tools/autotest/rc_plan.py split <groups.txt...> --out-dir DIR [--lanes 2] [--history RUN_ROOT...]
    python -I tools/autotest/rc_plan.py remaining <lane groups.txt> <run root> --out FILE

Why (reports/test-speed-c127.md, 2026-11-26): an rc's rows ran as ~35 separate queue jobs on lane 1 while lane 2 did 3 %
of the work. One run per lane, each a groups.py --lane N run of its own file, halves the game time; the offline
script offline/rc-two-lanes.next.ps1 runs the smoke tier first, then both lane files at once (commit guard).

split: every [group] of every file is one unit (its scenarios stay together and in order, as groups.py plays them).
A group name used in two files becomes <file stem>-<name> (groups.py refuses a name twice in one file). Units go to
the lane with the fewest minutes so far, longest first (LPT). Minutes of a scenario: the mean of its earlier runs
under --history (run.json 'scenario' + the first to last timestamp of steps.txt, + CLOSE_MIN), else DEFAULT_MIN.
Writes lane1.txt .. laneN.txt (groups.py files: each group's own lines, unchanged) and plan.json (lanes, groups,
minutes, the source of every estimate). Every written file is parsed again by groups.parse_groups before it is kept.

remaining: the lines of a lane file whose run did not leave a run folder under <run root> (a lane-2 run ended early,
VOID-COMMIT): a groups.py file of only those, so the other lane plays what is left, not the whole file again.
Only run.json files written after the lane file count: a reused run root's older runs (another rc) are not "done".
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import groups as G   # noqa: E402

# Measured 2026-11-26 over 54 boots (reports/test-speed-c127.md "Per boot"): a run is ~4 min of steps.txt time
# (boot ~25 s, load ~22 s, scenario 1.5-3.5 min) plus ~0.5 min to close the games and judge.
DEFAULT_MIN = 4.5
CLOSE_MIN = 0.5
TS = re.compile(r"^(\d\d):(\d\d):(\d\d)\b")


class Fail(Exception):
    pass


def blocks(text):
    """[(group name, [scenario lines as written])] in file order; comments and blank lines dropped."""
    out, cur = [], None
    for raw in text.lstrip("﻿").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^\[([A-Za-z0-9_-]+)\]$", line)
        if m:
            cur = (m.group(1), [])
            out.append(cur)
        elif cur is None:
            raise Fail("a scenario line before the first [group]: %r" % line)
        else:
            cur[1].append(line)
    return out


def scenario_of(line):
    return line.split(None, 1)[0]


def steps_minutes(path):
    """First to last timestamp of a steps.txt in minutes (across midnight too), or None."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            ts = [m for m in (TS.match(x) for x in f) if m]
    except OSError:
        return None
    if len(ts) < 2:
        return None
    sec = lambda m: int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
    return ((sec(ts[-1]) - sec(ts[0])) % 86400) / 60.0


def history(roots):
    """{scenario: [minutes of each earlier run]} from run.json + steps.txt under the run roots."""
    seen = {}
    for root in roots or []:
        for d, _, files in os.walk(root):
            if "run.json" not in files or "steps.txt" not in files:
                continue
            try:
                with open(os.path.join(d, "run.json"), encoding="utf-8") as f:
                    name = json.load(f).get("scenario")
            except (OSError, ValueError):
                continue
            m = steps_minutes(os.path.join(d, "steps.txt"))
            if name and m is not None:
                seen.setdefault(name, []).append(m + CLOSE_MIN)
    return seen


def estimate(scenario, hist):
    runs = hist.get(scenario)
    if runs:
        return sum(runs) / len(runs), "mean of %d earlier run(s)" % len(runs)
    return DEFAULT_MIN, "default %.1f min (no earlier run)" % DEFAULT_MIN


def units(files):
    """[(unique group name, lines, source file)] of all files, every file checked by groups.parse_groups first."""
    out, names = [], set()
    for path in files:
        with open(path, encoding="utf-8-sig") as f:
            text = f.read()
        try:
            G.parse_groups(text)
        except G.Fail as e:
            raise Fail("%s: %s" % (path, e))
        stem = re.sub(r"[^A-Za-z0-9_-]", "-", os.path.splitext(os.path.basename(path))[0])
        for name, lines in blocks(text):
            unique = name if name not in names else "%s-%s" % (stem, name)
            if unique in names:
                raise Fail("%s: group [%s] cannot be made unique" % (path, name))
            names.add(unique)
            out.append((unique, lines, path))
    if not out:
        raise Fail("no group in %s" % ", ".join(files))
    return out


def assign(us, hist, lanes):
    """LPT: longest unit first, to the lane with the fewest minutes (ties: the lower lane). Keeps file order inside a
    lane. Returns [{lane, minutes, groups: [{name, lines, source, minutes, estimates}]}]."""
    rated = []
    for i, (name, lines, src) in enumerate(us):
        est = [estimate(scenario_of(x), hist) for x in lines]
        rated.append({"order": i, "name": name, "lines": lines, "source": src,
                      "minutes": round(sum(m for m, _ in est), 2),
                      "estimates": [{"scenario": scenario_of(x), "minutes": round(m, 2), "from": why}
                                    for x, (m, why) in zip(lines, est)]})
    plan = [{"lane": n, "minutes": 0.0, "groups": []} for n in range(1, lanes + 1)]
    for u in sorted(rated, key=lambda u: (-u["minutes"], u["order"])):
        best = min(plan, key=lambda p: (p["minutes"], p["lane"]))
        best["groups"].append(u)
        best["minutes"] = round(best["minutes"] + u["minutes"], 2)
    for p in plan:
        p["groups"].sort(key=lambda u: u["order"])
    return plan


def group_text(groups, header):
    lines = ["# " + h for h in header]
    for u in groups:
        lines += ["", "[%s]" % u["name"]] + u["lines"]
    return "\n".join(lines) + "\n"


def write_checked(path, text):
    G.parse_groups(text)   # refuses what groups.py would refuse, before the file exists
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def split(files, out_dir, lanes=2, roots=None):
    if lanes < 1:
        raise Fail("--lanes must be at least 1")
    plan = assign(units(files), history(roots), lanes)
    os.makedirs(out_dir, exist_ok=True)
    for p in plan:
        p["file"] = os.path.join(out_dir, "lane%d.txt" % p["lane"])
        if p["groups"]:
            write_checked(p["file"], group_text(p["groups"], [
                "rc_plan.py split, lane %d of %d: %d group(s), ~%.1f min (estimates in plan.json)"
                % (p["lane"], lanes, len(p["groups"]), p["minutes"])]))
        else:
            p["file"] = ""   # more lanes than groups: this lane plays nothing
    total = round(sum(p["minutes"] for p in plan), 2)
    doc = {"sources": list(files), "history": list(roots or []), "lanes": plan, "total_minutes": total,
           "longest_lane_minutes": max(p["minutes"] for p in plan)}
    with open(os.path.join(out_dir, "plan.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
    return doc


def done_runs(run_root, since=None):
    """{(group, scenario): count} of the runs that left a run folder (run.json) under run_root, written at or after
    since (epoch seconds; None = all)."""
    done = {}
    for d, _, files in os.walk(run_root):
        if "run.json" in files:
            pj = os.path.join(d, "run.json")
            try:
                if since is not None and os.path.getmtime(pj) < since:
                    continue
                with open(pj, encoding="utf-8") as f:
                    j = json.load(f)
            except (OSError, ValueError):
                continue
            k = (j.get("group", ""), j.get("scenario", ""))
            done[k] = done.get(k, 0) + 1
    return done


def remaining(lane_file, run_root):
    """groups.py text of the runs of lane_file with no run folder under run_root (or None: nothing is left)."""
    with open(lane_file, encoding="utf-8-sig") as f:
        bl = blocks(f.read())
    # the lane file is written by split before its runs start: older run.json files are another pass's runs
    done = done_runs(run_root, os.path.getmtime(lane_file)) if os.path.isdir(run_root) else {}
    left = []
    for name, lines in bl:
        keep = []
        for x in lines:
            k = (name, scenario_of(x))
            if done.get(k, 0) > 0:
                done[k] -= 1
            else:
                keep.append(x)
        if keep:
            left.append({"name": name, "lines": keep})
    if not left:
        return None
    return group_text(left, ["rc_plan.py remaining: the runs of %s with no run folder under %s" % (lane_file, run_root)])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("split")
    s.add_argument("groups", nargs="+")
    s.add_argument("--out-dir", required=True)
    s.add_argument("--lanes", type=int, default=2)
    s.add_argument("--history", nargs="*", default=[], help="run roots of earlier runs (minutes per scenario)")
    r = sub.add_parser("remaining")
    r.add_argument("lane_file")
    r.add_argument("run_root")
    r.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "split":
            doc = split(a.groups, a.out_dir, a.lanes, a.history)
            for p in doc["lanes"]:
                print("lane %d: %d group(s), ~%.1f min, %s" % (p["lane"], len(p["groups"]), p["minutes"],
                                                              p["file"] or "(nothing)"))
            print("total ~%.1f min, longest lane ~%.1f min" % (doc["total_minutes"], doc["longest_lane_minutes"]))
            return 0
        text = remaining(a.lane_file, a.run_root)
        if text is None:
            print("remaining: nothing left")
            if os.path.exists(a.out):
                os.remove(a.out)
            return 0
        write_checked(a.out, text)
        print("remaining: %s" % a.out)
        return 0
    except (Fail, G.Fail) as e:
        print("rc_plan.py: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
