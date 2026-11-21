# Nucleus Co-op handler for Squadala!

`OoTCoop.js` is a handler for [Nucleus Co-op](https://github.com/SplitScreen-Me/splitscreenme-nucleus), a free Windows tool that runs several copies of a PC game split-screen on one PC, each with its own controller. With it, 2 to 4 players on one PC start Squadala! on Longshot in one click: player 1's game hosts on this PC, and the others join it. With **Who can join: Allow online players**, friends on other PCs can join the same session too, with an invite code.

How to play with it: [docs/play-lan.md, "Split screen (experimental)"](../../docs/play-lan.md#split-screen-experimental).

The handler is not on the Nucleus handler hub. Install it by hand, as described below. Each kit has a copy in its `nucleus` folder (with a short `README.txt`).

## Install

1. Get Nucleus Co-op from its [releases](https://github.com/SplitScreen-Me/splitscreenme-nucleus/releases) (`NucleusApp.zip`, version 2.4.2 or later) and unzip it to a short folder, for example `C:\Nucleus`.
2. Copy `OoTCoop.js` (the kit's `nucleus\OoTCoop.js`, or this folder's) into its `handlers` folder (`C:\Nucleus\handlers\OoTCoop.js`).
3. Start `NucleusCoop.exe`, click the **+** button (**Add game**; not the handler hub, which does not have this handler) and pick the `longshot.exe` of your kit's co-op game. That is `<kit>\hookshot\game\longshot.exe`, or the folder that `hookshot\game-folder.txt` names. Nucleus lists it as **OoT Co-op (Longshot)**.

## What it does at each start

For each player, Nucleus makes an instance folder (`C:\Nucleus\content\OoTCoopLongshot\Instance0..3`) with links to the game's files. To make those links, Nucleus runs its StartGame helper as administrator, so Windows asks for admin rights once. The instance folders are kept afterwards (`Game.KeepSymLinkOnExit`), so later starts make no links and do not ask. Then `Game.Play` writes these files into each instance folder:

| File | What it holds |
|---|---|
| `coop.ini` | The session's server and port (this PC only: `127.0.0.1`, port 27590; online: below), the player's Nucleus nickname (1 to 16 characters, else `Player N`), and the session's join password. Player 1's start makes the session and keeps it in `nucleus-players\session.txt` beside the game (a line each: password, server, port) |
| `coop-options.ini` | The kit's `coop-options.ini` (or the game's own copy), plus this game's controller: `LICHII_COOP_PAD=<Nucleus's pad number>` and `LICHII_COOP_KEYBOARD=off`. The keyboard player gets `LICHII_COOP_PAD=none` and `LICHII_COOP_KEYBOARD=focus`. Players 2 to 4 have no `color` line, so each gets its slot's colour |
| `user` | A junction (no admin needed). Player 1's points to the game's own `user` folder, so player 1 plays their own saves. Player N's (N from 2 to 4) points to `nucleus-players\playerN\user` beside the game. A guest plays from its co-op save folder, which is filled with the host's save when it joins |
| `player-key.txt` | Player 1's own key, or a key kept for player N in `nucleus-players\playerN` (the Lobby's mutes and bans follow it) |
| `longshot_options.ini` | A copy of the game's own, with `fullscreen=0` (Nucleus places the windows) and `discord_status=0` |

Player 1's start also opens the kit's `server\coop_server.exe --port 27590 --bind 127.0.0.1 --max-players 4 --password <session password>`. The server listens on this PC only, so Windows shows no firewall prompt and nobody else can join.

With **Who can join: Allow online players** (the handler's option on Nucleus's setup screen, `Context.Options["Online"]`), player 1's start runs the kit's `start-server.cmd <port>` in a window of its own (`cmd /c start "Squadala server"`) instead, the same server start as `Host.bat` (`scripts\start-server.ps1`): `coop_server` on every address of this PC with `--upnp` and `--invite` (unless `coop-options.ini` says `upnp = no` / `invite = no`), the invite code shown in that window and copied to the clipboard, and a log in the kit's `logs` folder. The port is the kit's (`port` in its `coop.ini`, else 27510). The join password is `server_password` from the kit's `coop-options.ini`, else a new one, handed to `start-server.ps1` in `OOT_COOP_SESSION_PASSWORD` as `Host.bat` does (never on a command line; the handler removes it from Nucleus's environment right after). `server_bind` goes along as `-Bind`, and then the players on this PC use that address; `server_allow` goes along as `-Allow` with `127.0.0.1` added. The players on this PC join at `127.0.0.1` (or the bound address); friends on other PCs paste the invite code under **Join** in their own game. Anything but **Allow online players** (no choice yet, an older Nucleus) is this PC only.

If a `coop_server.exe` already runs on this PC when player 1 starts (`Host.bat`, `start-server.cmd`, a session not ended), the handler starts no server and says so (a message box, else Nucleus's log), in both modes: that server has another password or holds the port, and it is left alone (friends may be on it). The online server window runs `start-server.cmd` through `cmd /c`, so it closes with the server.

When the session ends, Nucleus closes every `coop_server.exe` on this PC (`Game.KillProcessesOnClose`).

Each game reads its own controller and keeps reading it while another window is in front (patch 0015, `LICHII_COOP_PAD`). So the handler uses none of Nucleus's input hooks (no x360ce or XInput DLLs in the game folder). It does say it uses XInput (`Game.Hook.XInputEnabled = true`): that only makes Nucleus list the controllers on its setup screen and give each player its pad number; the DLLs come only with `Game.Hook.CustomDllEnabled`, `Game.XInputPlusDll`, `Game.UseX360ce` and `Game.ProtoInput`, all off. The module reads `coop-options.ini` beside `longshot.exe` because the instance folder has no kit of its own (`CoopTitle::ApplyKitOptions`). A kit counts only when it is the game's own: the game is its `hookshot\game`, or `OOT_COOP_KIT` or `coop-kit.txt` names it. So Nucleus unzipped inside a kit folder works too. A co-op game built before that change gives every copy the same controller: build it again (`Build-Hookshot-Coop.bat`).

What stays linked: the game's big files (`longshot.exe`, `romfs`, `mods` and the shader and pipeline caches). The game saves a cache by writing a `.tmp` file and moving it over the old one. That puts a file of the instance's own in its folder and leaves the original as it was, so two games never write the same file.

## Troubleshooting

- **No controllers on Nucleus's setup screen** (the handler of 0.11.1 or older): that handler had XInput off, and Nucleus then lists no pads at all. In your copy (`C:\Nucleus\handlers\OoTCoop.js`) change `Game.Hook.XInputEnabled = false;` to `Game.Hook.XInputEnabled = true;` (or take the newer `OoTCoop.js` from the next release), then restart Nucleus. Nucleus lists Xbox-style (XInput) controllers; a PlayStation or Switch controller needs a tool such as DS4Windows that makes it look like an Xbox controller.

## Test

Windows only, nothing to install:

```
cscript //nologo //E:jscript tools\nucleus\test_handler.js
```

It runs `Game.Play` for three players (two controllers and one keyboard) against a stand-in of Nucleus's `Context` on a made-up kit in `%TEMP%`, then checks every file above, including real junctions, and both choices of **Who can join** (the server window's command line, the kit's port, password, bind and allow lines, `session.txt`). It prints `ok: N checks` and exits with 0 when all pass. It cannot check Nucleus itself: how it places the windows, its pad numbers, its option screen, and the admin prompt.
