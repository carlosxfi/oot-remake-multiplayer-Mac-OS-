Split screen with Nucleus Co-op (experimental, Windows): 2 to 4 players on this PC, one game each
==============================================================================================

OoTCoop.js in this folder is the Nucleus Co-op handler for Squadala!. Each player runs a full game:
about 10 GB of RAM per player and a strong graphics card. Build the co-op game first (Start Squadala.bat).

Once:
1. Download NucleusApp.zip (2.4.2 or later) from
   https://github.com/SplitScreen-Me/splitscreenme-nucleus/releases and unzip it to a short folder,
   for example C:\Nucleus.
2. Copy OoTCoop.js from this folder into C:\Nucleus\handlers.
3. Start C:\Nucleus\NucleusCoop.exe and click the + button (Add game). Not the handler hub: this
   handler is not on it. Pick the longshot.exe of this kit's co-op game: hookshot\game\longshot.exe
   in this kit, or the folder that hookshot\game-folder.txt names. Nucleus lists it as
   "OoT Co-op (Longshot)".

Each time:
4. Plug in the controllers, start Nucleus and click "OoT Co-op (Longshot)".
5. Drag each controller (and the keyboard, for a keyboard player) onto a part of the screen and give
   each player a name.
6. Who can join: "This PC only" (the players on this PC), or "Allow online players" when friends on
   other PCs join too.
7. Press Play. The first time, Windows asks to let Nucleus make changes (the links to the game's
   files); later starts do not ask. The games start about 15 seconds apart and join by themselves.
8. With "Allow online players": the window "Squadala server" shows an invite code after a few seconds
   and copies it to the clipboard (Alt+Tab to it). Send it to your friends in DMs only. Each friend
   opens Join in the Multiplayer tab of their own game, pastes it and chooses Join. The first
   time, Windows may ask to let coop_server.exe through its firewall: allow it.
9. To stop, end the session in Nucleus: it closes the games and the server.

More: docs\SETUP.md, and on the web docs/play-lan.md, "Split screen (experimental)", and
tools/nucleus/README.md in https://github.com/daurosk/oot-remake-multiplayer
