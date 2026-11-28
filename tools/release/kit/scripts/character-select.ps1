# "Uninstall Character Select.bat" and "Reinstall Character Select.bat" (owner, 0.12.0): Snail's mod (Character Select,
# the - menu's Characters page and its NPC characters) out of the game, or downloaded and made again. This only sets the
# kit's answer (kit-paths.ps1 Set-KitCharacterSelectFiles); "Start Squadala.bat", which the .bat runs next, does the
# work with its own step (the same install, rebuild and undo as any start). Saves, settings and your character pick stay.
param([Parameter(Mandatory = $true)][ValidateSet('uninstall', 'reinstall')][string]$Do)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'kit-paths.ps1')
$kit = Split-Path -Parent $PSScriptRoot
$exe = Get-KitHookshotExe (Get-KitHookshotGame $kit)
$gameDir = if ($exe) { Split-Path -Parent $exe } else { '' }
$why = Set-KitCharacterSelectFiles $kit $gameDir $Do
if ($why) { Write-Host "Nothing was changed: $why." -ForegroundColor Yellow; exit 1 }
if ($Do -eq 'uninstall') {
    Write-Host 'Character Select (Snail''s mod) is taken out at this start: the game is built again without it (a few minutes),'
    Write-Host '  then it starts. Your saves and settings stay. It is not put back by itself: "Reinstall Character Select.bat" does that.'
} else {
    Write-Host 'Character Select (Snail''s mod) is downloaded and made again at this start (a few minutes): it asks whether to'
    Write-Host '  include the NPC characters (press Y to download them, N to skip). Your saves and settings stay.'
}
Write-Host ''
exit 0
