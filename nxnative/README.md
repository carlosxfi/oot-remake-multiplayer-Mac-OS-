# nxnative bring-up

This folder holds the smallest proof needed before the co-op module is wired
into nxnative: load a host-side dylib into the Mac app and read the local
player through the same `coop::host::GameMemory` addresses used by the other
hosts. It changes no renderer code and contains no game data.

## Position probe

Build and run it on the Mac that built the nxnative app:

```sh
chmod +x nxnative/probe/build.sh nxnative/probe/run.sh
nxnative/probe/build.sh
nxnative/probe/run.sh "$HOME/Downloads/The Legend of Zelda - Ocarina of Time.app"
```

The exact app name may contain a different dash. Drag the app from Finder into
Terminal after typing `nxnative/probe/run.sh ` to avoid that problem.

The terminal should first print `position probe loaded`, then `waiting for a
valid player state`. Load a save. A successful proof prints Link's age and
position twice a second.

The probe discovers nxnative's ASLR-selected `main` base from its loader table.
The override is only for diagnosing a loader whose symbols are unavailable:

```sh
COOP_NXNATIVE_MAIN_BASE=0x... nxnative/probe/run.sh "/path/to/game.app"
```

If macOS reports that the inserted library is blocked, run the executable from
this script rather than opening the app through Finder. Do not remove signing
or security settings for this test.

## After the proof

Replace the probe's direct reads with a `GameMemory` adapter and link
`coop_host`, `coop_net`, `coop_presence`, and the protocol into the dylib.
Keep puppets and game calls out until the local sampler and network client run
reliably. The later nxnative host bridge needs bounded read, write, function
call, frame callback, input, and overlay operations; the Metal renderer stays
unchanged.
