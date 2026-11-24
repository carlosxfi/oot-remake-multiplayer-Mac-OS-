"""A live two-game play-test session: start the server and both games once, then act and look one step at a time.

The two-game runner (hookshot.py run) plays a whole script blind. This keeps the games open after their boot script
(`run --keep-open`) and lets a tester (a person, or Claude through tools/autotest/mcp_playtest.py) take one step at a
time and see the result before the next: the step's own script lines (hookshot.py's test script format, `;` between
lines), then what each game says now as JSON (the control bridge's `status` snapshot: map, Link's place, facing, life,
event, loading, prompt) and the new co-op log lines since the last look. A checkpoint writes each game's shared flags
(the module's `dump TAG` test command) and a screenshot of each game beside the session;
tools/autotest/desync_check.py compares them.

The games run in real time between steps (the bridge has no frame step): a step's lines are what the game does, and
a long think between steps is time the game goes on (Link stands still, enemies do not). Standard library only.

A session on a test lane (`--lane N`, tools/autotest/lanes.py) uses that lane's game pair, port and control pipes and
holds its lock (laneN.lock) for the whole session, not only the boot. Every session has a watchdog (a process of its
own, `live.py watchdog`, started by `start`): it holds the lane lock under its own pid, and at the session's end (`stop`,
the maximum duration, a tester idle too long, a game that ended, or the watchdog itself stopped by a signal) it quits
both games, kills what did not quit and the server, and lets the lock go. A watchdog killed outright cannot clean up,
but its lock goes stale with it (the lock names its pid). Each game's save is restored to its baseline before the
boot, as the lane runs do (`--no-restore` keeps whatever the copies hold).

    python -I tools/autotest/live.py start --session DIR --server coop_server.exe (--lane N | --game-a A --game-b B)
                                           [--boot-a boot.A.txt --boot-b boot.B.txt] [--env K=V ...]
                                           [--max-minutes 30] [--idle-minutes 10] [--lock-wait S] [--no-restore]
    python -I tools/autotest/live.py do --session DIR A "goto 10.0 20.0 0.5 300; press A"
    python -I tools/autotest/live.py state --session DIR [A|B]
    python -I tools/autotest/live.py shot --session DIR [A|B]
    python -I tools/autotest/live.py checkpoint --session DIR TAG
    python -I tools/autotest/live.py stop --session DIR
"""
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hookshot  # noqa: E402  (the same folder)
import lanes  # noqa: E402  (the same folder; it brings tools/hookshot/hs.py)

TAGS = ("A", "B")
PIPE = "coop-live"   # without a lane
PORT = 27593         # without a lane
# Log lines a tester never needs (renderer, GPU, controller and frame-time chatter).
NOISE = re.compile(r"\] (vk:|nvn:|hid:|vi:|vcores:|barrier:|\[debug\])")
MAX_LINES = 80
MAX_MINUTES = 30     # a session's whole length (the lane lock goes stale after 4 h anyway; a slot is shorter)
IDLE_MINUTES = 10    # no tester command for this long after the boot: the tester is gone (hand-off, crash, pause)
WATCH_EVERY = 5.0    # s between the watchdog's looks
QUIT_WAIT = 15.0     # s a game gets to quit before it is killed


def session_file(d):
    return os.path.join(d, "session.json")


def watchdog_file(d):
    return os.path.join(d, "watchdog.json")


def stop_file(d):
    return os.path.join(d, "stop-request")


def active_file(d):
    return os.path.join(d, "active")


def read_json(path, default=None):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def write_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1)
    for i in range(20):   # (Windows: a reader holding the old file a moment refuses the replace)
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if i == 19:
                raise
            time.sleep(0.05)


def load(d):
    with open(session_file(d), encoding="utf-8") as f:
        return json.load(f)


def save(d, s):
    write_json(session_file(d), s)


def lock_owner(d):
    return "live.py session %s" % d


def touch(d):
    """A tester command was given now (the watchdog's idle clock)."""
    with open(active_file(d), "a", encoding="utf-8"):
        pass
    os.utime(active_file(d), None)


def activity(d, s):
    try:
        return os.path.getmtime(active_file(d))
    except OSError:
        return s.get("started", 0)


def game_pids(d):
    """({tag: pid}, server pid) from the runner's pids.json (hookshot.py run --keep-open)."""
    p = read_json(os.path.join(d, "pids.json"), {}) or {}
    return {x: int(v) for x, v in (p.get("games") or {}).items() if v}, (int(p["server"]) if p.get("server") else None)


def ended(d):
    """Why the session ended ('' while it runs)."""
    return (read_json(watchdog_file(d), {}) or {}).get("ended") or ""


def open_session(d):
    """The session for a tester command: refused once it ended (the games are closed and the lane is free)."""
    s = load(d)
    why = ended(d)
    if why:
        raise SystemExit("the session ended (%s): start a new one" % why)
    touch(d)
    return s


def resolve(a):
    """(game A folder, game B folder, port, pipe): the lane's (tools/autotest/lanes.py) unless given."""
    if a.lane:
        L = lanes.lane(a.lane)
        return (a.game_a or L["a"]), (a.game_b or L["b"]), (a.port or L["port"]), L["pipe"]
    if not (a.game_a and a.game_b):
        raise SystemExit("--game-a and --game-b (or --lane N)")
    return a.game_a, a.game_b, (a.port or PORT), PIPE


def runner_argv(a, game_a, game_b, port, pipe, d):
    """hookshot.py run's arguments for the boot. No --lane: the watchdog holds the lane lock for the whole session (the
    runner would let it go when the boot ends, with the games still open)."""
    argv = ["run", "--game-a", game_a, "--game-b", game_b, "--server", a.server, "--port", str(port),
            "--out", d, "--name", "live", "--pipe", pipe, "--keep-open", "--every", "0",
            "--scenario-timeout", str(a.boot_timeout)]
    if a.no_restore:
        argv.append("--no-restore")
    if a.boot_a:
        argv += ["--script-a", a.boot_a]
    if a.boot_b:
        argv += ["--script-b", a.boot_b]
    if a.no_load:
        argv.append("--no-load")
    elif a.load:
        argv += ["--load", a.load]
    if getattr(a, "playtest", False):
        argv += ["--playtest-ports", str(playtest_base(a.lane))]
    for kv in a.env:
        argv += ["--env", kv]
    if getattr(a, "display", None):
        argv += ["--display", a.display]
    return argv


def playtest_base(lane):
    """The play-test bridge's port base: game A (p1) listens on base + 1, B (p2) on base + 2
    (tools/autotest/playtest_client.py port_for: 5000 + N, or 5000 + 10*L + N on lane L)."""
    return 5000 + 10 * int(lane or 0)


def spawn_watchdog(d):
    kw = ({"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP | 0x00000008}   # DETACHED_PROCESS: outlives the shell
          if os.name == "nt" else {"start_new_session": True})
    with open(os.path.join(d, "watchdog.txt"), "ab") as log:
        return subprocess.Popen([sys.executable, "-I", os.path.abspath(__file__), "watchdog", "--session", d],
                                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, close_fds=True, **kw)


def wait_lock(d, proc, timeout, clock=time.monotonic, sleep=time.sleep):
    """The watchdog's word on the lock: 'held', 'none' (no lane), or why it could not start."""
    end = clock() + timeout
    while clock() < end:
        w = read_json(watchdog_file(d), {}) or {}
        if w.get("ended"):
            return w["ended"]
        if w.get("lock") in ("held", "none"):
            return w["lock"]
        if proc.poll() is not None:
            return "the watchdog ended (exit %s, see watchdog.txt)" % proc.returncode
        sleep(0.2)
    return "the watchdog did not answer in %d s" % timeout


def start(a):
    d = os.path.abspath(a.session)
    os.makedirs(d, exist_ok=True)
    w = read_json(watchdog_file(d), {}) or {}
    if w.get("pid") and not w.get("ended") and lanes.hs.pid_alive(int(w["pid"])):
        raise SystemExit("a session is still open in %s: stop it first" % d)
    game_a, game_b, port, pipe = resolve(a)
    for f in (watchdog_file(d), stop_file(d), os.path.join(d, "pids.json")):
        try:
            os.remove(f)
        except OSError:
            pass
    s = {"started": time.time(), "dir": d, "lane": a.lane, "port": port, "booted": False,
         "max_s": int(a.max_minutes * 60), "idle_s": int(a.idle_minutes * 60), "lock_wait": a.lock_wait,
         "games": {}, "offsets": {}}
    for i, (x, g) in enumerate(zip(TAGS, (game_a, game_b))):
        s["games"][x] = {"folder": os.path.abspath(g), "pipe": "%s-%s" % (pipe, x),
                         "commands": os.path.join(d, "commands-%s.txt" % x),
                         "log": os.path.join(d, "game-out-%s.txt" % x)}
        if getattr(a, "playtest", False):
            s["games"][x]["playtest_port"] = playtest_base(a.lane) + i + 1
    save(d, s)
    touch(d)
    lock = wait_lock(d, spawn_watchdog(d), a.lock_wait + 30)
    if lock not in ("held", "none"):
        return {"ok": False, "session": d, "lane": a.lane, "error": lock}
    code = hookshot.main(runner_argv(a, game_a, game_b, port, pipe, d))
    s = load(d)
    for x in TAGS:
        log = s["games"][x]["log"]
        s["offsets"][x] = os.path.getsize(log) if os.path.exists(log) else 0
    s["booted"] = True
    save(d, s)
    touch(d)
    out = {"ok": code == 0, "session": d, "lane": a.lane, "lock": lock, "port": port,
           "boot": "played" if code == 0 else "FAILED (see steps.txt): the session is closed",
           "ends": "after %d min, or %d min without a command" % (a.max_minutes, a.idle_minutes)}
    if getattr(a, "playtest", False):
        out["playtest"] = {"p%d" % (i + 1): s["games"][x]["playtest_port"] for i, x in enumerate(TAGS)}
    if code != 0:
        out["stop"] = stop(argparse.Namespace(session=d))
    return out


def bridge(s, x):
    return hookshot.Bridge.connect(s["games"][x]["pipe"], timeout=15)


def new_lines(d, s, x):
    """The game's co-op log lines since the last look (noise left out), at most MAX_LINES (the newest)."""
    path = s["games"][x]["log"]
    try:
        with open(path, "rb") as f:
            f.seek(s["offsets"].get(x, 0))
            data = f.read()
            s["offsets"][x] = f.tell()
    except OSError:
        return []
    lines = [ln for ln in data.decode("utf-8", "replace").splitlines() if "coop:" in ln and not NOISE.search(ln)]
    lines = [re.sub(r"^\[hos [^\]]*\] ", "", ln) for ln in lines]
    if len(lines) > MAX_LINES:
        lines = ["(%d older lines left out)" % (len(lines) - MAX_LINES)] + lines[-MAX_LINES:]
    return lines


def snapshot(b):
    r = b.request("status", reply_timeout=10.0)
    g = r.get("game") or {}
    out = {"frame": r.get("frame"), "state": r.get("state"), "map": r.get("map")}
    for k in ("pos", "facing", "camera", "life", "max_life", "rupees", "age", "event", "loading", "prompt"):
        if k in g:
            out[k] = g[k]
    return out


def look(d, s, x):
    b = bridge(s, x)
    try:
        st = snapshot(b)
    finally:
        b.close()
    st["log"] = new_lines(d, s, x)
    return st


def state(a):
    d = os.path.abspath(a.session)
    s = open_session(d)
    out = {x: look(d, s, x) for x in ([a.player] if a.player else TAGS)}
    save(d, s)
    return out


def do(a):
    d = os.path.abspath(a.session)
    s = open_session(d)
    x = a.player
    text = "\n".join(part.strip() for part in a.script.split(";") if part.strip()) + "\n"
    try:
        steps = hookshot.parse(text)
    except hookshot.ScriptError as e:   # (a warp to a scene the module would refuse: nothing is sent)
        return {"error": "ScriptError: %s" % e}
    b = bridge(s, x)
    err = None
    try:
        p = hookshot.Player(b, hookshot.Log(os.path.join(d, "player-%s.txt" % x)),
                            connect=lambda: bridge(s, x), commands=s["games"][x]["commands"])
        p.play(steps)
    except Exception as e:  # (a step the game refused: said, and the look still comes back)
        err = "%s: %s" % (e.__class__.__name__, e)
    finally:
        b.close()
    touch(d)   # (a long step is not idle time)
    out = {x: look(d, s, x)}
    if err:
        out["error"] = err
    save(d, s)
    return out


def take_shot(s, x, path):
    """A screenshot of game x at path: the path, or why there is none."""
    b = bridge(s, x)
    try:
        b.request("shot", reply_timeout=30.0, path=path, timeout=20)
        return path
    except Exception as e:
        return "no shot: %s" % e
    finally:
        b.close()


def shot(a):
    d = os.path.abspath(a.session)
    s = open_session(d)
    os.makedirs(os.path.join(d, "shots"), exist_ok=True)
    return {x: take_shot(s, x, os.path.join(d, "shots", "%s-%d.png" % (x, int(time.time() * 1000))))
            for x in ([a.player] if a.player else TAGS)}


def checkpoint(a):
    d = os.path.abspath(a.session)
    if not re.fullmatch(r"[A-Za-z0-9_-]+", a.tag):
        raise SystemExit("a tag is letters, digits, - and _")
    s = open_session(d)
    cps = os.path.join(d, "checkpoints")
    os.makedirs(cps, exist_ok=True)
    out = {"tag": a.tag}
    for x in TAGS:
        dump = s["games"][x]["commands"] + ".dump-%s.json" % a.tag
        try:
            os.remove(dump)
        except OSError:
            pass
    for x in TAGS:
        b = bridge(s, x)
        try:
            p = hookshot.Player(b, hookshot.Log(os.path.join(d, "player-%s.txt" % x)),
                                commands=s["games"][x]["commands"])
            p.play(hookshot.parse("dump %s\n" % a.tag))
            out[x] = {"status": snapshot(b)}
        finally:
            b.close()
    for x in TAGS:   # (the picture proves what the flags say: owner rule, no PASS from data alone)
        got = take_shot(s, x, os.path.join(cps, "%s-%s.png" % (a.tag, x)))
        out[x]["shot" if not got.startswith("no shot") else "shot_error"] = got
    for x in TAGS:
        dump = s["games"][x]["commands"] + ".dump-%s.json" % a.tag
        deadline = time.monotonic() + 10
        while not os.path.exists(dump) and time.monotonic() < deadline:
            time.sleep(0.1)
        try:
            with open(dump, encoding="utf-8") as f:
                out[x]["flags"] = json.load(f).get("flags", {})
        except (OSError, ValueError) as e:
            out[x]["flags"] = None
            out[x]["error"] = "no dump: %s" % e
    with open(os.path.join(cps, "%s.json" % a.tag), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    save(d, s)
    return {"tag": a.tag, **{x: {"map": out[x]["status"].get("map"), "flags": len(out[x]["flags"] or {}),
                                 "shot": out[x].get("shot") or out[x].get("shot_error")} for x in TAGS}}


# --- the watchdog ------------------------------------------------------------------------------------------------

def quit_game(s, x):
    b = bridge(s, x)
    try:
        b.request("quit", reply_timeout=10.0, check=False)
    finally:
        b.close()


def kill(pid):
    try:
        os.kill(pid, signal.SIGTERM)   # (Windows: TerminateProcess)
    except OSError:
        pass


def release_lock(n, owner):
    return lanes.hs.lock_release(lanes.hs.main_root(lanes.REPO), "lane%d" % n, owner, say=lambda *_: None)


def close(d, s, alive=None, quit=quit_game, killer=kill, release=release_lock, wait=QUIT_WAIT,
          clock=time.monotonic, sleep=time.sleep):
    """Ends a session: both games asked to quit, what is still running after `wait` s killed, the server killed, the
    lane lock let go. What happened to each, as a dict."""
    alive = alive or lanes.hs.pid_alive
    games, server = game_pids(d)
    out = {}
    for x in TAGS:
        try:
            quit(s, x)
            out[x] = "quit asked"
        except Exception as e:
            out[x] = "not reached: %s" % e
    end = clock() + wait
    while clock() < end and any(alive(p) for p in games.values()):
        sleep(0.5)
    for x, p in games.items():
        if alive(p):
            killer(p)
            out[x] = out.get(x, "") + ", killed"
    if server and alive(server):
        killer(server)
        out["server"] = "killed"
    if s.get("lane"):
        out["lock"] = "lane%d.lock %s" % (s["lane"], "released" if release(s["lane"], lock_owner(d)) else "held by another")
    return out


def watch(d, clock=time.time, sleep=time.sleep, alive=None, every=WATCH_EVERY):
    """Waits for the session's end; returns why it ended."""
    alive = alive or lanes.hs.pid_alive
    s = load(d)
    while True:
        try:
            s = load(d)
        except (OSError, ValueError):
            pass   # (being replaced this moment: the last one read)
        now = clock()
        if os.path.exists(stop_file(d)):
            return "stop asked"
        if now - s["started"] >= s["max_s"]:
            return "maximum duration (%d min) reached" % (s["max_s"] // 60)
        if s.get("booted"):
            if s.get("idle_s") and now - activity(d, s) >= s["idle_s"]:
                return "no tester command for %d min" % (s["idle_s"] // 60)
            gone = [x for x, p in sorted(game_pids(d)[0].items()) if not alive(p)]
            if gone:
                return "game %s ended" % " and ".join(gone)
        sleep(every)


def run_watchdog(d, acquire=None, watcher=watch, closer=close, pid=None):
    """The watchdog's whole life: the lane lock taken under its pid, the wait, the clean-up (also on an error or a
    signal). Writes watchdog.json: pid, lock ('held', 'none', 'refused'), ended (why), closed (what close did)."""
    pid = os.getpid() if pid is None else pid
    s = load(d)
    w = {"pid": pid, "lock": None, "ended": None}
    if s.get("lane"):
        acquire = acquire or (lambda n, owner, wait: lanes.lock_acquire(n, owner, wait=wait, pid=pid))
        if not acquire(s["lane"], lock_owner(d), s.get("lock_wait", 0)):
            w.update(lock="refused", ended="lane %d is not free (lanes.py lock status)" % s["lane"])
            write_json(watchdog_file(d), w)
            return 1
        w["lock"] = "held"
    else:
        w["lock"] = "none"
    write_json(watchdog_file(d), w)
    why = "the watchdog failed"
    try:
        why = watcher(d)
    except SystemExit as e:   # (a signal: see watchdog())
        why = "watchdog stopped (%s)" % e
    except Exception as e:
        why = "watchdog error: %s: %s" % (e.__class__.__name__, e)
    finally:
        w["closed"] = closer(d, load(d))
        w["ended"] = why
        write_json(watchdog_file(d), w)
    return 0


def watchdog(a):
    def bye(signum, _frame):
        raise SystemExit("signal %d" % signum)
    for name in ("SIGTERM", "SIGBREAK", "SIGINT"):
        if hasattr(signal, name):
            try:
                signal.signal(getattr(signal, name), bye)
            except (ValueError, OSError):
                pass
    return {"watchdog": run_watchdog(os.path.abspath(a.session))}


def stop(a, wait=60.0, clock=time.monotonic, sleep=time.sleep, alive=None):
    """Asks the watchdog to end the session and waits for it; without a live watchdog, closes the session here."""
    alive = alive or lanes.hs.pid_alive
    d = os.path.abspath(a.session)
    with open(stop_file(d), "w", encoding="utf-8"):
        pass
    w = read_json(watchdog_file(d), {}) or {}
    end = clock() + wait
    while not w.get("ended") and w.get("pid") and alive(int(w["pid"])) and clock() < end:
        sleep(0.5)
        w = read_json(watchdog_file(d), {}) or {}
    if not w.get("ended"):
        w["closed"] = close(d, load(d), alive=alive)
        w["ended"] = "stop asked (closed without the watchdog)"
        write_json(watchdog_file(d), w)
    return {"ended": w["ended"], "closed": w.get("closed")}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    st = sub.add_parser("start")
    st.add_argument("--session", required=True)
    st.add_argument("--lane", type=int, choices=list(lanes.LANES),
                    help="a test lane: its pair, port and pipes unless given; its lock held for the whole session")
    st.add_argument("--game-a", help="(required without --lane)")
    st.add_argument("--game-b", help="(required without --lane)")
    st.add_argument("--server", required=True)
    st.add_argument("--port", type=int, help="default %d (a lane: its own)" % PORT)
    st.add_argument("--boot-a")
    st.add_argument("--boot-b")
    st.add_argument("--load", help="a load script (A until play); default: the boot scripts load")
    st.add_argument("--no-load", action="store_true", help="the boot scripts take the title themselves")
    st.add_argument("--no-restore", action="store_true", help="keep the copies' saves (default: their baseline)")
    st.add_argument("--env", action="append", default=[], help="K=V, or A:K=V / B:K=V for one game")
    st.add_argument("--boot-timeout", type=int, default=900)
    st.add_argument("--max-minutes", type=float, default=MAX_MINUTES, help="the session's whole length")
    st.add_argument("--idle-minutes", type=float, default=IDLE_MINUTES, help="0: no idle end")
    st.add_argument("--lock-wait", type=float, default=0, help="s to wait for a busy lane (default: refuse at once)")
    st.add_argument("--playtest", action="store_true",
                    help="turn each game's play-test bridge on (A = p1, B = p2: 5001/5002, lane L 5000+10*L+1/+2)")
    st.add_argument("--display", help="the monitor the game windows go to, by device name (default $%s, else %s): "
                                      "tools/autotest/placement.py" % (hookshot.placement.ENV,
                                                                       hookshot.placement.DEFAULT_DISPLAY))
    for name in ("do", "state", "shot", "checkpoint", "stop", "watchdog"):
        q = sub.add_parser(name)
        q.add_argument("--session", required=True)
        if name == "do":
            q.add_argument("player", choices=TAGS)
            q.add_argument("script", help="test script lines, ';' between them")
        elif name in ("state", "shot"):
            q.add_argument("player", nargs="?", choices=TAGS)
        elif name == "checkpoint":
            q.add_argument("tag")
    a = p.parse_args(argv)
    out = {"start": start, "do": do, "state": state, "shot": shot, "checkpoint": checkpoint, "stop": stop,
           "watchdog": watchdog}[a.cmd](a)
    print(json.dumps(out, indent=1))
    return 1 if a.cmd == "do" and "error" in out else 0


if __name__ == "__main__":
    sys.exit(main())
