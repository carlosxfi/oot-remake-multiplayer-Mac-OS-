"""An MCP server (stdio) that gives Claude Code the controls of a live play-test session (tools/autotest/live.py).

Register it once from the repository folder, pointing at a session started with `live.py start`:

    claude mcp add oot-playtest -- python -I tools/autotest/mcp_playtest.py --session <DIR>

The tools act on the session's two games (A and B): look (`state`), act (`act`: test script lines), see (`shot`),
checkpoint both games' shared flags (`checkpoint`) and compare them (`desync_check`). The MCP protocol is spoken by
hand (newline-delimited JSON-RPC 2.0 on stdin/stdout: initialize, tools/list, tools/call), standard library only.
"""
import argparse
import io
import json
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import desync_check  # noqa: E402
import live  # noqa: E402
import playtest_client as pc  # noqa: E402

SCRIPT_HELP = (
    "Test script lines (tools/autotest/hookshot.py's format), ';' between them. Times are game frames (60 a second). "
    "Movement: `goto X Z TOLERANCE FRAMES` (walk to a point, metres), `toward X Z FRAMES [BUTTONS]` (walk that way, "
    "holding buttons), `stick LX LY FRAMES`, `wait FRAMES`. Buttons: `press A 8`, `hold ZL+B 30` (A B X Y L R ZL ZR "
    "PLUS MINUS UP DOWN LEFT RIGHT). Co-op test commands (need LICHII_COOP_CHEATS=1): `warp SCENE` (e.g. "
    "ForestTemple, SacredForestMeadow_Adult), `place X Y Z`, `face X Z`, `give ITEM`, `setflag NAME VALUE`, "
    "`kill ACTOR`, `hit ACTOR N`, `hurt N`, `chat TEXT`, `mark NAME`.")

TOOLS = [
    {"name": "state", "description": "What a game shows now: map, Link's position [x,y,z] in metres, facing and "
     "camera yaw (0 = +z, 90 = +x), life and max life (quarter hearts), rupees, age, whether an event or a load is "
     "running, the A-button prompt, and the new co-op log lines since the last look. Both games without `player`.",
     "inputSchema": {"type": "object", "properties": {"player": {"type": "string", "enum": ["A", "B"]}}}},
    {"name": "act", "description": "Play test script lines in one game, then return its state. " + SCRIPT_HELP,
     "inputSchema": {"type": "object", "required": ["player", "script"],
                     "properties": {"player": {"type": "string", "enum": ["A", "B"]},
                                    "script": {"type": "string"}}}},
    {"name": "shot", "description": "Screenshot one game or both: the PNG paths (open them to see the screen). "
     "Use sparingly; state is cheaper.",
     "inputSchema": {"type": "object", "properties": {"player": {"type": "string", "enum": ["A", "B"]}}}},
    {"name": "checkpoint", "description": "Write both games' shared dungeon and story flags and a screenshot of "
     "each game under a tag (letters, digits, - and _). Take one after every action that should sync, then run "
     "desync_check; open the shots for anything visible (flags alone are no proof of what a player sees).",
     "inputSchema": {"type": "object", "required": ["tag"], "properties": {"tag": {"type": "string"}}}},
    {"name": "desync_check", "description": "Compare every checkpoint of the session: the first tag where the two "
     "games' shared flags differ, and which flags.",
     "inputSchema": {"type": "object", "properties": {"all": {"type": "boolean"}}}},
]


PLAYER = {"type": "string", "enum": list(pc.PLAYERS)}
FRAMES = {"type": "integer", "minimum": 1, "maximum": 3600}
PAUSED = ("Every reply has the game's frame count; the game is paused between calls and only runs the frames a "
          "call asks for.")

# One game's tools (--player): that game's play-test bridge (docs/playtest.md, "Bridge protocol").
PLAYER_TOOLS = [
    {"name": "get_state", "description": "This game's state: scene and room, Link's position [x,y,z] in metres, "
     "facing and camera yaw (degrees: 0 = +z, 90 = +x), life and max life (quarter hearts), rupees, age, equips, "
     "inventory, co-op status, event/loading and the A prompt. " + PAUSED,
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "walk_to", "description": "Walk Link to a point (x, z in metres) with the camera-relative stick, a "
     "few frames at a time. Stops on arrival (within tolerance metres), a scene change, a load or an event, or "
     "after max_frames; says why, the frames used and the distance left.",
     "inputSchema": {"type": "object", "required": ["x", "z"],
                     "properties": {"x": {"type": "number"}, "z": {"type": "number"},
                                    "tolerance": {"type": "number", "minimum": 0.05}, "max_frames": FRAMES}}},
    {"name": "press", "description": "Hold buttons (" + " ".join(pc.BUTTONS) + ") and optionally the stick "
     "(angle in degrees: 0 = away from the camera, 90 = right; strength 0..1) for N frames (default 2), then "
     "pause. Returns the state after.",
     "inputSchema": {"type": "object", "properties": {
         "buttons": {"type": "array", "items": {"type": "string", "enum": list(pc.BUTTONS)}},
         "stick_angle": {"type": "number"}, "stick_strength": {"type": "number", "minimum": 0, "maximum": 1},
         "frames": FRAMES}}},
    {"name": "step", "description": "Let the game run N frames with no input, then pause. " + PAUSED,
     "inputSchema": {"type": "object", "required": ["frames"], "properties": {"frames": FRAMES}}},
    {"name": "warp", "description": "Warp to a scene (a map name, e.g. KokiriForest_Child, ForestTemple) and "
     "optionally an entrance (a start position name in it), through the game's own scene change. The game runs "
     "until Link is there with no event or load, then is held. Set a test up this way; do not walk across Hyrule.",
     "inputSchema": {"type": "object", "required": ["scene"],
                     "properties": {"scene": {"type": "string"}, "entrance": {"type": "string"}}}},
    {"name": "flag", "description": "Set a game flag (a GameData flag name or a 0x hash) to a value: a number, "
     "true/false, or an enum value's name. Done inside the held frame; replies with the old value (was).",
     "inputSchema": {"type": "object", "required": ["name"],
                     "properties": {"name": {"type": "string"},
                                    "value": {"type": ["integer", "boolean", "string"]}}}},
    {"name": "give", "description": "Give Link an item (bombs, bow, hookshot, or an item flag name the cheats "
     "panel knows). It is there after the next frame: step 1 before checking.",
     "inputSchema": {"type": "object", "required": ["item"], "properties": {"item": {"type": "string"}}}},
    {"name": "checkpoint", "description": "Write this game's shared flags and state under a tag (letters, digits, "
     "- and _) with its frame count, to the run's checkpoint log. Take the same tag on EVERY game after each "
     "action that should sync, then run desync_check.",
     "inputSchema": {"type": "object", "required": ["tag"], "properties": {"tag": {"type": "string"}}}},
    {"name": "desync_check", "description": "Compare every game's checkpoints in the run's log folder, matched by "
     "tag: the first tag where the games' shared flags differ, which flags, and each game's frame.",
     "inputSchema": {"type": "object", "properties": {"all": {"type": "boolean"}}}},
    {"name": "pause", "description": "Hold the game (it is held from the first call anyway). " + PAUSED,
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "resume", "description": "Let the game run in real time again (a person takes over). The next call "
     "holds it again.", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "log", "description": "This game's new co-op log lines (`coop:`) since the last call, the newest "
     "%d at most; `match` keeps only lines containing that text. Needs --session (games from live.py start: p1 = A, "
     "p2 = B). No frame passes." % live.MAX_LINES,
     "inputSchema": {"type": "object", "properties": {"match": {"type": "string"}}}},
    {"name": "screenshot", "description": "A PNG of this game's screen (the path: open it to see). Use sparingly; "
     "get_state is cheaper. Anything visible is proven only by a picture. A held game runs 1 frame for it.",
     "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}}}},
]


class Game:
    """One game's tools: its bridge client, which player it is, and the run's log folder (shared by every game's
    server, so desync_check sees them all)."""

    def __init__(self, player, client, log_dir, session=None):
        self.player, self.client, self.log_dir, self.session = player.lower(), client, log_dir, session
        self.offsets = None   # this server's own place in the game's log (two servers never write one session file)

    def log_lines(self, match=None):
        """The game's new co-op log lines since the last call (live.py's session log: p1 = A, p2 = B)."""
        if not self.session:
            raise ValueError("log needs --session (games from live.py start); read the game's own log otherwise")
        i = pc.player_index(self.player) - 1
        if i >= len(live.TAGS):
            raise ValueError("live.py starts games A and B only (p1, p2): %s has no session log" % self.player)
        x = live.TAGS[i]
        s = live.load(self.session)
        if self.offsets is None:
            self.offsets = {x: (s.get("offsets") or {}).get(x, 0)}   # from the end of the boot
        lines = live.new_lines(self.session, {"games": s["games"], "offsets": self.offsets}, x)
        if match:
            lines = [ln for ln in lines if match in ln]
        return {"frame": self.client.last_frame, "lines": lines}

    def call(self, name, args):
        if self.session:
            live.open_session(self.session)   # (an ended session's games are gone: say so, do not drive them)
            live.touch(self.session)          # the session's watchdog counts this as a tester command
        try:
            return self._call(name, args)
        finally:
            if self.session:
                live.touch(self.session)      # (a long walk_to or warp is not idle time)

    def _call(self, name, args):
        c = self.client
        if name == "get_state":
            return c.state()
        if name == "walk_to":
            return pc.walk_to(c, float(args["x"]), float(args["z"]), float(args.get("tolerance", 0.5)),
                              int(args.get("max_frames", 600)))
        if name == "press":
            buttons = [str(b).upper() for b in args.get("buttons") or []]
            bad = [b for b in buttons if b not in pc.BUTTONS]
            if bad:
                raise ValueError("no button %s" % ", ".join(bad))
            strength = float(args.get("stick_strength", 0))
            stick = [float(args.get("stick_angle", 0)), strength] if strength > 0 else None
            return c.input(stick, buttons, int(args.get("frames", 2)))
        if name == "step":
            return c.step(int(args["frames"]))
        if name == "warp":
            return c.warp(args["scene"], args.get("entrance"))
        if name == "flag":
            return c.flag(args["name"], args.get("value", 1))
        if name == "give":
            return c.give(args["item"])
        if name == "pause":
            return c.pause()
        if name == "resume":
            return c.resume()
        if name == "checkpoint":
            return pc.checkpoint(c, self.log_dir, self.player, args["tag"])
        if name == "desync_check":
            buf = io.StringIO()
            code = desync_check.check_clients(self.log_dir, bool(args.get("all")),
                                              out=lambda s: buf.write(s + "\n"))
            return {"result": desync_check.RESULTS[code], "report": buf.getvalue()}
        if name == "log":
            return self.log_lines(args.get("match"))
        if name == "screenshot":
            label = args.get("name") or "shot"
            if not pc.TAG.match(label):
                raise ValueError("a name is letters, digits, - and _")
            r = c.shot("%s-%s-%d" % (self.player, label, int(time.time() * 1000)))
            got = r.get("path")
            if got and os.path.isfile(got):   # a copy beside the run's logs: every game's shots in one place
                folder = os.path.join(self.log_dir, "shots")
                os.makedirs(folder, exist_ok=True)
                r["copy"] = shutil.copy(got, os.path.join(folder, os.path.basename(got)))
            return r
        raise ValueError("no tool %s" % name)


class Args:
    def __init__(self, **kw):
        self.__dict__.update(kw)


def call(session, name, args):
    if name == "state":
        return live.state(Args(session=session, player=args.get("player")))
    if name == "act":
        return live.do(Args(session=session, player=args["player"], script=args["script"]))
    if name == "shot":
        return live.shot(Args(session=session, player=args.get("player")))
    if name == "checkpoint":
        return live.checkpoint(Args(session=session, tag=args["tag"]))
    if name == "desync_check":
        buf = io.StringIO()
        code = desync_check.check(session, bool(args.get("all")), out=lambda s: buf.write(s + "\n"))
        return {"result": ["agree", "desync", "nothing to compare", "incomplete"][code], "report": buf.getvalue()}
    raise ValueError("no tool %s" % name)


def reply(id_, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": id_}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def serve(session, game=None):
    """Answers MCP requests on stdin: the two-game session's tools (session), or one game's (game)."""
    tools = PLAYER_TOOLS if game else TOOLS
    name = ("oot-%s" % game.player) if game else "oot-playtest"
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except ValueError:
            reply(None, error={"code": -32700, "message": "parse error"})
            continue
        method, id_ = req.get("method"), req.get("id")
        if id_ is None:
            continue  # a notification (initialized, cancelled): nothing to answer
        if method == "initialize":
            reply(id_, {"protocolVersion": (req.get("params") or {}).get("protocolVersion", "2024-11-05"),
                        "capabilities": {"tools": {}},
                        "serverInfo": {"name": name, "version": "2"}})
        elif method == "tools/list":
            reply(id_, {"tools": tools})
        elif method == "tools/call":
            params = req.get("params") or {}
            try:
                args = params.get("arguments") or {}
                out = game.call(params.get("name"), args) if game else call(session, params.get("name"), args)
                reply(id_, {"content": [{"type": "text", "text": json.dumps(out, indent=1)}]})
            except (Exception, SystemExit) as e:  # (SystemExit from a bad tag too: an error the tester reads)
                reply(id_, {"content": [{"type": "text", "text": "%s: %s" % (e.__class__.__name__, e)}],
                            "isError": True})
        elif method == "ping":
            reply(id_, {})
        else:
            reply(id_, error={"code": -32601, "message": "no method %s" % method})


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--session", help="a two-game session (live.py start): the tools state, act, shot, checkpoint, "
                   "desync_check; with --player: the session whose watchdog each call keeps awake")
    p.add_argument("--player", choices=pc.PLAYERS, help="one game's tools, through its play-test bridge")
    p.add_argument("--port", type=int, help="that game's bridge port (default: the player's, on --lane when given)")
    p.add_argument("--lane", type=int, help="a test lane (tools/autotest/lanes.py): its bridge ports")
    p.add_argument("--log-dir", default="playtest-logs",
                   help="the run's checkpoint logs and shots; give every game's server the same one")
    a = p.parse_args(argv)
    if not a.player:
        if not a.session:
            p.error("give --session, or --player (and --session when the games came from live.py start)")
        serve(os.path.abspath(a.session))
        return 0
    session = os.path.abspath(a.session) if a.session else None
    serve(None, Game(a.player, pc.Client(a.port or pc.port_for(a.player, a.lane)), os.path.abspath(a.log_dir),
                     session))
    return 0


if __name__ == "__main__":
    sys.exit(main())
