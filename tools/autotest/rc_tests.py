"""One rc pass of the in-game bridge tests (docs/ingame-tests.md) over both test lanes: plan, then one summary.

    python -I tools/autotest/rc_tests.py plan --collect FILE [--smoke FILE] --out-dir DIR [--lanes 1,2]
                                              [--history ROOT...] [--skip-proven ROOT... --identity KEY...]
    python -I tools/autotest/rc_tests.py summary <lane out dir...> [--plan DIR/plan.json] --out DIR

B #129 (2026-11-26, owner: everything uses the bridge test system): the rc rows are the pytest bridge tests, not .ps1
group lines. One pytest process per lane (`pytest tools/autotest/ingame --lanes L --plan DIR/plan.json --out O/laneL
--expect-commit SHA`, both at once: one game-pair boot per lane, many tests), then ONE summary for the rc.
Interface agreed with test-bridge-c127-3 (08:3x UTC): bt_plugin --plan keeps the lane's nodeids plus every smoke test
and deselects the rest, matching on BASE nodeids (proven.base_nodeid: no --repeat [rN]); the plan's skipped tests
become summary rows with result SKIP; summary.json holds "identity" (proven.identity_of of the lane's pair, taken at
boot and at session end) and each row a "test_hash" (proven.test_hash).

plan: --collect is `pytest tools/autotest/ingame --collect-only -q` (one nodeid per line; other lines are ignored),
--smoke the same with `-m smoke`. Smoke tests go to EVERY lane (each boot proves itself, smoke gate per lane); the
rest is balanced by measured seconds (LPT): the median of a test's earlier PASS/FAIL/FLAKY rows in summary.json files
under --history (SETUP-FAIL/SKIP/NO-REF rows are cut short or not played: not timings), else the median of every
measured test, else DEFAULT_S. Writes DIR/plan.json {lanes: {"1": [nodeids], ...}, smoke, skipped, seconds}.

--skip-proven (B #128 rule, unchanged): a test is not played again when, on EVERY --identity key, a summary.json
with that identity key holds a row of it (same base nodeid, same test_hash as now) and every such row is PASS. A
FLAKY, FAIL, SETUP-FAIL or NO-REF row there, a summary without an identity key, or a test whose module is gone
proves nothing. A smoke test is never skipped (it is each boot's own proof). Each skipped test is listed in
plan.json "skipped" with its proof folders, and the rc summary lists it as SKIP.

summary: the lanes' summary.json files (a lane without one is a SETUP-FAIL row for that lane) and the plan's skipped
tests -> DIR/summary.json + summary.md. Verdict PASS only when at least one test passed and none is FAIL, FLAKY,
SETUP-FAIL or NO-REF.
"""
import argparse
import json
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import proven as P  # noqa: E402

DEFAULT_S = 120.0          # bt_plugin DEFAULT_BUDGET: a test's deadline when it names none
TIMED = ("PASS", "FAIL", "FLAKY")
NOT_PASS = ("FAIL", "FLAKY", "SETUP-FAIL", "NO-REF")


class Fail(Exception):
    pass


def read_ids(path):
    """Nodeids of a `pytest --collect-only -q` listing (lines with '::'), base ids, in order, once each."""
    if not path:
        return []
    out = []
    with open(path, encoding="utf-8-sig") as f:
        for line in f:
            line = line.strip()
            if "::" in line and not line.startswith(("=", "<")):
                b = P.base_nodeid(line.split(" ", 1)[0])
                if b not in out:
                    out.append(b)
    return out


def summaries(roots):
    """[(folder, summary.json dict)] under the roots."""
    out = []
    for root in roots or []:
        for d, _, files in os.walk(root):
            if "summary.json" in files:
                try:
                    with open(os.path.join(d, "summary.json"), encoding="utf-8") as f:
                        doc = json.load(f)
                except (OSError, ValueError):
                    continue
                if isinstance(doc, dict) and isinstance(doc.get("tests"), list):
                    out.append((d, doc))
    return out


def history(roots):
    """{base nodeid: [seconds]} of the timed rows."""
    h = {}
    for _, doc in summaries(roots):
        for r in doc["tests"]:
            if r.get("result") in TIMED and isinstance(r.get("seconds"), (int, float)) and r.get("test"):
                h.setdefault(P.base_nodeid(r["test"]), []).append(float(r["seconds"]))
    return h


def estimate(test, hist):
    if hist.get(test):
        return statistics.median(hist[test]), "measured (%d run(s))" % len(hist[test])
    every = [s for v in hist.values() for s in v]
    if every:
        return statistics.median(every), "median of every measured test"
    return DEFAULT_S, "default %d s" % DEFAULT_S


def proofs(test, keys, docs, repo=None):
    """(proven, [{folder, key, result}]): see the module text."""
    th = P.test_hash(test, repo)
    seen = {k: [] for k in keys}
    if not th:
        return False, []
    for folder, doc in docs:
        k = (doc.get("identity") or {}).get("key")
        if k not in seen:
            continue
        for r in doc["tests"]:
            if r.get("test") and P.base_nodeid(r["test"]) == test and r.get("test_hash") == th:
                seen[k].append({"folder": folder, "key": k, "result": r.get("result")})
    ok = bool(keys) and all(seen[k] and all(x["result"] == "PASS" for x in seen[k]) for k in keys)
    return ok, [x for k in keys for x in seen[k]]


def plan(tests, smoke, lanes, roots=None, proven_roots=None, keys=None, repo=None):
    if not lanes:
        raise Fail("--lanes: at least one lane")
    smoke = [t for t in smoke if t in tests] if smoke else []
    skipped = []
    rest = [t for t in tests if t not in smoke]
    if proven_roots:
        if not keys:
            raise Fail("--skip-proven needs --identity (each lane's build key: proven.py identity --lane N)")
        docs = summaries(proven_roots)
        keep = []
        for t in rest:
            ok, proof = proofs(t, keys, docs, repo)
            if ok:
                skipped.append({"test": t, "keys": list(keys), "proof": proof})
            else:
                keep.append(t)
        rest = keep
    hist = history(roots)
    est = {t: estimate(t, hist) for t in tests}
    smoke_s = round(sum(est[t][0] for t in smoke), 1)
    ln = {str(n): {"tests": [], "seconds": smoke_s} for n in lanes}
    order = {t: i for i, t in enumerate(tests)}
    for t in sorted(rest, key=lambda t: (-est[t][0], order[t])):
        best = min(lanes, key=lambda n: (ln[str(n)]["seconds"], n))
        ln[str(best)]["tests"].append(t)
        ln[str(best)]["seconds"] = round(ln[str(best)]["seconds"] + est[t][0], 1)
    for v in ln.values():
        v["tests"].sort(key=lambda t: order[t])
    return {"lanes": {k: smoke + v["tests"] for k, v in ln.items()},
            "lane_seconds": {k: v["seconds"] for k, v in ln.items()},
            "smoke": smoke, "skipped": skipped,
            "estimates": {t: {"seconds": round(est[t][0], 1), "from": est[t][1]} for t in tests},
            "history": list(roots or []), "skip_proven": {"roots": list(proven_roots or []), "keys": list(keys or [])},
            "longest_lane_minutes": round(max(v["seconds"] for v in ln.values()) / 60, 1)}


def merge(lane_dirs, plan_doc=None):
    """One rc summary from the lanes' summary.json (see the module text)."""
    rows, lanes = [], []
    for d in lane_dirs:
        p = os.path.join(d, "summary.json")
        try:
            with open(p, encoding="utf-8") as f:
                doc = json.load(f)
            if not isinstance(doc.get("tests"), list):
                raise ValueError("no tests list")
        except (OSError, ValueError, AttributeError) as e:
            rows.append({"test": "(lane %s)" % d, "result": "SETUP-FAIL", "lane": d,
                         "why": "no readable summary.json: %s" % e})
            lanes.append({"dir": d, "identity": None, "verdict": "SETUP-FAIL"})
            continue
        lanes.append({"dir": d, "identity": doc.get("identity"), "verdict": doc.get("verdict"),
                      "minutes": doc.get("minutes")})
        rows += [dict(r, lane=d) for r in doc["tests"]]
    played = {P.base_nodeid(r["test"]) for r in rows if r.get("test")}
    for s in (plan_doc or {}).get("skipped", []):
        if s["test"] not in played:   # (a lane's bt_plugin --plan already wrote it as SKIP)
            rows.append({"test": s["test"], "result": "SKIP", "lane": "",
                         "why": "proven: " + "; ".join("%s %s" % (x["result"], x["folder"]) for x in s["proof"])})
    counts = {k: sum(1 for r in rows if r.get("result") == k) for k in
              ("PASS", "FAIL", "FLAKY", "SETUP-FAIL", "NO-REF", "SKIP")}
    bad = sum(counts[k] for k in NOT_PASS) + sum(1 for r in rows if r.get("result") not in counts)
    return {"lanes": lanes, "counts": counts, "tests": rows,
            "minutes": max([x.get("minutes") or 0 for x in lanes] or [0]),
            "verdict": "PASS" if counts["PASS"] and not bad else "NOT PASS"}


def summary_md(doc):
    c = doc["counts"]
    out = ["# rc in-game tests: %s" % doc["verdict"], "",
           " ".join("%s %d" % (k, v) for k, v in c.items()) + ", ~%s min (longest lane)" % doc["minutes"], "",
           "| result | test | lane | why |", "|---|---|---|---|"]
    order = {"FAIL": 0, "SETUP-FAIL": 1, "FLAKY": 2, "NO-REF": 3, "PASS": 4, "SKIP": 5}
    for r in sorted(doc["tests"], key=lambda r: order.get(r.get("result"), -1)):
        out.append("| %s | %s | %s | %s |" % (r.get("result"), r.get("test"), os.path.basename(r.get("lane") or ""),
                                             (r.get("why") or "").replace("|", "/").replace("\n", " ")[:300]))
    return "\n".join(out) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--collect", required=True)
    p.add_argument("--smoke")
    p.add_argument("--out-dir", required=True)
    p.add_argument("--lanes", default="1,2")
    p.add_argument("--history", nargs="*", default=[])
    p.add_argument("--skip-proven", nargs="*", default=[])
    p.add_argument("--identity", nargs="*", default=[])
    s = sub.add_parser("summary")
    s.add_argument("lane_dirs", nargs="+")
    s.add_argument("--plan")
    s.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "plan":
            tests = read_ids(a.collect)
            if not tests:
                raise Fail("%s: no test nodeids (pytest --collect-only -q output?)" % a.collect)
            lanes = [int(x) for x in a.lanes.split(",") if x.strip()]
            doc = plan(tests, read_ids(a.smoke), lanes, a.history, a.skip_proven, a.identity)
            os.makedirs(a.out_dir, exist_ok=True)
            with open(os.path.join(a.out_dir, "plan.json"), "w", encoding="utf-8") as f:
                json.dump(doc, f, indent=1)
            for t in doc["skipped"]:
                print("skip %s (proven: %s)" % (t["test"], "; ".join("%s %s" % (x["result"], x["folder"])
                                                                    for x in t["proof"])))
            for k, v in doc["lanes"].items():
                print("lane %s: %d test(s) (%d smoke), ~%.1f min" % (k, len(v), len(doc["smoke"]),
                                                                    doc["lane_seconds"][k] / 60))
            print("longest lane ~%.1f min, %d skipped (proven on this build)" % (doc["longest_lane_minutes"],
                                                                                len(doc["skipped"])))
            return 0
        pd = None
        if a.plan:
            with open(a.plan, encoding="utf-8") as f:
                pd = json.load(f)
        doc = merge(a.lane_dirs, pd)
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, "summary.json"), "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
        with open(os.path.join(a.out, "summary.md"), "w", encoding="utf-8") as f:
            f.write(summary_md(doc))
        print("rc in-game tests: %s (%s)" % (doc["verdict"], ", ".join("%s %d" % kv for kv in doc["counts"].items())))
        return 0 if doc["verdict"] == "PASS" else 1
    except (Fail, OSError, ValueError) as e:
        print("rc_tests.py: %s" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
