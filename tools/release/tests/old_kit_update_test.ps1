# An old kit, as released, updates to a new release made by make_kit_hookshot.ps1: with the OLD kit's own updater
# (its scripts\update.ps1, run from a copy as its play.ps1 does), from a local release list (OOT_COOP_UPDATE_API)
# that names the new release's assets as GitHub will. No network, nothing published, no game run.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\old_kit_update_test.ps1 `
#       -OldZip <Squadala-v0.11.3-windows.zip or a newer kit> -NewDir <make_kit_hookshot -OutRoot folder> -Tag v0.11.4
#
# -OldZip  the old release's zip (best the one downloaded from its GitHub release; its .sha256 beside it is checked).
#          From v0.11.4 a kit up to v0.11.2 cannot see the new release (no oot-coop-* asset): it goes through v0.11.3.
# -NewDir  the folder make_kit_hookshot.ps1 wrote Squadala-<Tag>-windows.zip and its .sha256 to
# -Tag     the new release's tag
# -Work    a folder for the test (default: a new temp folder, removed after)
#
# Checks: the old updater sees the new release (-Status), installs it (-Launch, exit 3), and leaves a working new kit:
# VERSION.txt and every MANIFEST.txt file are the new kit's, the start file is there, the player's coop-options.ini
# values, hookshot\work, the game folder (coop.ini, the built game) and logs are kept, the new options are added,
# and the next start's game build sees that the patches changed (so it brings the game up to date). Exits 1 on a FAIL.
param(
    [Parameter(Mandatory)][string]$OldZip,
    [Parameter(Mandatory)][string]$NewDir,
    [Parameter(Mandatory)][ValidatePattern('^v\d')][string]$Tag,
    [string]$Work = ''
)
$ErrorActionPreference = 'Stop'
$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}
function Sha([string]$P) { (Get-FileHash -LiteralPath $P -Algorithm SHA256).Hash.ToLowerInvariant() }
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
$ps = Join-Path $PSHOME 'powershell.exe'

$own = -not $Work
if ($own) { $Work = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-oldkit-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)) }
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$savedApi = $env:OOT_COOP_UPDATE_API
try {
    # --- The old kit, unzipped as a player has it, with a player's things in it
    if (Test-Path -LiteralPath "$OldZip.sha256") {
        Check ((Sha $OldZip) -eq ([regex]::Match([System.IO.File]::ReadAllText("$OldZip.sha256"), '[0-9a-fA-F]{64}').Value.ToLowerInvariant())) 'the old zip matches its .sha256'
    }
    $play = Join-Path $Work 'player'
    New-Item -ItemType Directory -Force -Path $play | Out-Null
    & $tar -x -f $OldZip -C $play
    $kit = @(Get-ChildItem -LiteralPath $play -Directory)[0].FullName
    $oldVersion = ([System.IO.File]::ReadAllLines((Join-Path $kit 'VERSION.txt'))[0])
    Write-Host "old kit: $oldVersion"
    $optFile = Join-Path $kit 'coop-options.ini'
    $opt = [System.IO.File]::ReadAllText($optFile) -replace '(?m)^pvp = off', 'pvp = on'
    # (the local zip has no attestation: the signature check is the release-attest workflow's, on the real assets)
    $opt += "`r`n# mine`r`nverify_signature = no`r`n"
    [System.IO.File]::WriteAllText($optFile, $opt)
    $game = Join-Path $kit 'hookshot\game'
    New-Item -ItemType Directory -Force -Path (Join-Path $game 'bin'), (Join-Path $kit 'hookshot\work\build') | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $game 'bin\longshot.exe'), 'built game')
    [System.IO.File]::WriteAllText((Join-Path $game 'bin\coop.ini'), "name=Tester`r`n")
    [System.IO.File]::WriteAllText((Join-Path $kit 'hookshot\work\build\obj.o'), 'compiled')
    [System.IO.File]::WriteAllText((Join-Path $kit 'logs\coop.log'), 'a log')
    # The game as the old kit built it: its stamp, as hookshot-build.ps1 writes it.
    function Get-Stamp([string]$K) {
        $kv = @{}
        foreach ($f in 'PIN', 'TOOLS.txt') {
            Get-Content -LiteralPath (Join-Path $K "hookshot\$f") | ForEach-Object { if ($_ -match '^(\w+)=(.*)$') { $kv["$f.$($Matches[1])"] = $Matches[2].Trim() } }
        }
        $ph = (@(Get-ChildItem -LiteralPath (Join-Path $K 'hookshot\patches') -Filter '*.patch' -File | Sort-Object Name) | ForEach-Object { Sha $_.FullName }) -join ','
        return "release=$($kv['TOOLS.txt.release'])`ncommit=$($kv['PIN.commit'])`npatches=$ph"
    }
    $oldStamp = Get-Stamp $kit
    [System.IO.File]::WriteAllText((Join-Path $game 'coop-built-from.txt'), $oldStamp)
    Check ((Select-String -LiteralPath (Join-Path $kit 'scripts\hookshot-build.ps1') -SimpleMatch '$stamp = "release=$($toolsPin.release)`ncommit=$($pin.commit)`npatches=$patchHash"' -Quiet)) 'the old kit''s game stamp is the one this test writes'
    $mine = @{}
    foreach ($f in 'hookshot\game\bin\longshot.exe', 'hookshot\game\bin\coop.ini', 'hookshot\game\coop-built-from.txt', 'hookshot\work\build\obj.o', 'logs\coop.log') {
        $mine[$f] = Sha (Join-Path $kit $f)
    }

    # --- The new release, as GitHub lists it
    $newWin = Join-Path $NewDir "Squadala-$Tag-windows.zip"
    foreach ($f in $newWin, "$newWin.sha256") { Check (Test-Path -LiteralPath $f) "the new release has $([System.IO.Path]::GetFileName($f))" }
    Check (-not (Test-Path -LiteralPath (Join-Path $NewDir "oot-coop-$Tag.zip"))) "the new release has no oot-coop-$Tag.zip (Squadala- names only, from v0.11.4)"
    foreach ($z in @($newWin)) {
        $line = [System.IO.File]::ReadAllText("$z.sha256").Trim()
        Check ($line -eq "$(Sha $z)  $([System.IO.Path]::GetFileName($z))") "$([System.IO.Path]::GetFileName($z)).sha256 names its own file and hash"
    }
    $assets = @()
    foreach ($f in @(Get-ChildItem -LiteralPath $NewDir -File | Where-Object { $_.Name -like "oot-coop-$Tag*" -or $_.Name -like "Squadala-$Tag*" -or $_.Name -eq 'Update.bat' })) {
        $assets += [pscustomobject]@{ name = $f.Name; browser_download_url = $f.FullName; size = $f.Length }
    }
    # (the Linux kit goes up beside it: the Windows updater must never take it)
    foreach ($ln in @("Squadala-$Tag-linux.zip")) {
        $lin = Join-Path $Work $ln
        [System.IO.File]::WriteAllText($lin, 'not a windows kit')
        [System.IO.File]::WriteAllText("$lin.sha256", "$(Sha $lin)  $ln`n")
        $assets += [pscustomobject]@{ name = $ln; browser_download_url = $lin; size = 17 }
        $assets += [pscustomobject]@{ name = "$ln.sha256"; browser_download_url = "$lin.sha256"; size = 90 }
    }
    $list = Join-Path $Work 'releases.json'
    $rel = @([pscustomobject]@{ tag_name = $Tag; draft = $false; html_url = "page/$Tag"; assets = $assets })
    [System.IO.File]::WriteAllText($list, (ConvertTo-Json -InputObject $rel -Depth 5))
    $env:OOT_COOP_UPDATE_API = $list

    # --- What the old kit's play.ps1 does on start: -Status, then the update from a copy of its updater with -Launch
    $updater = Join-Path $kit 'scripts\update.ps1'
    $status = (& $ps -NoProfile -ExecutionPolicy Bypass -File $updater -Status -Kit $kit | Out-String).Trim()
    Check ($status -eq "newer $Tag") "the old kit's -Status says 'newer $Tag' [$status]"
    $copy = Join-Path $Work 'update-copy.ps1'
    Copy-Item -LiteralPath $updater -Destination $copy
    $out = & $ps -NoProfile -ExecutionPolicy Bypass -File $copy -Kit $kit -Launch 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host "     | $_" }
    Check ($code -eq 3) "the old kit's update -Launch ends with 3 (updated: start the new kit) [$code]"

    # --- A working new kit
    $newKit = Join-Path $Work 'new'
    New-Item -ItemType Directory -Force -Path $newKit | Out-Null
    & $tar -x -f $newWin -C $newKit
    $newKit = @(Get-ChildItem -LiteralPath $newKit -Directory)[0].FullName
    $v = [System.IO.File]::ReadAllLines((Join-Path $kit 'VERSION.txt'))[0]
    $newV = [System.IO.File]::ReadAllLines((Join-Path $newKit 'VERSION.txt'))[0]
    # A release gate's test kit names its commit after the tag ('(release v0.11.12-test-<sha8>)'); a published kit has the
    # bare tag. Either way the old updater must leave exactly the new zip's VERSION.txt.
    Check ($newV -match "\(release $([regex]::Escape($Tag))(-test-[0-9a-f]{8})?\)") "the new zip's VERSION.txt is release $Tag or its test kit [$newV]"
    Check ($v -eq $newV) "VERSION.txt is the new release's [$v]"
    Check (-not (Test-Path -LiteralPath (Join-Path $kit 'update-in-progress.txt'))) 'no unfinished-update marker'
    $files = @([System.IO.File]::ReadAllLines((Join-Path $newKit 'MANIFEST.txt')) | Where-Object { $_ -and -not $_.StartsWith('#') })
    $bad = @($files | Where-Object { -not (Test-Path -LiteralPath (Join-Path $kit $_)) -or (Sha (Join-Path $kit $_)) -ne (Sha (Join-Path $newKit $_)) })
    Check ($bad.Count -eq 0) "all $($files.Count) files of the new kit are in place [$(($bad | Select-Object -First 5) -join ', ')]"
    $oldTag = [regex]::Match($oldVersion, 'release ([^ )]+)').Groups[1].Value
    $oldFiles = @([System.IO.File]::ReadAllLines((Join-Path $kit "backup-$oldTag\MANIFEST.txt")) | Where-Object { $_ -and -not $_.StartsWith('#') })
    $left = @($oldFiles | Where-Object { $files -notcontains $_ -and (Test-Path -LiteralPath (Join-Path $kit $_)) })
    Check ($left.Count -eq 0) "no file of the old kit that the new one dropped is left [$(($left | Select-Object -First 5) -join ', ')]"
    $top = @(Get-ChildItem -LiteralPath $kit -File | ForEach-Object Name | Sort-Object)
    $newTop = @(Get-ChildItem -LiteralPath $newKit -File | ForEach-Object Name | Sort-Object)
    Check ((Compare-Object $top $newTop).Count -eq 0) "the kit's top folder is the new kit's [$($top -join ', ')]"
    Check (Test-Path -LiteralPath (Join-Path $kit 'Start Squadala.bat')) 'the start file Start Squadala.bat is there'
    Check (-not (Test-Path -LiteralPath (Join-Path $kit 'Start OoT Co-op.bat'))) 'one start file: the old updater removed Start OoT Co-op.bat (by its MANIFEST.txt)'
    # The new kit's first start (play.ps1 -NoUpdate after the old updater: update.ps1 -Tidy) moves a shortcut that started it.
    $links = Join-Path $Work 'links'
    New-Item -ItemType Directory -Force -Path $links | Out-Null
    $wsh = New-Object -ComObject WScript.Shell
    $l = $wsh.CreateShortcut((Join-Path $links 'My co-op.lnk')); $l.TargetPath = Join-Path $kit 'Start OoT Co-op.bat'; $l.Save()
    $env:OOT_COOP_SHORTCUT_DIRS = $links
    try { & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $kit 'scripts\update.ps1') -Kit $kit -Tidy | Out-Null } finally { Remove-Item env:OOT_COOP_SHORTCUT_DIRS }
    Check ($wsh.CreateShortcut((Join-Path $links 'My co-op.lnk')).TargetPath -ieq (Join-Path $kit 'Start Squadala.bat')) 'one start file: the new kit''s -Tidy points the shortcut at Start Squadala.bat'
    Check ((Select-String -LiteralPath (Join-Path $kit 'scripts\update.ps1') -SimpleMatch 'function Get-KitReleaseZip' -Quiet)) 'the new kit''s updater knows the -windows name'
    $o = [System.IO.File]::ReadAllText($optFile)
    Check ($o -match '(?m)^pvp = on' -and $o -match '(?m)^# mine' -and $o -match '(?m)^verify_signature = no') 'coop-options.ini keeps the player''s values'
    $want = @([System.IO.File]::ReadAllLines((Join-Path $newKit 'coop-options.ini')) | Where-Object { $_ -match '^\s*[#;]?\s*([A-Za-z_][A-Za-z0-9_]*)\s*=' } |
        ForEach-Object { [regex]::Match($_, '([A-Za-z_][A-Za-z0-9_]*)\s*=').Groups[1].Value } | Sort-Object -Unique)
    $missing = @($want | Where-Object { $o -notmatch ('(?m)^\s*[#;]?\s*' + $_ + '\s*=') })
    Check ($missing.Count -eq 0) "coop-options.ini has every key of the new kit's [$($missing -join ', ')]"
    foreach ($f in $mine.Keys) {
        Check ((Test-Path -LiteralPath (Join-Path $kit $f)) -and (Sha (Join-Path $kit $f)) -eq $mine[$f]) "kept: $f"
    }
    # The new kit's play.ps1 (started next with -NoUpdate and the old one's arguments) takes those arguments.
    $np = Join-Path $kit 'scripts\play.ps1'
    $cmd = Get-Command -Name $np -ErrorAction SilentlyContinue
    $params = if ($cmd) { @($cmd.Parameters.Keys) } else { @() }
    $lack = @('NoUpdate', 'Mode', 'Server', 'Name', 'Port', 'Hookshot' | Where-Object { $params -notcontains $_ })
    Check ($cmd -and $lack.Count -eq 0) "the new play.ps1 takes -NoUpdate -Mode -Server -Name -Port -Hookshot [$($lack -join ', ')]"
    # The game: the kit's stamp is now the new release's. When the release changed Longshot's pin, tools or patches, the
    # next start's build sees that and brings the game up to date (incremental: hookshot\work kept); when it did not
    # (v0.11.2 -> v0.11.3: the module only), the built game is reused as it is.
    $newStamp = Get-Stamp $kit
    Check ((Select-String -LiteralPath (Join-Path $kit 'scripts\hookshot-build.ps1') -SimpleMatch '$stamp = "release=$($toolsPin.release)`ncommit=$($pin.commit)`npatches=$patchHash"' -Quiet)) 'the new kit''s game stamp is made the same way'
    Check ($newStamp -eq (Get-Stamp $newKit)) 'the kit''s game stamp is the new release''s'
    if ($newStamp -eq $oldStamp) {
        Write-Host "     | the same Longshot pin, tools and patches as $oldTag`: the built game is reused (no game build on the next start)"
    } else {
        Check ($newStamp -ne $oldStamp) 'the game''s stamp no longer matches: the next start builds the game again'
    }
} finally {
    $env:OOT_COOP_UPDATE_API = $savedApi
    if ($own) { Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue }
}
if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'all checks passed'
exit 0
