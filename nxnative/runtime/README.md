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
