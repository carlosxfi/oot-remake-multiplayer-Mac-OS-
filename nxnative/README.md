# nxnative bring-up

This folder brings the co-op library into nxnative in stages. The Mac dylib
now uses the shared `coop::host::PlayerReader` and `Sampler` through a
read-only `GameMemory` adapter. The default build encodes messages locally.
An optional network build can test the encrypted relay with a local observer
or join a Windows host. Its opt-in spawn test can display one remote player.

## Compatibility milestones

The first milestone is a full play session with a Windows host on the same
co-op release. Use the shared co-op core for the gameplay rules and adapt
nxnative's frame, memory, game-call, input, and rendering interfaces around it.
The single-puppet LAN probe remains a reference for the Mac adapter.

- Player lifecycle: join, show every peer, move between scenes, despawn,
  reappear, and reconnect without duplicate actors or crashes.
- Shared progress: pick up items, use dungeon keys, open doors, and reload a
  save without either player losing progress or becoming stuck.
- Shared world and combat: interact with objects, defeat enemies and a boss,
  then leave and return to verify the same state on both clients.

After that milestone, the goal is feature parity with the Windows release.
Compare the Mac and Windows clients for full pose and equipment, name tags,
map markers, customization, hosting and session menus, chat, and voice.
Keep the Windows build and game versions in each test result.

Use a disposable save in Kokiri Forest for the first lifecycle check.
Join a Windows host, walk into Lost Woods and back, repeat with the other
player crossing, then disconnect and rejoin. There should be exactly one
remote actor when both players are in the same scene. For shared progress,
start before a chest or small-key pickup. For combat, start before a boss
and check the saved result on both games after a reload. Keep saves local
and record the observed outcomes and relevant co-op log lines.

## Position probe

Build and run it on the Mac that built the nxnative app:

```sh
chmod +x nxnative/probe/build.sh nxnative/probe/run.sh
nxnative/probe/build.sh
nxnative/probe/run.sh "$HOME/Downloads/The Legend of Zelda - Ocarina of Time.app"
```

The exact app name may contain a different dash. Drag the app from Finder into
Terminal after typing `nxnative/probe/run.sh ` to avoid that problem.

Click Play and load a save. nxnative's launcher redirects the game's output
to its log. In a second Terminal, watch the probe there:

```sh
tail -f "$HOME/Library/Logs/nxnative/0400486024DEC000.log" | grep --line-buffered '\[coop-nxnative\]'
```

The probe starts only in the `dmhost` child, not the settings launcher.
Look for `shared local sampler ready`, `discovered main base`, then Link's
age, coordinates, scene and the availability of rotation, animation and
equipment once per second. `samples`, `packets` and `bytes` are cumulative
counts of messages encoded locally. A `?` means that field could not be read;
it does not imply all sampling failed. No packets are sent over the network.

The probe discovers nxnative's ASLR-selected `main` base from its loader table
when exported, or from the current game's nxnative log otherwise. It checks
that the candidate has a readable MOD0 header inside the loader's reported
module size. MOD0 offsets beyond 16 MB are supported. Rejected candidates
print the offset and reason, so an old log or unreadable mapping is visible.
The default log is `~/Library/Logs/nxnative/0400486024DEC000.log`;
`COOP_NXNATIVE_LOG=/path/to/current.log` overrides it.

The base override is only for diagnosing discovery problems. An address from
a previous launch may belong to a different mapping after restarting:

```sh
COOP_NXNATIVE_MAIN_BASE=0x... nxnative/probe/run.sh "/path/to/game.app"
```

The portable checks need only a C++20 compiler. They cover module discovery,
bounded reads and the shared reader/sampler encoding real protocol messages
from synthetic player memory, including movement and invalid or missing data:

```sh
sh nxnative/probe/check.sh
```

If macOS reports that the inserted library is blocked, run the executable from
this script rather than opening the app through Finder. Do not remove signing
or security settings for this test.

## After the proof

The raw position proof ran on the user's Mac: coordinates changed with Link.
The shared reader/sampler also ran: it reported the scene, rotation, animation
and equipment, with samples increasing while Link moved. Scene transitions
and equipment changes still need more playtesting. These checks do not
establish multiplayer or puppet rendering.

Sampling currently polls about every 16 ms on its own thread. It is not
synchronized with a game frame, and can miss brief equipment requests or read
fields across an update. Packet timestamps use a local diagnostic clock.
Before sending them, integrate `coop_net::Client` and its synchronized server
clock, plus `coop_presence` for received players. A later bridge needs frame
callbacks, writes, game calls, input, and overlay operations for puppets and
shared progress.

## Encrypted loopback check

This opt-in build starts the existing co-op ENet relay on `127.0.0.1` at a
free port, plus a game client and an observer client. Each start uses a fresh
random join password that is never logged. Both clients complete the existing
encrypted handshake. The game client uses its synchronized server clock for
the shared sampler, and the observer checks the relayed player samples.
The relay is bound only to loopback and does not join an existing room.
It does not spawn puppets or apply incoming flags to the save.

Quit the game. Install CMake if missing (`brew install cmake`), then run:

```sh
sh nxnative/network/build.sh
COOP_NXNATIVE_LOOPBACK=1 sh nxnative/probe/run.sh "/path/to/game.app"
```

The first network build fetches the repository's existing pinned ENet and
Monocypher dependencies. The build also runs a portable automated check:
two clients join the real encrypted relay, an observer receives synthetic
player samples, and movement reaches that observer. No game is needed.

After loading a save, use the same log watcher as above. Look for
`loopback game=connected observer=connected encrypted=2`, increasing
`received` and `failures=0`, plus `observer received Link position` following
the live player. `sent` counts accepted outgoing messages; `received` counts
player samples received by the observer. A lone send count proves no delivery.

Without `COOP_NXNATIVE_LOOPBACK=1` the network build starts no relay or clients.
`sh nxnative/probe/build.sh` restores the original offline-only build.
The network build has not yet been checked inside the Mac game.

## Join a Windows host over LAN

The Windows game and this branch must use the same co-op protocol and game
build. The bridge uses the shared protocol constant, not an independently
chosen version. The shared protocol, codec, network client and local test
relay now follow protocol 18, including the host-approval handshake, platform
updates and hit-record message layouts. This first cross-platform check sends player samples,
status and equipment, and receives player samples into presence. It does not
apply incoming progress, warp the player, create puppets, or draw name tags.

On Windows, load a test save, choose Host, and open Same network. Keep that
game running as progress host. Note its local IPv4 address, UDP port and room
password. Enter those on the Mac; do not put the password in Git or logs.

On the Mac, quit the game, rebuild, and use the interactive join helper:

```sh
sh nxnative/network/build.sh
sh nxnative/network/join.sh "/path/to/game.app"
```

The helper prompts for the host address, port (default 27510), an ASCII
nickname of at most 16 characters (default MacLink), and the room password.
The password stays visible so it can be checked before joining. The helper
passes it only to this launch. The loopback check
is disabled for this mode. No router port forwarding is needed on the same
LAN; Windows must allow the host game's UDP traffic on the private network.

The build runs an additional remote-client check with a local relay and an
independent host client: configuration validation, encrypted sample delivery
in both directions, changed coordinates, and a refused wrong password.
These checks must pass before trying the Mac against Windows.

Watch the usual Mac log. Look for `LAN state=connected`, a nonzero `self`,
`peers=1` (one Windows player), increasing `sent` and `received`, and
`failures=0`. `remote player ... position` should follow the Windows Link.
A refused join logs its reason; check the protocol/game version and the room
password. A client still connecting suggests checking the local address,
port, Windows private-network firewall permission, or Wi-Fi client isolation.

When the Windows host requires approval, the Mac log says
`waiting for Windows host approval`. Accept that player's request on Windows.
The Mac waits for the server's approval window instead of timing out at the
normal handshake deadline. The remote-client check covers this wait, verifies
that no player state is sent before approval, then checks two-way delivery.
It also checks that a legacy protocol HELLO is refused and that the new
host-approval and hit-record messages encode and decode correctly.

The injected bridge also installs a build-checked, no-op calc-thread hook at
the recurring co-op frame call in `moduleCalc_`. Its `calc-thread bridge frames` count
must increase while gameplay runs. This proves the next puppet stage can make
game calls from the correct guest thread; this diagnostic stage still makes no
guest calls and writes no player or save state. The hook refuses to install if
the original instruction does not exactly match game build 1.1.1.

The position probe prefers a valid position from the live Link actor and falls
back to the GameUI copy. Move Link and compare the local `Link ... position`
line with `calc-thread bridge frames`. Frame counts are logged in offline and
LAN modes. A growing count with fixed coordinates points toward the position
reader. A fixed count calls for checking the hook while the game is running.

The Mac refuses progress-host takeover and stops if it is selected, because
its shared-progress implementation is not ready. Keep Windows hosting during
the check. Mac-side puppet display and shared progress are separate work.
The LAN mode and its new test still need to run on the Mac.



## Preparing remote Link

The LAN client hands fresh, same-scene poses to the calc-thread callback.
The count appears as `calc-thread remote targets`. With both games in the
same scene it should reach one. It returns to zero after a scene change or
after the remote game's samples go stale.

The callback currently counts eligible poses. Actor creation still needs
nxnative game calls and the equivalent of Hookshot's Link puppet guard.

The network dylib runs a one-time ARM64 call check on the calc thread when
a same-scene target arrives. Look for `calc-thread actor call check=passed`.
The check uses a synthetic function in the dylib to verify the eight integer
register arguments, four stack arguments, float argument, and integer return.
Creating a Link actor still requires loading the co-op actor packs in nxnative
and adapting the Link puppet guard.

## Actor pack and guard preflight

An existing generated co-op mod can be passed to the LAN launcher by giving
its `romfs` directory as the second argument. Keep the whole generated
overlay together, including its resource size table. If you already have a
supported extracted RomFS folder or a .nup locally, the existing builder can
generate a local mod folder:

```sh
python3 -m tools.puppet_pack.build --romfs "/path/to/local/source" \
  --mod-dir "$HOME/Projects/oot-coop-puppet" --no-hud --tunics none --look-slots 0
sh nxnative/network/join.sh "/path/to/game.app" "$HOME/Projects/oot-coop-puppet/romfs"
```

The launcher checks for both Link packs and gives the directory to nxnative
through `DM_ROMFS_OVERLAY`. This start also enables the component-chain guard.
Look for `puppet guard installed (component chain)` in the Mac log. If the
expected instructions differ, the probe logs a refusal and skips the guard.

This check does not create actors. Hookshot's Windows patch also handles other
near-null reads during creation and updates. The component-chain guard only
covers the known hair-component path.

## One-puppet spawn test

Launch with the same generated overlay and opt in for a single experimental
actor request. Use a test save in both games.

```sh
COOP_NXNATIVE_SPAWN_TEST=1 sh nxnative/network/join.sh "/path/to/game.app" \
  "$HOME/Projects/oot-coop-puppet/romfs"
```

After two seconds with the same remote target in the same scene, the Mac
requests one actor for that target. The log reports `puppet spawn test`
messages for the request and result. A live actor follows received positions.
If the remote target leaves, the client requests deletion. On a local scene
change the game handles teardown. Relaunch to try another spawn. This test
applies received rotation and switches between the `Wait` and `Move`
animation commands, then copies the active command into the partial slots
for the upper body and other layers. Other animations and their exact playback frames are
outside this test. The known
component-chain guard is required, but other missing-component crash paths
are still possible during a real spawn.

### Colored puppet packs

For a colored Link, generate the overlay with the pack builder's default
tunic list (omit `--tunics none`). The spawn test uses the remote player's
co-op color and the same nearest-preset selection as the Windows client.
If the selected variant is missing, it uses the plain actor. The log line
`puppet spawn test requesting` shows the selected actor. Color is selected
when making the actor, so relaunch after changing the remote player's color.

## Shared v0.11.8 lifecycle test

Build the shared module and probe, then start the opt-in shared client with
a previously generated puppet overlay and a disposable save:

```sh
sh nxnative/network/build-shared.sh
COOP_NXNATIVE_SHARED_MODULE="$PWD/nxnative/network/build/shared-core/hookshot/hookshot_coop.dylib" \
  sh nxnative/network/join.sh "/path/to/game.app" "$HOME/Projects/oot-coop-puppet-colors/romfs"
```

Look for `shared module active on calc thread`, then the shared module's
connection and actor logs. Walk out of and back into the same scene, then
reconnect. This test uses the calc-thread callback. Present, input, and UI
hooks, atomic guest writes, and save handoff still need Mac integration.

## Mac multiplayer menu

Install the Finder launcher once using the nxnative app and a built puppet
overlay:

```sh
sh nxnative/network/install-menu.sh "/path/to/game.app" "$HOME/Projects/oot-coop-puppet-colors/romfs"
```

Open **OOT Co-op** from your home folder's **Applications** folder. In the
game window, choose **Multiplayer → Host or Join** or press **⌘⇧M**. For LAN
hosting, leave the password blank to generate one and use **Copy invite** to
share the address and password. To join, paste that line in the address box
or enter the address, port, and password separately. The panel shows the
network connection state and any server refusal. Use **Leave** to end the session.
The panel runs the shared co-op host and join functions. The game still uses
its local save during this test.

### Windows v0.11.12 playtest

Use Squadala v0.11.12 on the Windows host. The Mac shared module
uses protocol 18. Start with disposable saves and
check that both players can see each other move. Then test a chest, key and
rupee pickup, an enemy defeat, and a loading zone together. Check player
lists on each platform and record any missing pages or other differences.
After 15–20 minutes, collect a `/bugreport` from each client where available.
For a Mac run without in-game chat, keep the nxnative game log instead.

### In-window multiplayer playtest

On the `macos-playtest-hud` branch, use **Multiplayer → Host or Join** or
press **⌘⇧M** to open the Mac panel inside the game window. Use **Close** or
**Esc** to return focus to the game. The player list appears once peers are
in the session. Each row shows a remote nickname and current/max hearts.
A dash means the peer has yet to send health data.

With a Windows host, check that remote health changes when a peer takes
damage or heals, and that their row goes away when they leave. Try the
panel with the game window resized and in fullscreen. Record any focus
problems, and whether Finder joining still stalls. The panel is operated
with the mouse and keyboard. Overhead name tags are a future step.

### Native pause menu playtest

The `macos-playtest-pause-menu` branch uses the game's **System → Multiplayer**
entry with a patched nxnative app and the local pause-menu asset pack.
See [the runtime setup and playtest checklist](runtime/README.md).
