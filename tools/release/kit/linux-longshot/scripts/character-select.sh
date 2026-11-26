#!/usr/bin/env bash
# uninstall-character-select.sh and reinstall-character-select.sh (owner, 0.11.13; kit-paths.ps1 Set-KitCharacterSelectFiles):
# Snail's mod (Character Select, the - menu's Characters page and its NPC characters) out of the game, or downloaded and
# made again. This only sets the kit's answer; start-squadala.sh, run next, does the work in build.sh charsel_step (the
# same install, rebuild and undo as any start). Saves, settings and your character pick stay. $1: uninstall | reinstall.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
do=${1:-}
[[ $do == uninstall || $do == reinstall ]] || { echo 'usage: character-select.sh uninstall|reinstall' >&2; exit 2; }
[[ -f $DATA/longshot/CHARACTER-SELECT.txt ]] || { echo 'Nothing was changed: this kit has no Character Select (no data/longshot/CHARACTER-SELECT.txt).'; exit 1; }
kit_game_running && { echo 'Nothing was changed: the game is running. Close it first, then run this file again.'; exit 1; }
ini=$DATA/coop-options.ini
if [[ $do == uninstall ]]; then
    rm -f "$DATA/character-select-reinstall.txt"
    printf 'off\n' >"$DATA/character-select-answer.txt"
    [[ -f $ini ]] && sed -i 's/^[[:space:]]*character_select[[:space:]]*=.*/character_select = off/' "$ini"
    echo 'Character Select (Snail'"'"'s mod) is taken out at this start: the game is built again without it (a few minutes),'
    echo '  then it starts. Your saves and settings stay. It is not put back by itself: reinstall-character-select.sh does that.'
else
    printf 'reinstall\n' >"$DATA/character-select-reinstall.txt"   # (charsel_step: the copy set aside, the zip fetched again)
    rm -f "$DATA/character-select-answer.txt"
    [[ -f $ini ]] && sed -i 's/^[[:space:]]*character_select[[:space:]]*=.*/character_select = ask/' "$ini"
    echo 'Character Select (Snail'"'"'s mod) is downloaded and made again at this start (a few minutes): it asks whether to'
    echo '  include the NPC characters (press y to download them, n to skip). Your saves and settings stay.'
fi
echo
