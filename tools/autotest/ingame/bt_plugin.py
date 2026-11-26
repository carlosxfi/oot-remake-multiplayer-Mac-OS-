"""pytest plugin of the in-game test system (docs/ingame-tests.md): lanes, one game-pair boot per lane, results.

    python -I -m pytest tools/autotest/ingame --lanes 1 --out DIR            (one lane)
    python -I -m pytest tools/autotest/ingame -n 2 --lanes 1,2 --out DIR     (both lanes at once: xdist worker N -> lane N+1)
    ... --fake                                                               (no game: fake bridges, for the selftests)
    ... --repeat 10 --no-smoke-gate                                          (the pilot: everything 10 times, one boot)

Results (one summary per run, DIR/summary.json + summary.md, and pytest's --junitxml if asked):
  PASS        every assert held
  FAIL        an assert failed or the test ran past its budget ('timeout'); rerun once: FAIL again stays FAIL
  FLAKY       failed, then passed on the one rerun (never counted as a pass)
  SETUP-FAIL  the lane, the boot or the test's own set-up (Pair.setup(), < 30 s) did not hold; not rerun
  NO-REF      a look check has no stored reference yet (its shot is kept under DIR/new/); not a pass
Smoke gate: a smoke test (marker `smoke`) that FAILs or SETUP-FAILs stops the run there (the rc is broken), unless
--no-smoke-gate.
"""
import json
import os
import subprocess
import sys
import time

import pytest
from _pytest.runner import runtestprotocol

HERE = os.path.dirname(os.path.abspath(__file__))
AUTOTEST = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, AUTOTEST)
import pair as P   # noqa: E402

RESULTS = ("PASS", "FAIL", "FLAKY", "SETUP-FAIL", "NO-REF", "SKIP")
DEFAULT_BUDGET = 120


def pytest_addoption(parser):
    g = parser.getgroup("ingame")
    g.addoption("--lanes", default="1", help="test lanes, comma separated; xdist worker N takes the Nth")
    g.addoption("--out", default=None, help="results folder (summary.json, summary.md, shots)")
    g.addoption("--fake", action="store_true", help="fake games (fakegame.py): no game, for the selftests")
    g.addoption("--repeat", type=int, default=1, help="run every test N times on the same boot (the pilot: 10)")
    g.addoption("--no-smoke-gate", action="store_true", help="go on after a smoke test that did not pass")
    g.addoption("--no-rerun", action="store_true", help="no FLAKY rerun of a failed test")
    g.addoption("--kit-ls", default=None, help="the lanes folder (default: lanes.py's kit root + ls)")
    g.addoption("--expect-commit", default=None, help="SETUP-FAIL unless the lane's lane-server.txt commit starts so")
    g.addoption("--session-minutes", type=float, default=240, help="live.py --max-minutes of each lane's session")


def pytest_configure(config):
    config.addinivalue_line("markers", "smoke: the rc smoke tier (runs first; a miss stops the run)")
    config.addinivalue_line("markers", "budget(seconds): the test's own hard deadline (default %d)" % DEFAULT_BUDGET)
    config.addinivalue_line("markers", "row(text): the rc row the test proves (in the summary)")
    config.addinivalue_line("markers", "look: a user-facing look check (bridge shots on each game, compared to refs)")
    config._bt_rows = []
    config._bt_started = time.time()


# -- repeat and order: smoke first, then rows; repeat r of everything before repeat r+1 ------------------------
@pytest.fixture(autouse=True)
def bt_rep(request):
    return getattr(request, "param", 0)


def pytest_generate_tests(metafunc):
    n = metafunc.config.getoption("repeat")
    if n > 1 and "bt_rep" in metafunc.fixturenames:
        metafunc.parametrize("bt_rep", range(n), ids=lambda i: "r%d" % (i + 1))


def _rep_of(item):
    cs = getattr(item, "callspec", None)
    return cs.params.get("bt_rep", 0) if cs else 0


def pytest_collection_modifyitems(config, items):
    order = {id(it): i for i, it in enumerate(items)}
    items.sort(key=lambda it: (_rep_of(it), 0 if it.get_closest_marker("smoke") else 1, order[id(it)]))


# -- the lane: one boot for every test this process runs -------------------------------------------------------
def _lane_for(config):
    lanes = [int(x) for x in str(config.getoption("lanes")).split(",") if x.strip()]
    wid = getattr(config, "workerinput", {}).get("workerid", "gw0")
    n = int(wid[2:]) if wid.startswith("gw") else 0
    if n >= len(lanes):
        raise P.SetupFail("xdist worker %s has no lane (--lanes %s): give one lane per worker" % (wid, config.getoption("lanes")))
    return lanes[n]


def _out_dir(config, lane):
    out = config.getoption("out") or os.path.join(os.getcwd(), "ingame-out")
    d = os.path.join(out, "lane%d" % lane)
    os.makedirs(d, exist_ok=True)
    return d


def _live(args, timeout):
    """tools/autotest/live.py as its own process (it prints one JSON answer)."""
    cmd = [sys.executable, "-I", os.path.join(AUTOTEST, "live.py")] + args
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    try:
        return json.loads(p.stdout)
    except ValueError:
        return {"ok": False, "error": "live.py %s: exit %s: %s" % (args[0], p.returncode, (p.stderr or p.stdout)[-600:])}


@pytest.fixture(scope="session")
def lane_pair(request):
    config = request.config
    lane = _lane_for(config)
    out = _out_dir(config, lane)
    refs = os.path.join(HERE, "refs")
    if config.getoption("fake"):
        import fakegame
        lanefake = fakegame.FakeLane(out)
        pair = P.Pair(lanefake.games(), out_dir=out, refs_dir=os.environ.get("BT_FAKE_REFS", refs))
        pair.lane, pair.info = lane, {"fake": True}
        yield pair
        pair.close()
        lanefake.close()
        return
    import lanes as L
    kit_ls = config.getoption("kit_ls") or os.path.join(L.kit_root(), "ls")
    t0 = time.monotonic()
    info = P.lane_check(kit_ls, lane)                       # (SetupFail: every test of this lane is SETUP-FAIL)
    want = config.getoption("expect_commit")
    if want and not info["commit"].startswith(want):
        raise P.SetupFail("lane%d runs commit %s, not %s" % (lane, info["commit"], want))
    session = os.path.join(out, "session")
    started = _live(["start", "--session", session, "--lane", str(lane), "--playtest", "--server", info["server"],
                     "--env", "LICHII_COOP_PUPPET=1", "--env", "LICHII_COOP_TEST_TO_TITLE=1",
                     "--max-minutes", str(config.getoption("session_minutes")), "--idle-minutes", "15"], timeout=1200)
    if not started.get("ok"):
        raise P.SetupFail("lane%d boot: %s" % (lane, started.get("error") or started.get("boot")))
    try:
        pair = P.Pair(P.read_session(session), out_dir=out, refs_dir=refs)
        pair.lane, pair.info, pair.session = lane, dict(info, boot_s=round(time.monotonic() - t0, 1)), session
        for x in ("A", "B"):
            h = pair.c[x].hello()
            if not h.get("control"):
                raise P.SetupFail("lane%d %s: the bridge has no Longshot control (HOS_CONTROL)" % (lane, x))
        yield pair
        pair.close()
    finally:
        _live(["stop", "--session", session], timeout=180)


@pytest.fixture
def pair(request, lane_pair):
    m = request.node.get_closest_marker("budget")
    lane_pair.begin(m.args[0] if m else DEFAULT_BUDGET)
    if getattr(lane_pair, "session", None):
        import live
        live.touch(lane_pair.session)          # (the session's watchdog counts it as a tester's command)
    yield lane_pair
    request.node.user_properties.append(("shots", list(lane_pair.shots)))
    request.node.user_properties.append(("notes", list(lane_pair.notes)))
    missing = lane_pair.no_ref()
    if missing:
        pytest.fail("NO-REF: no stored reference for %s (candidates under new/)" % ", ".join(missing), pytrace=False)


# -- results ---------------------------------------------------------------------------------------------------
def _kind(reports):
    """One run's result from its setup / call / teardown reports."""
    for r in reports:
        if r.when == "setup" and r.failed:
            return "SETUP-FAIL"
        if r.when == "setup" and r.skipped:
            return "SKIP"
    for r in reports:
        if r.when == "call" and r.failed:
            text = str(r.longrepr)
            if "SetupFail" in text:
                return "SETUP-FAIL"
            if "NO-REF" in text and "AssertionError" in text:
                return "NO-REF"
            return "FAIL"
        if r.when == "call" and r.skipped:
            return "SKIP"
    for r in reports:
        if r.when == "teardown" and r.failed and "NO-REF" in str(r.longrepr):
            return "NO-REF"
    if any(r.failed for r in reports):
        return "FAIL"
    return "PASS"


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_protocol(item, nextitem):
    item.ihook.pytest_runtest_logstart(nodeid=item.nodeid, location=item.location)
    t0 = time.monotonic()
    reports = runtestprotocol(item, nextitem=nextitem, log=False)
    kind, first = _kind(reports), None
    if kind == "FAIL" and not item.config.getoption("no_rerun"):
        first = reports
        reports = runtestprotocol(item, nextitem=nextitem, log=False)
        kind = "FLAKY" if _kind(reports) == "PASS" else "FAIL"
        if kind == "FLAKY":
            reports = first          # (the failure is what the summary keeps: a FLAKY is never shown as a pass)
    secs = round(time.monotonic() - t0, 1)
    why = ""
    for r in reports:
        if r.failed:
            crash = getattr(r.longrepr, "reprcrash", None)
            why = " ".join((crash.message if crash else str(r.longrepr)).split())[:400]
            break
    for r in reports:
        r.user_properties.append(("result", kind))
        r.user_properties.append(("seconds", secs))
        r.user_properties.append(("why", why))
        item.ihook.pytest_runtest_logreport(report=r)
    item.ihook.pytest_runtest_logfinish(nodeid=item.nodeid, location=item.location)
    if item.get_closest_marker("smoke") and kind in ("FAIL", "SETUP-FAIL") and not item.config.getoption("no_smoke_gate"):
        item.session.shouldstop = "smoke %s %s: the rc stops here" % (item.name, kind)
    return True


def pytest_runtest_logreport(report):
    if report.when != "teardown":
        return
    props = dict(report.user_properties)
    if "result" not in props:
        return
    cfg = _CONFIG.get("c")
    if cfg is not None:
        cfg._bt_rows.append({"test": report.nodeid, "result": props["result"], "seconds": props.get("seconds"),
                             "why": props.get("why", ""), "shots": props.get("shots", []),
                             "worker": getattr(report, "node", None) and getattr(report.node, "gateway", None)
                             and report.node.gateway.id})


_CONFIG = {}


def pytest_sessionstart(session):
    _CONFIG["c"] = session.config


def pytest_sessionfinish(session, exitstatus):
    config = session.config
    if hasattr(config, "workerinput"):
        return     # (xdist: the controller writes the one summary)
    rows = config._bt_rows
    counts = {k: sum(1 for r in rows if r["result"] == k) for k in RESULTS}
    out = config.getoption("out") or os.path.join(os.getcwd(), "ingame-out")
    os.makedirs(out, exist_ok=True)
    summary = {"started": config._bt_started, "minutes": round((time.time() - config._bt_started) / 60, 2),
               "lanes": config.getoption("lanes"), "fake": bool(config.getoption("fake")),
               "repeat": config.getoption("repeat"), "counts": counts, "tests": rows,
               "verdict": "PASS" if rows and counts["PASS"] == len(rows) else "NOT PASS"}
    with open(os.path.join(out, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    lines = ["# In-game tests: %s" % summary["verdict"], "",
             "%s; %.1f min; lanes %s%s" % (", ".join("%s %d" % (k, v) for k, v in counts.items() if v),
                                         summary["minutes"], summary["lanes"], " (fake games)" if summary["fake"] else ""),
             "", "| Result | Test | s | Why |", "|---|---|---|---|"]
    for r in rows:
        lines.append("| %s | %s | %s | %s |" % (r["result"], r["test"], r["seconds"], r["why"].replace("|", "/")))
    with open(os.path.join(out, "summary.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
