"""The outside half of the play-test bridge: one game's bridge client, walk_to's steering and the checkpoint log.

The game's play-test bridge (hookshot/src/coop_playtest.cpp; off unless LICHII_COOP_PLAYTEST_PORT is set;
docs/playtest.md, "Bridge protocol") listens on 127.0.0.1 only, one port per game, and speaks one JSON object per
line each way. Every reply carries the game's frame count, which lines up two games' checkpoints later
(desync_check.py --clients). A held game runs only the frames a command asks for.

This file holds what is easy to change without a game build: the client, the camera-relative steering of walk_to
(learnt from how Link actually moves), and the per-game checkpoint log. mcp_playtest.py --player turns it into MCP
tools. Standard library only.
"""
import json
import math
import os
import re
import socket
import time

HOST = "127.0.0.1"
PORT_BASE = 5000          # player N: PORT_BASE + N (p1 5001 ... p4 5004); on test lane L: PORT_BASE + 10*L + N
PLAYERS = ("p1", "p2", "p3", "p4")
TAG = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
BUTTONS = ("A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LS", "RS", "UP", "DOWN", "LEFT", "RIGHT")
MAX_FRAMES = 3600


class BridgeError(Exception):
    """The game refused a command (ok false): its error code (bad_request, busy, no_control, not_in_play, timeout,
    failed) and message."""

    def __init__(self, reply):
        super().__init__("%s: %s" % (reply.get("error"), reply.get("message", "")))
        self.reply = reply
        self.code = reply.get("error")


def player_index(player):
    p = str(player).lower()
    if p not in PLAYERS:
        raise ValueError("a player is one of %s" % ", ".join(PLAYERS))
    return PLAYERS.index(p) + 1


def port_for(player, lane=None):
    """The bridge port of player p1..p4, on a test lane when given (tools/autotest/lanes.py)."""
    return PORT_BASE + 10 * int(lane or 0) + player_index(player)


class Client:
    """One game's bridge over ONE connection, kept for the life of the process (the bridge lifts its hold when the
    connection closes, so a crashed tester never leaves a game frozen). It connects at the first request and holds
    the game then (`hold`, unless the request is pause or resume itself), so the game is still between calls. A
    broken connection is opened again at the next request; the game ran in real time meanwhile, so it is held
    again and that reply says "reconnected". Replies are matched by id, in order."""

    def __init__(self, port, host=HOST, timeout=30.0, connect=socket.create_connection, hold=True):
        if host not in ("127.0.0.1", "localhost"):
            raise ValueError("the bridge is local only (127.0.0.1)")
        self.host, self.port, self.timeout, self._connect, self.hold = host, int(port), timeout, connect, hold
        self.sock = self.rfile = None
        self.connects = 0
        self.next_id = 1
        self.last_frame = None

    def close(self):
        """Closes the connection: the bridge lets the game run again."""
        for f in (self.rfile, self.sock):
            try:
                if f is not None:
                    f.close()
            except OSError:
                pass
        self.sock = self.rfile = None

    def _open(self):
        self.sock = self._connect((self.host, self.port), self.timeout)
        self.rfile = self.sock.makefile("rb")
        self.connects += 1

    def _send(self, cmd, wait, args):
        rid = self.next_id
        self.next_id += 1
        self.sock.settimeout(wait)
        self.sock.sendall((json.dumps(dict(args, id=rid, cmd=cmd), separators=(",", ":")) + "\n").encode())
        while True:
            line = self.rfile.readline()
            if not line:
                raise ConnectionError("%s: the bridge closed the connection" % cmd)
            reply = json.loads(line.decode("utf-8"))
            if isinstance(reply.get("id"), int) and reply["id"] < rid:
                continue   # an older request's late reply (one that timed out here)
            return reply

    def request(self, cmd, check=True, reply_timeout=None, **args):
        wait = reply_timeout or self.timeout
        reconnected = False
        if self.sock is None:
            self._open()
            reconnected = self.connects > 1
            if self.hold and cmd not in ("pause", "resume"):
                try:
                    held = self._send("pause", self.timeout, {})
                except (OSError, ValueError):
                    self.close()
                    raise
                if not held.get("ok"):
                    self.close()
                    raise BridgeError(held)
        try:
            reply = self._send(cmd, wait, args)
        except (OSError, ValueError):
            self.close()   # (the next request connects again and holds the game)
            raise
        if reconnected:
            reply["reconnected"] = True
        if reply.get("frame") is not None:
            self.last_frame = reply["frame"]
        if check and not reply.get("ok"):
            raise BridgeError(reply)
        return reply

    # the commands (docs/playtest.md, "Bridge protocol")
    def hello(self):
        return self.request("hello")

    def pause(self):
        return self.request("pause")

    def resume(self):
        return self.request("resume")

    def state(self):
        return self.request("state")

    def step(self, frames=1):
        return self.request("step", frames=_frames(frames))

    def input(self, stick=None, buttons=(), frames=1, rstick=None):
        """Holds the pad for `frames` frames, then the game is held again. A stick is [angle, strength]: degrees
        clockwise from up (up = away from the camera), strength 0..1."""
        args = {"buttons": list(buttons), "frames": _frames(frames)}
        if stick is not None:
            args["stick"] = [round(float(stick[0]), 2) % 360.0, round(max(0.0, min(1.0, float(stick[1]))), 3)]
        if rstick is not None:
            args["rstick"] = [round(float(rstick[0]), 2) % 360.0, round(max(0.0, min(1.0, float(rstick[1]))), 3)]
        return self.request("input", reply_timeout=self.timeout + frames / 20.0, **args)

    def warp(self, scene, entrance=None, timeout=60):
        args = {"scene": scene, "timeout": timeout}
        if entrance not in (None, ""):
            args["entrance"] = entrance
        return self.request("warp", reply_timeout=timeout + 15, **args)

    def flag(self, name, value=1):
        return self.request("flag", name=name, value=value)

    def give(self, item):
        return self.request("give", item=item)

    def dump(self, tag):
        return self.request("dump", tag=tag)

    def shot(self, name=None):
        """A screenshot; the game saves it under its own playtest/shots/<name>.png and the reply gives the path."""
        if name is not None and not TAG.match(name):
            raise ValueError("a shot name is 1 to 64 letters, digits, - and _")
        return self.request("shot", reply_timeout=60, **({"name": name} if name else {}))


def _frames(n):
    n = int(n)
    if not 1 <= n <= MAX_FRAMES:
        raise ValueError("frames is 1 to %d" % MAX_FRAMES)
    return n


def game_of(reply):
    """The state in a reply: its "state" (or "game") object when it has one, else the reply itself (state)."""
    for key in ("state", "game"):
        if isinstance(reply.get(key), dict):
            return reply[key]
    return reply


# --- walk_to -------------------------------------------------------------------------------------------------------

def _wrap(deg):
    return (deg + 180.0) % 360.0 - 180.0


def yaw_to(pos, x, z):
    """The world yaw in degrees from pos toward (x, z): 0 toward +z, 90 toward +x (the state's convention)."""
    return math.degrees(math.atan2(x - pos[0], z - pos[2]))


class Steering:
    """Camera-relative steering. The model: Link walks toward world yaw camera + offset + sign * stick angle. The
    bridge's starting guess is offset 0, sign +1 (docs/playtest.md); both are learnt from Link's real moves. The
    offset is learnt for each sign; the sign whose offset fits the moves best wins (they tell apart once the stick
    angle changes, as it does on any bend). A walk that gets nowhere for `stuck_after` pushes steps aside."""

    def __init__(self, slow_within=3.0, stuck_after=3, detour_for=3, sign=1):
        self.slow_within, self.stuck_after, self.detour_for = slow_within, stuck_after, detour_for
        self.sign = sign
        self.fit = {1: {"offset": 0.0, "err": 0.0, "n": 0}, -1: {"offset": 0.0, "err": 0.0, "n": 0}}
        self.last = None              # (pos, camera, stick angle) of the last push
        self.stuck = self.detour_left = 0
        self.detour = 90.0

    def learn(self, pos, camera):
        if self.last is None:
            return
        lpos, lcam, lang = self.last
        if math.hypot(pos[0] - lpos[0], pos[2] - lpos[2]) <= 0.05:
            self.stuck += 1
            if self.stuck >= self.stuck_after and not self.detour_left:
                self.detour, self.detour_left, self.stuck = -self.detour, self.detour_for, 0
            return
        self.stuck = 0
        seen = yaw_to(lpos, pos[0], pos[2])
        for s, f in self.fit.items():
            got = _wrap(seen - lcam - s * lang)            # what this sign says the offset is
            if f["n"] == 0:
                f["offset"] = got
            else:
                miss = _wrap(got - f["offset"])
                f["err"] = 0.6 * f["err"] + 0.4 * abs(miss)
                f["offset"] = _wrap(f["offset"] + 0.5 * miss)
            f["n"] += 1
        other = -self.sign
        if self.fit[other]["n"] >= 2 and self.fit[other]["err"] + 10.0 < self.fit[self.sign]["err"]:
            self.sign = other

    def push(self, pos, camera, target):
        """The stick [angle, strength] for Link at pos (metres) with the camera yaw (None: unknown, taken as 0) to
        walk toward target (x, z)."""
        camera = float(camera or 0.0)
        self.learn(pos, camera)
        want = yaw_to(pos, target[0], target[1])
        angle = self.sign * _wrap(want - camera - self.fit[self.sign]["offset"])
        if self.detour_left:
            angle += self.detour
            self.detour_left -= 1
        dist = math.hypot(target[0] - pos[0], target[1] - pos[2])
        strength = round(max(0.45, min(1.0, dist / self.slow_within)), 3)
        angle = round(angle, 2) % 360.0
        self.last = (list(pos), camera, angle)
        return [angle, strength]


def walk_to(client, x, z, tolerance=0.5, max_frames=600, chunk=8):
    """Walks Link to (x, z) a few frames at a time (the game is held after each). Stops on arrival (within
    `tolerance` metres), a scene change, a load or an event, or after max_frames. Returns why, the frames used,
    the distance left and the last state."""
    st = game_of(client.state())
    scene = st.get("scene")
    steer = Steering()
    used = 0
    while True:
        pos = st.get("pos")
        if not pos:
            return {"arrived": False, "reason": "no position in the state (not in play?)", "frames": used,
                    "state": st}
        dist = math.hypot(x - pos[0], z - pos[2])
        out = {"frames": used, "distance": round(dist, 3), "state": st}
        if dist <= tolerance:
            return dict(out, arrived=True, reason="arrived")
        if st.get("scene") != scene:
            return dict(out, arrived=False, reason="scene changed")
        if st.get("loading") or st.get("event"):
            return dict(out, arrived=False, reason="loading" if st.get("loading") else "event")
        if used >= max_frames:
            return dict(out, arrived=False, reason="timed out")
        n = min(chunk, max_frames - used)
        st = game_of(client.input(steer.push(pos, st.get("camera"), (x, z)), (), n))
        used += n


# --- the per-game checkpoint log -----------------------------------------------------------------------------------

def checkpoint_file(log_dir, player):
    return os.path.join(log_dir, "%s.checkpoints.jsonl" % player.lower())


def read_dump(reply):
    """The game's dump: (flags, state) from the file the reply names ("path"), or from the reply itself."""
    if isinstance(reply.get("flags"), dict):
        return reply["flags"], reply.get("state")
    path = reply.get("path")
    if not path:
        return None, None
    deadline = time.monotonic() + 10
    while not os.path.exists(path) and time.monotonic() < deadline:
        time.sleep(0.1)
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    return d.get("flags"), d.get("state")


def checkpoint(client, log_dir, player, tag):
    """The game's shared flags and state under a tag, appended to this game's checkpoint log with its frame."""
    if not TAG.match(tag):
        raise ValueError("a tag is 1 to 64 letters, digits, - and _")
    reply = client.dump(tag)
    try:
        flags, state = read_dump(reply)
        error = None if isinstance(flags, dict) else "the dump has no flags"
    except (OSError, ValueError) as e:
        flags, state, error = None, None, "no dump: %s" % e
    if not isinstance(state, dict):
        state = game_of(client.state())
    entry = {"tag": tag, "player": player.lower(), "frame": reply.get("frame"), "time": time.time(),
             "scene": state.get("scene"), "state": state, "flags": flags if isinstance(flags, dict) else None,
             "dump": reply.get("path")}
    if error:
        entry["error"] = error
    os.makedirs(log_dir, exist_ok=True)
    with open(checkpoint_file(log_dir, player), "a", encoding="utf-8") as f:
        f.write(json.dumps(entry, separators=(",", ":")) + "\n")
    out = {"tag": tag, "frame": entry["frame"], "scene": entry["scene"],
           "flags": len(entry["flags"]) if entry["flags"] is not None else None}
    if error:
        out["error"] = error
    return out
