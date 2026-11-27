#!/usr/bin/env bash
# Takes Snail's mod (Character Select: the - menu's Characters page and its NPC characters) out of the game;
# the start that follows builds the game again without it. It stays out until reinstall-character-select.sh.
# Close the game first. Your saves, settings and character pick are kept. Then it runs start-squadala.sh.
# In a terminal:  bash uninstall-character-select.sh
here=$(cd "$(dirname "$0")" && pwd)
bash "$here/scripts/character-select.sh" uninstall || exit 1
exec bash "$here/start-squadala.sh" "$@"
