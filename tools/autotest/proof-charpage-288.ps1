# Row 288 proof (crash on the Characters page after a seed made in the game and Restart now): a mod whose menus'
# layout archive (romfs\UI\Layout2Archive\Common.Product.*.blarc.zs, here the game's own copy, as a seed ships it)
# sorts last is put into game A, and the games start WITHOUT the kit's merge (OOT_TEST_NO_MERGE=1), as the Insert
# menu's Restart now starts them. Needs the WHOLE PC (a kit-mode run in lane 1). Kits as kit_pair.py setup made them,
# with a Characters tab's code in the game (Character Select ON, or co-op's own).
#
#   proof-charpage-288.ps1 -KitA <kit-A> -KitB <kit-B> -Mode hold     part B: the page is held, no crash
#   proof-charpage-288.ps1 -KitA <kit-A> -KitB <kit-B> -Mode merge    part A: the game merges at its start
#                                                                     (OOT_COOP_START_MERGE=1, OOT_COOP_RESTART=1), the page opens
#   proof-charpage-288.ps1 -KitA <kit-A> -KitB <kit-B> -Mode repro    on a kit WITHOUT the fix (0.11.12): the crash
#
# -Mode hold and merge need the game, the module and the kit's tools built from this branch (merge needs
# hookshot\tools\puppet_pack\merge_tables.py with --in-game: checked). The mod is removed again, always.
# Exit 1 FAIL (hold/merge: the game aborted or the log lines are missing; repro: no crash), 2 UNPROVEN (the log
# lines are right: owner rule, no PASS from log lines alone, read the shots named in the scenario's head),
# 3 the kit is not this branch's.
param(
    [Parameter(Mandatory)][string]$KitA,
    [Parameter(Mandatory)][string]$KitB,
    [Parameter(Mandatory)][ValidateSet('hold', 'merge', 'repro')][string]$Mode,
    [int]$Timeout = 300,
    [string]$Server = ''
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not $Server) { $Server = [System.IO.Path]::GetFullPath((Join-Path $here '..\..\..\integrate-0112\build\server\coop_server.exe')) }
$runPs1 = Join-Path $here 'run.ps1'
$scenario = if ($Mode -eq 'hold') { 'charpage-288-hold' } else { 'charpage-288-open' }
function Get-GameDir([string]$Kit) {
    $note = Join-Path $Kit 'hookshot\game-folder.txt'
    $g = if (Test-Path -LiteralPath $note) { "$(Get-Content -LiteralPath $note -TotalCount 1)".Trim() } else { 'hookshot\game' }
    if (-not [System.IO.Path]::IsPathRooted($g)) { $g = Join-Path $Kit $g }
    return [System.IO.Path]::GetFullPath($g)
}
$gameA = Get-GameDir $KitA
if ($Mode -eq 'merge') {
    $merge = Join-Path $KitA 'hookshot\tools\puppet_pack\merge_tables.py'
    if (-not (Test-Path -LiteralPath $merge) -or -not (Select-String -LiteralPath $merge -SimpleMatch -Quiet -Pattern '--in-game')) {
        Write-Host "NOT THIS BRANCH charpage-288 ${Mode}: $merge is missing or has no --in-game (kit tools not built from this branch)"
        exit 3
    }
}
$own = @(Get-ChildItem -LiteralPath (Join-Path $gameA 'romfs\UI\Layout2Archive') -Filter 'Common.Product.*.blarc.zs' -ErrorAction SilentlyContinue) | Select-Object -First 1
if (-not $own) { throw "the game's own layout archive was not found under $gameA\romfs\UI\Layout2Archive" }
$fake = Join-Path $gameA 'mods\zz-row288-seed'
$fakeDir = Join-Path $fake 'romfs\UI\Layout2Archive'
$names = 'OOT_TEST_NO_MERGE', 'OOT_COOP_START_MERGE', 'OOT_COOP_RESTART', 'HOS_CRASH_WATCH'
$runLines = @()
$started = Get-Date
try {
    New-Item -ItemType Directory -Force -Path $fakeDir | Out-Null
    Copy-Item -LiteralPath $own.FullName -Destination (Join-Path $fakeDir $own.Name) -Force
    Write-Host "  $fake made (the game's own $($own.Name))"
    $env:OOT_TEST_NO_MERGE = '1'
    $env:HOS_CRASH_WATCH = '0'
    if ($Mode -eq 'merge') { $env:OOT_COOP_START_MERGE = '1'; $env:OOT_COOP_RESTART = '1' }
    $runLines = @(& powershell -NoProfile -ExecutionPolicy Bypass -File $runPs1 -Runtime hookshot -Lane 1 -Kit "$KitA,$KitB" -Scenario $scenario -Timeout $Timeout -Server $Server |
        ForEach-Object { Write-Host $_; "$_" })
} finally {
    foreach ($n in $names) { Remove-Item "Env:\$n" -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $fake) { Remove-Item -LiteralPath $fake -Recurse -Force }
    Write-Host "  $fake removed (the next kit or lane start merges again)"
}
$runDir = @($runLines | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Container) }) | Select-Object -Last 1
$log = @((Join-Path $gameA 'logs\longshot_log.txt'), (Join-Path $gameA 'longshot_log.txt')) | Where-Object { Test-Path -LiteralPath $_ } |
    Sort-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc } | Select-Object -Last 1
$text = if ($log -and (Get-Item -LiteralPath $log).LastWriteTime -ge $started) { [System.IO.File]::ReadAllText($log) } else { '' }
$fatal = $text -match 'FATAL: the game aborted'
function Has([string]$needle) { return $text.Contains($needle) }
$want = switch ($Mode) {
    'hold'  { @("randomizer: the game reads the menus' layout archive", 'coop: pause tab: the Characters page is held') }
    'merge' { @('mods: the game was started again', 'the merge ended with 0') }
    default { @() }
}
$missing = @($want | Where-Object { -not (Has $_) })
$unwanted = if ($Mode -eq 'merge' -and (Has 'the Characters page is held')) { @('the Characters page is held (the merge did not carry the layouts)') } else { @() }
Write-Host "  log: $log; abort: $fatal; missing lines: $($missing -join ' | '); unwanted: $($unwanted -join ' | ')"
$text -split "`n" | Select-String -SimpleMatch -Pattern 'mods: merge', 'the merge ended', 'layout archive', 'Characters page is held' |
    Select-Object -First 12 | ForEach-Object { Write-Host "    $_" }
if (-not $text) {
    Write-Host "FAIL charpage-288 ${Mode}: no log of this run [shots in $runDir]"
    exit 1
}
if ($Mode -eq 'repro') {
    if ($fatal) { Write-Host "UNPROVEN charpage-288 repro: the game aborted, as the report (read the last shots: the crash must come at the Characters tab) [shots in $runDir]"; exit 2 }
    Write-Host "FAIL charpage-288 repro: no crash (is the Characters tab's code in this kit's game, and did the mod's archive win?) [shots in $runDir]"
    exit 1
}
if ($fatal -or $missing.Count -gt 0 -or $unwanted.Count -gt 0 -or -not $runDir) {
    Write-Host "FAIL charpage-288 ${Mode}: abort $fatal, missing or unwanted log lines above [shots in $runDir]"
    exit 1
}
Write-Host "UNPROVEN charpage-288 ${Mode}: the log lines are right and nothing aborted; read the shots named in scenarios\$scenario.A.txt [shots in $runDir]"
exit 2
