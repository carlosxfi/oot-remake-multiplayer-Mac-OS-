#!/usr/bin/env bash
# Downloads Snail's mod (Character Select: the - menu's Characters page and its NPC characters) and makes it
# again, in place of the copy there (also a broken or half-made one). It asks whether to include the NPC characters:
# this is the way to get them later.
# Close the game first. Your saves, settings and character pick are kept. Then it runs start-squadala.sh.
# In a terminal:  bash reinstall-character-select.sh
here=$(cd "$(dirname "$0")" && pwd)
bash "$here/scripts/character-select.sh" reinstall || exit 1
exec bash "$here/start-squadala.sh" "$@"
