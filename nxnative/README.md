# nxnative bring-up

This folder brings the co-op library into nxnative in stages. The Mac dylib
now uses the shared `coop::host::PlayerReader` and `Sampler` through a
read-only `GameMemory` adapter. The default build encodes messages locally.
An optional network build can test the encrypted relay with a local observer.
Neither build applies another player's state to the game yet.

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
chosen version. This first cross-platform check sends player samples,
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
nickname of at most 16 characters (default MacLink), and the room password
with echo disabled. It passes them only to this launch. The loopback check
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

The Mac refuses progress-host takeover and stops if it is selected, because
its shared-progress implementation is not ready. Keep Windows hosting during
the check. Mac-side puppet display and shared progress are separate work.
The LAN mode and its new test still need to run on the Mac.
