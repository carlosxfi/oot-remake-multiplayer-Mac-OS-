# Live play-testing (two games, step by step)

The unit tests prove the sync logic; only a game run shows what two players actually see. The two-game runner
(`tools/autotest/hookshot.py run`, [testing.md](testing.md)) plays a whole script blind. The live kit keeps both games
open and takes one step at a time, so a tester (a person or Claude Code) acts, looks at both games, and decides the
next step: the way agent-driven play of other games works, with the game handing over its state as data.

Maintainer only, like the rest of `tools/autotest`: it needs two Hookshot test copies with the game.

## The pieces

| Piece | What it does |
| --- | --- |
| `tools/autotest/live.py start` | Starts the server and both games (the runner with `--keep-open`), plays each game's boot script (e.g. warp to a dungeon, past the title), and leaves them running. Each game's save is restored to its baseline first (`--no-restore` keeps the copies' saves). `--lane N` uses a test lane's pair, port and pipes (`tools/autotest/lanes.py`). Starts the session's watchdog. Writes `session.json` in the session folder. |
| `live.py do DIR A "lines"` | Plays test script lines in one game (`goto`, `toward`, `press`, `hold`, `warp`, `place`, `setflag`, `kill`, `hit`, ...; `;` between lines), then returns that game's state. |
| `live.py state DIR [A\|B]` | Each game's control-bridge snapshot (map, Link's place, facing, camera, life, rupees, age, event, loading, A prompt) and its new `coop:` log lines since the last look. |
| `live.py shot DIR [A\|B]` | A screenshot of each game (a PNG path). |
| `live.py checkpoint DIR TAG` | Both games write the value of every flag in the shared dungeon and story lists (the module's `dump TAG` test command, next to its commands file; `CoopDungeon::Flags` and `CoopStory::Flags`, not the live-only or autogen lists), and a screenshot of each game is taken (`checkpoints/TAG-A.png`, `TAG-B.png`); all are kept as `checkpoints/TAG.json` with each game's status. The games dump one after the other, so a flag still on its way between them can differ in one checkpoint: a real desync still differs in the next one. |
| `live.py stop DIR` | Ends the session: the watchdog quits both games, kills what did not quit and the server, and lets the lane lock go. |
| `tools/autotest/desync_check.py DIR` | Compares the checkpoints in order: the first tag where the two games' shared flags differ, and which flags, with the checkpoint's shots. Exit 0 every checkpoint compared and agrees, 1 desync, 2 nothing compared (no checkpoint, or none with both games' flags), 3 incomplete (one could not be compared). A plain script, no judgment. |
| `tools/autotest/mcp_playtest.py --session DIR` | An MCP server (stdio, standard library only) with the tools `state`, `act`, `shot`, `checkpoint` and `desync_check`, so Claude Code calls them by name. |

Register the MCP server once from the repository folder, for a session started with `live.py start`:

```sh
claude mcp add oot-playtest -- python -I tools/autotest/mcp_playtest.py --session <session folder>
```

## Bridge protocol

The play-test bridge is a small part of the co-op module inside each game (`hookshot/src/coop_playtest.cpp`). An agent
or MCP client drives one game per connection. It adds what Longshot's own control bridge lacks: **frame control** (the
game holds still between calls). It also gives the co-op module's test commands and Longshot's input, shots and Link
snapshot through one socket with one frame count.

### Turning it on (off by default)

Nothing listens unless the game is started with:

| Switch | Value |
|---|---|
| `LICHII_COOP_PLAYTEST_PORT` | TCP port, 1024 to 65535. Unset, empty or `0`: the bridge is off and nothing listens |
| `HOS_CONTROL` | Longshot's control socket (Linux) or pipe (Windows). The bridge forwards `input`, `shot` and the Link snapshot in `state` to it. Without it those answer `no_control` |
| `LICHII_COOP_PLAYTEST_IDLE` | Seconds a hold may last with no command before the game resumes by itself (default 600, `0` never), in case the tester crashed |

The shipped 0.11.12 kits (Windows and Linux) have the bridge built in but off. A tester turns it on for one game with
these variables; `live.py start` sets them for each game it starts. The log says
`coop: playtest: listening on 127.0.0.1:<port>` or `coop: playtest: off (<why>)`.

**Ports, one per game:** player N on `5000 + N` (p1 5001, p2 5002, ... p4 5004). On test lane L: `5000 + 10*L + N`
(lane 1: 5011, 5012). The MCP server takes the port as its argument.

**Security:** bound to `127.0.0.1` only, never another address. One client at a time: a second connection gets one
`busy` reply and is closed. **Keep one connection open for the whole session:** when the client's connection closes
(it quit or crashed), a held game runs again at once, so a lost tester never leaves a game frozen. Dumps and shots go
only into the game's own `playtest/dumps/` and `playtest/shots/` folders, named by a tag; no path is taken from the
client. It can do what Longshot's control bridge and the cheats panel can do (move Link, warp, set
flags, give items), so it is for test copies with a throwaway save.

### Messages

One JSON object per line (UTF-8, `\n`), at most 64 KB a line. A request has `cmd` and an optional `id` (any JSON value,
given back). Requests are answered in order, one reply each. Every reply carries:

| Field | Meaning |
|---|---|
| `id` | The request's `id`, or `null` |
| `ok` | `true`, or `false` with `error` (a code below) and `message` (for a person) |
| `frame` | The game's frame count: calc frames since the game started (the module's per-frame hook). It is what `step` counts and what lines up two games' dumps |
| `paused` | Whether the game is held now |

`input`, `step` and `warp` replies hold the end state nested as `"state": {...}`; `state` itself is flat.

Error codes: `bad_request` (unknown `cmd`, a missing or bad field), `busy` (another client), `no_control`
(`HOS_CONTROL` unset or unreachable), `not_in_play` (no Link: title, file select, a load), `timeout`, `failed` (the
game refused; `message` says why).

```
> {"id":1,"cmd":"hello"}
< {"id":1,"ok":true,"frame":5321,"paused":false,"protocol":1,"control":true}
```

### Frame control

The game holds inside its own per-frame hook: the calc thread waits there before the frame's update, so a held game
does not advance at all (no Link, no enemies, no timers). The window, the sound thread, Longshot's control bridge and
the co-op connection keep running (the network thread answers the server's pings, so a held game is never dropped
as timed out). The other game's puppet of this player stands still while it is held. The server does not know a game
is held: an enemy or boss this game owns stays with it while the other game runs. So **always hold both games** and
step them in turn, the same number of frames.

| Command | Fields | Does |
|---|---|---|
| `pause` | | Holds the game at its next frame. Replies once it is held |
| `resume` | | Lets it run in real time again |
| `step` | `frames` (1 to 3600, default 1) | Lets exactly that many frames run, then holds. Replies when held again, with `from` (`frame - from == frames`). A running game is held first |

### Commands

| Command | Fields | Does / replies |
|---|---|---|
| `state` | | The state below. Does not advance the game |
| `input` | `buttons` (list of names, default none), `stick` `[angle, strength]` (left stick: angle in degrees, 0 = up, 90 = right, clockwise; strength 0 to 1), `rstick` (the same for the right stick), `frames` (1 to 3600, default 1), `exclusive` (default `true`: the real pad and keys are shut out meanwhile) | Holds that pad for `frames` frames from the next frame, lets them run, then holds. Replies with `from` and the `state` at the end. Buttons (Switch names): `A B X Y L R ZL ZR PLUS MINUS LS RS UP DOWN LEFT RIGHT` |
| `warp` | `scene` (a map name, e.g. `KokiriForest_Child`), `entrance` (a start position name in that scene; omitted: the scene's own), `timeout` (seconds, default 60) | The co-op test warp (the game's own `Sequence_ReservedNextBanc`, as the test script's `warp`). The game runs (10 frames at a time) until Link is in that scene with no event or load, then holds again. Replies with `"arrived": true` and the `state`; after the timeout, still held, `timeout` with the state in `message` |
| `flag` | `name` (a GameData flag name or a `0x` hash), `value` (number, `true`/`false`, or an enum value's name) | Sets the flag through the game's own setter (the test script's `setflag` / `setenum`). Done inside the held frame: no frame passes. Replies with `was` and `value` |
| `give` | `item` (`bombs`, `bow`, `hookshot`, or an item flag name the cheats panel knows) | The test script's `give` (the cheats panel's queue, so it needs `LICHII_COOP_CHEATS=1`): queued inside the held frame, in the inventory after the next frame runs |
| `dump` | `tag` (1 to 64 letters, digits, `-` and `_`) | Writes `playtest/dumps/<tag>.json` in the game's folder: `{"frame", "tag", "state", "flags": {name: value}}`, the shared flags (the checkpoint's lists) read inside the held frame. Replies with `path` (absolute) and `flags` (the count) |
| `shot` | `name` (optional; letters, digits, `-` and `_`; default `shot-f<frame>`) | A screenshot through Longshot's bridge into `playtest/shots/<name>.png`. A held game presents nothing, so it runs 1 frame at a time (at most 5) until the picture comes: the reply's `advanced` says how many. Replies with `path`, `width`, `height` |

### State

```
{"frame":5400,"paused":true,"scene":"KokiriForest_Child","room":null,"pos":[-7.5,0.0,71.1],"facing":90.0,
 "camera":180.0,"life":12,"max_life":12,"rupees":5,"age":"child","event":false,"loading":false,"prompt":"Talk",
 "equips":{"weapon":"KokiriSword","shield":"DekuShield","head":null,"armor":"Kokiri","boots":"Normal"},
 "inventory":{"PouchData.Item_Bomb.StockNum":0,"PouchData.Item_Hookshot.StockNum":1},"coop":{"players":[{"number":1,"name":"A","self":true,"state":"connected","scene":"KokiriForest_Child"}]},
 "snapshot_present":5398,"control":true}
```

`scene`, `pos`, `facing`, `camera`, `life`, `max_life`, `rupees`, `age`, `event`, `loading` and `prompt` are
Longshot's Link snapshot (the control bridge's `status`, `game`): metres, yaw in degrees (0 toward +z, 90 toward +x),
quarter hearts. `snapshot_present` is Longshot's present count when it was taken (Link's last update; on a title,
pause or load it is older). `control` is false when Longshot's bridge did not answer (those fields are then `null`). `equips`, `inventory` and `coop` come from the module. A field it cannot read is `null`; `room`
stays `null` until the room is found in memory.

Walking: the stick is camera-relative. To walk toward world yaw `w` (`atan2(dx, dz)` in degrees), push the stick at
angle `w - camera` (the sign is settled by the LANE1 proof run; `walk_to` learns it from how Link moves). The camera turns as Link walks, so steer in short `input` steps (5 to 10 frames) and read `camera`
again each time.

## On a test lane: the lock and the watchdog

`live.py start --lane N` takes the lane's game pair (`<kit root>\ls\laneN-A` / `-B`), its server port and its control
pipes (`coop-laneN-A` / `-B`) from `tools/autotest/lanes.py`, and holds the lane's lock (`laneN.lock`) for the whole
session: the games stay open after the boot, so the lock must too. A busy lane refuses the session at once
(`--lock-wait S` waits for it). The lock is held by the session's watchdog (`live.py watchdog`, a process of its own
that `start` starts), under the watchdog's own pid. The watchdog ends the session when:

- `live.py stop` asks;
- the maximum duration is reached (`--max-minutes`, 30 by default: a lane slot, not an evening);
- no tester command came for `--idle-minutes` (10 by default; counted from the end of the boot): the tester handed
  off, crashed or paused;
- a game ended by itself;
- the watchdog is stopped by a signal, or fails.

Then it asks both games to quit, kills what is still running 15 s later and the server, and lets the lock go;
`watchdog.json` in the session folder says why and what happened. Commands on an ended session are refused. A
watchdog killed outright cannot clean up, but the lock names its pid, so the lock is stale as soon as it is gone.
Without `--lane` the same watchdog runs (the end rules and the clean-up), with no lock.

The lane rules still hold: one game run at a time on the lane, only in a slot the coordinator gave, no local builds
or tests while it runs.

## Where the game windows go

`live.py start` puts both game windows side by side on one monitor and does not take the focus. Choose the monitor
with `--display \\.\DISPLAY2` (a device name), or the environment variable `OOT_TEST_DISPLAY`; the default is
`\\.\DISPLAY2`. A name that no monitor has stops the start with the list of monitors. Details and what to check:
[testing.md](testing.md#where-the-test-games-windows-go). `live.py shot` and the MCP `shot` tool go through the
game's own control bridge, so they work wherever the windows are.

## Rules for a tester

- With the play-test bridge on ([Bridge protocol](#bridge-protocol)) the games are held between calls: nothing
  happens unless you act. Without it (the older `act` path) they run in real time between steps: put the moves that
  must happen together in one `act`.
- Set a test up with `warp`, `place` and `setflag`; do not walk across Hyrule to reach it. `warp` takes a real scene name (`Market_Child`, not `Market`); an unknown one is refused with an `unknown scene` line and nothing is written ([testing.md](testing.md)).
- Take a `checkpoint` on both games after every action that should sync, then run `desync_check`.
- Use `state` to look; `shot` only when the picture matters (a puppet in the wrong place, a door drawn open). A
  checkpoint takes its own shots: anything visible is proven by them, never by flags or log lines alone.
- A report says: the scenario, the steps, what was expected, what happened, and the first checkpoint that differs.
- A test run reports; it does not change the mod. Fixing is a separate session, so a run cannot quietly hide what it
  was meant to catch.

## Proven (2026-11-22)

Both test copies booted into the Forest Temple and left open (`live.py start`); `state` read both games (map, place,
life, rupees, age); `do A "goto ..."` walked A to within 0.4 m of the point; `checkpoint` wrote 1204 shared flags
from each game; `setflag ForestTemple_Room18_Picture_Shot 1` in A alone was 1 in both by the next checkpoint, and
`desync_check` said every checkpoint agreed. The MCP server answered `initialize`, `tools/list` and real `state` and
`desync_check` calls against the running games.

## Not there yet

- **Frame step in a game.** The play-test bridge holds the game in the module ([Bridge protocol](#bridge-protocol));
  that is not proven in a game yet. The LANE1 proof run for 0.11.12 checks it first.
- **Enemies and owners in the state.** The state has Link's side only. The module's debug walk already reads every
  actor's name, place, life and animation, and the objects sync knows each one's owner; a `dump` of those beside the
  flags is the next step (then the checker compares positions and owners too).
- **More than two games** (the runner starts A and B).
- **Lag on purpose.** The runner's `--test-delay-ms` and `--test-jitter-ms` give the server a delay; a live session
  can pass them through `start` once it takes them.

## Scenarios worth running first

- Both players open the same chest in the same second.
- One player picks up an item while the other changes scene.
- Both players hit the same switch, or kill the last enemy of a room.
- One player disconnects in a dungeon and comes back.
- One player saves and reloads while the other plays on.
- The same flag set by both players with the server's delay on.
- Child and adult Link in different eras at once.
- Tonight's survey fixes ([boss-multiplayer.md](boss-multiplayer.md), "Whole-game survey"): the Forest Temple
  basement floor turned by one player, a Lizalfos pair's owner, a Like Like grab while another player is near.

## Playing through the bridge: one MCP server per game

With the play-test bridge on (see "Bridge protocol"), each game gets its own MCP server,
`tools/autotest/mcp_playtest.py --player pN`. The server keeps one bridge connection open for as long as it runs. It
holds the game at its first call, so **the game is paused between calls**: it runs only the frames a call asks for.
When the server exits, the connection closes and the game runs again (so does a crashed tester's). Standard library
only, no `pip install`.

| Tool | What it does |
| --- | --- |
| `get_state` | Scene, room, Link's position `[x, y, z]` in metres, facing and camera yaw (0 = +z, 90 = +x), life, rupees, age, equips, inventory, co-op status, event, loading, A prompt. No frame passes |
| `walk_to x z [tolerance] [max_frames]` | Walks to the point 8 frames at a time, stopping on arrival (0.5 m by default), a scene change, a load, an event or `max_frames` (600). It says why it stopped, the frames used and the distance left |
| `press [buttons] [stick_angle stick_strength] [frames]` | Holds the buttons (and the stick: 0 = up, away from the camera, 90 = right) for N frames (2 by default), then holds the game |
| `step frames` | Lets N frames run with no input |
| `warp scene [entrance]` | The game's own scene change; it returns once Link is there with no event or load |
| `flag name [value]` | Sets a flag (name or `0x` hash; a number, true/false or an enum name) inside the held frame; replies with the old value |
| `give item` | Gives the item through the cheats queue: it shows after the next frame (`step 1`) |
| `checkpoint tag` | The game's `dump` (every shared flag) with its frame and state, appended to `<log dir>/<player>.checkpoints.jsonl` |
| `desync_check [all]` | `desync_check.py --clients` on the log folder: tags matched across every game, each game's frame printed |
| `log [match]` | The game's new `coop:` log lines since the last call (`match`: only lines with that text). Needs `--session`: games from `live.py start`, p1 = A, p2 = B |
| `pause`, `resume` | Hold the game, or let it run in real time (a person takes over; the next call holds it again) |
| `screenshot [name]` | A PNG of this game: the game saves it as `<game>/playtest/shots/<player>-<name>-<time>.png` (it runs one frame to show it); a copy goes to `<log dir>/shots/` |

Every reply carries the game's `frame`.

**walk_to's stick.** The stick is camera-relative. walk_to starts from the bridge's model (walk toward world yaw
`camera + stick angle`). It learns the real offset and whether the stick is mirrored from how Link actually moves on
each push, and reads the camera again after every 8 frames. On a bend the right model shows itself within a few
pushes. A walk that gets nowhere for three pushes steps aside for three. Into a wall it times out: walk_to does no
pathfinding, so give it points with a clear line between them and use `warp` for anything far.

### Setting up a run

1. Start the games with the bridge on, one port each: player N on `5000 + N`, or `5000 + 10*L + N` on test lane L.
   `live.py start --playtest [--lane L]` does it for its two games (A = p1, B = p2: the runner's
   `--playtest-ports BASE` sets each game's `LICHII_COOP_PLAYTEST_PORT`, and `LICHII_COOP_CHEATS=1` for `give`;
   `HOS_CONTROL` was set already), and its answer lists the ports. Give it `--max-minutes` for a long run (30 by
   default). By hand: set `LICHII_COOP_PLAYTEST_PORT`, `HOS_CONTROL` and `LICHII_COOP_CHEATS=1` in each game's
   environment (or `live.py start --env A:LICHII_COOP_PLAYTEST_PORT=5011 --env B:...`). Each game's log says
   `coop: playtest: listening on 127.0.0.1:<port>`.
2. Register one MCP server per game, all with the **same** `--log-dir` (desync_check reads every game's log there),
   and, for games from `live.py start`, the same `--session`: each call then counts as a tester command for the
   session's watchdog (else it ends the session after `--idle-minutes`), and an ended session is refused. In a
   project `.mcp.json` (the shape most MCP clients read):

   ```json
   {"mcpServers": {
     "oot-p1": {"command": "python", "args": ["-I", "tools/autotest/mcp_playtest.py", "--player", "p1", "--lane", "1", "--session", "sessions/run1", "--log-dir", "playtest-logs/run1"]},
     "oot-p2": {"command": "python", "args": ["-I", "tools/autotest/mcp_playtest.py", "--player", "p2", "--lane", "1", "--session", "sessions/run1", "--log-dir", "playtest-logs/run1"]}
   }}
   ```

   `--port N` names a port outright. Three or four players: `oot-p3` and `oot-p4` the same way (games started by
   hand: `live.py` starts two).
3. The tester follows [AGENTS.md](../AGENTS.md), "Play-testing the game", and runs [tests/scenarios.md](../tests/scenarios.md)
   in order.

After a scenario, `python -I tools/autotest/desync_check.py <log dir> --clients` gives the same answer as the
`desync_check` tool (exit 0 agree, 1 desync, 2 nothing compared, 3 incomplete).

### Rules for a tester on the bridge

- The games are held between calls. Nothing happens unless you act. Hold **every** game: a game whose server
  you have not called yet still runs (call `get_state` on each first).
- Set a test up with `warp`, `flag` and `give`. Do not walk across Hyrule.
- Take a `checkpoint` with the same tag on **every** game after each action that should sync. Let the other game
  run a few frames first (`step 30`): a flag reaches it over the network, and a held game receives nothing. Then
  run `desync_check`.
- Use `get_state` to look and `screenshot` only when the picture matters. Anything visible is proven by a picture,
  never by flags alone.
- A bug report has: the scenario, the steps (tool calls), what was expected, what happened, the first tag that
  differs with each game's frame, and the shots.
- Never edit the mod's source during a test run. Report; fixing is a separate session.

## Lag on purpose

Two games on one PC really go through the network stack. Each game's co-op module connects to the server with ENet
(UDP) at `127.0.0.1:<server port>` (`lichii/hook/coop_hook.cpp`, `coop/net/src/client.cpp`), so its packets pass
through loopback sockets like any other program's. The play-test bridge is TCP on loopback too: shape only the
server's UDP port, or the tester's own calls are delayed as well.

- **Longshot (the kits): the server's test delay.** `coop_server --test-delay-ms N --test-jitter-ms J` with
  `OOT_COOP_TEST=1` holds every packet (N is the round trip, half each way). `run.ps1 -Runtime hookshot -Lag N -Jitter J`
  and `groups.py --lag N` pass it on ([testing.md](testing.md), "On Longshot: the server's test delay"). This is the
  repeatable choice: same rules every run, nothing to install.
- **Windows, any build: Clumsy.** Filter on the server's port only, for example
  `udp and (udp.DstPort == 27511 or udp.SrcPort == 27511)` (Clumsy sees loopback packets: both directions go
  through the loopback interface). Then lag, drop, out-of-order. It needs administrator rights (WinDivert), so it
  is for a person's PC, not an unattended lane run.
- **Linux: tc netem on lo, on the server's port only.** Plain `tc qdisc add dev lo root netem delay 150ms loss 5%`
  delays every loopback packet, the bridge included, and each packet crosses `lo` twice (out and in), so it doubles.
  On the port only:

  ```sh
  sudo tc qdisc add dev lo root handle 1: prio
  sudo tc qdisc add dev lo parent 1:3 handle 30: netem delay 75ms loss 2.5%
  sudo tc filter add dev lo parent 1:0 protocol ip u32 match ip protocol 17 0xff match ip dport 27511 0xffff flowid 1:3
  sudo tc filter add dev lo parent 1:0 protocol ip u32 match ip protocol 17 0xff match ip sport 27511 0xffff flowid 1:3
  # ... run the scenarios ...
  sudo tc qdisc del dev lo root
  ```

  Each direction gets 75 ms here, so the round trip is about 150 ms.
- **The emulator build: `tools/netsim/netsim.py`**, the UDP relay ([testing.md](testing.md), "Bad connections").

Run the same scenario without lag first, then with it, and compare the two `desync_check` reports.
