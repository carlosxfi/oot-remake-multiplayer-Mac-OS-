# Native pause menu playtest

This branch connects the shared Multiplayer page to the game's **Pause → System → Multiplayer** entry. Once installed, launch **OOT Co-op** in Finder. Open the game's pause menu and use the controller to select Multiplayer.

The playtest needs a local nxnative app built with the controller bridge in [pause-menu-input.patch](pause-menu-input.patch) and a co-op overlay built with the pause menu assets. The patch targets nxnative commit `a63ca65115cbd586ccca3bedc4af89984eddf2e5`. Keep game files and generated output local. [nxnative build instructions](https://github.com/seedds/nxnative/tree/a63ca65115cbd586ccca3bedc4af89984eddf2e5)

## Developer setup

1. Apply the patch to a checkout of that nxnative commit. Build the game as a Mac app using nxnative's regular packaging command with `--platform mac-app`.
2. Generate a fresh co-op overlay with the pack builder and add `--pause-tab` to the options in the [Mac setup guide](../../README.md). Confirm it reports that it built the menu assets. The pack builder can otherwise leave them out and continue.
3. Run the Mac setup guide's launcher installer with the patched app and the new overlay. Start **OOT Co-op** as usual. If either part is missing, the playtest retains the earlier in-window panel.

## What to test

- Pause in solo play, move to **System**, and open **Multiplayer** with the controller. Confirm **B** returns to System and that the existing pause pages still work.
- Host a LAN session, or join a Windows v0.11.8 host, and verify both players can see each other move. Test controller focus, the session status, and leaving.
- If the button is missing, check the nxnative log for `native pause-menu bridge installed`, `native pause-menu bridge refused`, or `native pause menu needs the controller bridge and pause-menu pack`. Keep invite codes and room passwords out of reports.

## Playtest priorities

1. **Passed on Apple Silicon.** Launch OOT Co-op from Finder, allow Local Network, and join a Windows v0.11.8 host from the in-game menu. A second launch and join passed with Terminal closed.
2. **Performance retest passed on M4 Pro.** Connected gameplay improved from about 18 FPS to 60 FPS at 2560×1440 with the Windows player visible after spreading routine flag polling across eight frames. Mac → Windows heart capacity sync passed when loading a four-heart save. Shared chest, key and story flag checks are still pending. Watch for FPS drops across loading transitions and longer sessions.
3. **Session lifecycle.** Verify Windows can join a Mac-hosted room, then test leave, reconnect and scene transitions in both directions.
4. **Gameplay and presentation parity.** Exercise shared progress and combat, then verify keyboard input, menu layout, tunic colors, overhead names, minimap markers and animation.

## Developer-recommended multiplayer tests

- [ ] Leave and rejoin mid-dungeon. Check that keys, opened chests, and unlocked doors agree on both clients.
- [ ] Have one player warp or play the ocarina while the other enters a loading zone. Swap roles and check that both can continue playing and see each other when back in the same area.
- [ ] Trigger Game Over and test save/load on Mac and Windows separately using disposable saves. Check heart capacity, shared progress, and whether the session recovers.
- [ ] Fight a boss together. Check damage, defeat, rewards, and room state on both clients.
- [ ] If a third player is available, repeat join, scene transitions, and leave/rejoin. Check that both remaining players stay connected and watch Mac FPS with all three present.

## Host-save loading on Mac

Apply [host-save-remount.patch](host-save-remount.patch) to the same pinned nxnative checkout as the controller patch. Repackage the app with the same game input and existing build cache so it includes the updated Mac host. The existing co-op module discovers the save-root and remount exports at startup. Use a pack built with `--title-rows --pause-tab` for the title-screen test.

Open a Windows host session with a saved file, then join from the Mac title screen. Wait for the host-save ready notice before loading a file. Verify the guest loads the host's location and heart capacity, then quit, relaunch without joining, and verify the Mac's original slot. The guest uses a sibling `save-coop` folder. In-session host-save refreshes wait until the next title screen on Mac.

**Windows → Mac host-save transfer passed.** Joining from the Mac title screen received the Windows save, mounted the separate `save-coop` folder, and automatically loaded the newest slot. The guest reported the host’s inventory and four heart slots. The original Mac solo save was still available after relaunch. After rejoining Windows, the guest’s slingshot ammo returned to 30 after previously spending one shot. This is consistent with loading a fresh host snapshot. Ammo remains per player during the session. One-step paste and join on the title screen is still pending.

### Personal inventory on rejoin

The Mac launcher asks for an optional profile name before launch. Use the same name for the same campaign and a different name for a different host or save slot. Join from the title and load the host's save. The Mac checkpoints seeds, arrows, bombs, nuts, sticks, rupees and bottle contents while the guest is playing the co-op save. On a later join with the same profile, it restores those counts into the local copy of the host's save, up to the capacities in that save. It restores bottle contents only where the host save has the bottle. The first join uses the host's starting values. Existing inventory profiles are read and upgraded when written. The profile is stored beside the game's save folders in `coop-inventory`. Shared progress and the host's inventory still follow the existing co-op rules.

To test, start both players at 30 seeds. On the Mac, enter a new profile name, join from the title, spend one seed and wait a second. Leave and rejoin, then repeat after fully quitting the Mac app. The Mac should have 29 and the host should have 30 both times. Try a different profile and confirm it starts with the host's counts. Launch with no profile and verify the original Mac solo save.

### Character Select on Mac

Apply `character-select.patch` to the pinned nxnative source used for the Mac game build. The patch accepts IPS32 writes in section padding between read-only and writable data, as used by Character Select. Set
`DM_COOP_CHARACTER_SELECT_IPS` to the absolute path of the compatible IPS32 prepared by the Windows
co-op kit (its copy with the conflicting customizer code removed), then run the normal nxnative Mac
app build. Keep the IPS's original build-ID filename. The rewriter checks that ID against the game's
main NSO, applies the patch before translation and includes the IPS in the rewrite cache key. The
result carries a capability marker in its packaged fixup metadata. A regular build without the
environment variable uses the original game code.

Install the resulting app with a **prepared combined ROMFS overlay** containing the Character
Select assets and the Mac co-op pack built with its character catalog. The installer uses that
overlay at launch, and the launch script checks the app's marker and the pack's character manifest
before advertising Character Select to the co-op module. It does not download the mod or assemble
the combined overlay. This path needs a live Mac test before use as a release build.
