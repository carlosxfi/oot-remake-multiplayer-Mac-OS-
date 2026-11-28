# Squadala! for macOS

This fork ports [Squadala!](https://github.com/daurosk/oot-remake-multiplayer) to Apple Silicon using nxnative. This Mac build uses the Squadala v0.12.0 protocol (18).

## Build and play on a Mac

You’ll need an Apple Silicon Mac, the Xcode Command Line Tools, CMake 3.24 or newer, and Python 3.10 or newer. You’ll also need a working nxnative game app and your own game data for version 1.1.1. The pack builder accepts a local `.nup` or an extracted RomFS folder. Start the nxnative app once on its own to check that it runs.

If the command line tools are missing, run `xcode-select --install`. If you use Homebrew, you can install CMake and Python with `brew install cmake python`.

**1. Download the Mac source.**

```sh
mkdir -p "$HOME/Projects"
cd "$HOME/Projects"
git clone https://github.com/carlosxfi/oot-remake-multiplayer-macos.git oot-coop-nxnative
cd oot-coop-nxnative
```

**2. Build the local co-op asset pack and install the Finder launcher.**

Replace the two `/path/to/` placeholders with your game app and game data paths. Keep the quotes around the paths.

```sh
(
  set -e
  GAME_APP="/path/to/your/nxnative game.app"
  GAME_SOURCE="/path/to/your/game.nup"
  PUPPET_DIR="$HOME/Projects/oot-coop-puppet"

  PACK_PYTHON="$HOME/Projects/oot-coop-pack-env/bin/python"

  python3 -m venv "$HOME/Projects/oot-coop-pack-env"
  "$PACK_PYTHON" -m pip install zstandard
  "$PACK_PYTHON" -m tools.puppet_pack.build --romfs "$GAME_SOURCE" \
    --mod-dir "$PUPPET_DIR" --no-hud --look-slots 0

  sh nxnative/network/install-menu.sh "$GAME_APP" "$PUPPET_DIR/romfs"
  open "$HOME/Applications/OOT Co-op.app"
)
```

The installer builds both ARM64 co-op libraries, runs the nxnative checks, and creates **OOT Co-op** in your home folder’s **Applications** folder. The launcher uses the paths you supplied and the build in this checkout. If you move those folders, run the installer again with the new paths.

**3. Host or join a LAN playtest.**

Open **OOT Co-op** from Finder and choose **Multiplayer → Host or Join** in the macOS menu bar. To join, enter the host address, port, and room password, or paste the complete LAN invite into the address field. To host, choose **Host LAN**, then **Copy invite**. Keep invites private. For cross-play testing, use disposable saves and start with both players in the same scene. [Windows v0.11.12 release](https://github.com/daurosk/oot-remake-multiplayer/releases/tag/v0.11.12)

The port is in development. The menu, native build, and automated networking checks are in place. A live join launched from Finder can stall at **Connecting**. Use the panel status in your report. Host-save handoff is disabled in this launcher.

### Update an existing install

Quit the game first. Update your checkout to `main` and rebuild the libraries used by your Finder launcher. This also works if you previously installed from `macos-squadala-v0118`.

```sh
(
  set -e
  cd "$HOME/Projects/oot-coop-nxnative"
  git remote set-url origin https://github.com/carlosxfi/oot-remake-multiplayer-macos.git
  git fetch origin
  git switch main
  git pull --ff-only origin main
  sh nxnative/network/build-shared.sh
  open "$HOME/Applications/OOT Co-op.app"
)
```

If you cloned elsewhere, use the path to your checkout.

## Help with testing and development

- **Report a Mac issue.** Label the report **macOS/nxnative** and include your Mac model, macOS version, the short commit from `git rev-parse --short HEAD`, your game version, the host platform and co-op release, and the steps that reproduce the problem. For a join issue, include the exact panel status and whether you launched from Finder. The Mac log is at `~/Library/Logs/nxnative/0400486024DEC000.log`. Share only the relevant lines with addresses, player names, passwords, and invite codes removed. Keep game files, extracted assets, and saves local. [Open an issue on this Mac fork](https://github.com/carlosxfi/oot-remake-multiplayer-macos/issues)
- **Send a PR.** Fork this repository, start your topic branch from `main`, and select `main` as the base branch when you open the PR here. Keep each PR focused. Say what you tested and what still needs an in-game check. Run `sh nxnative/network/build-shared.sh` on Apple Silicon and `python3 tools/check_content.py` before submitting. The automated Mac build can run without a game app or game files. [Pull requests](https://github.com/carlosxfi/oot-remake-multiplayer-macos/pulls) · [Apple Silicon CI](https://github.com/carlosxfi/oot-remake-multiplayer-macos/actions/workflows/macos-nxnative.yml)
- **Pick a feature to work on.** Start with the Finder join stall, player lifecycle across scene changes and reconnects, or parity work for shared progress, remote animations and equipment, name tags, and minimap markers. The Mac launch and game bridge code is in `nxnative/`. [Developer notes and playtest checklist](nxnative/README.md)

## Credits and license

This port builds on the work of the [upstream Squadala contributors](https://github.com/daurosk/oot-remake-multiplayer) and nxnative. Co-op source is licensed under [GPL-3.0](LICENSE). See [third-party notices](THIRD_PARTY_NOTICES.md).
