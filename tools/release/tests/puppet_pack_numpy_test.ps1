# Regression C2-1 (reports/regress-01113-c2.md): the emulator kit's puppet pack must be made without numpy. 0.11.12's
# scripts\puppet-pack.ps1 needed only zstandard; rc-01113 0dacc172b stopped (exit 1) when numpy was missing and pip could
# not get it, so play.ps1 started the game with the other players shown only as markers. numpy only makes the character
# voices; without it build.py gives those characters the silent archive (voices.silent, test_puppet_pack.py
# test_build_character_voices_not_made).
# The kit's own scripts\puppet-pack.ps1 and kit-paths.ps1 run in a staged kit whose python\python.exe is this PC's
# Python made isolated by a python._pth (no site-packages: no numpy, no pip), with a stub zstandard module; the
# pack tool and the game check are stubs that record how they were called. PIP_NO_INDEX=1 as on an offline PC.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\tests\puppet_pack_numpy_test.ps1 [-Script <puppet-pack.ps1>]
# (-Script: another copy of puppet-pack.ps1, e.g. 0dacc172b's, to show the stop it had.) Exits 1 when a check fails.
param([string]$Script = '')
$ErrorActionPreference = 'Stop'
$scripts = Join-Path (Split-Path -Parent $PSScriptRoot) 'kit\scripts'
if (-not $Script) { $Script = Join-Path $scripts 'puppet-pack.ps1' }

$failed = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "ok   $What" } else { Write-Host "FAIL $What"; $script:failed++ }
}

$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { Write-Host 'FAIL no python on PATH (CI: windows-latest has one)'; exit 1 }
$base = (& $python -I -c 'import sys; print(sys.base_prefix)').Trim()
$ver = (& $python -I -c 'import sys; print("%d%d" % sys.version_info[:2])').Trim()

function New-Kit([bool]$WithNumpy) {
    $kit = Join-Path ([System.IO.Path]::GetTempPath()) ('oot-coop-np-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    $py = Join-Path $kit 'python'
    $stub = Join-Path $kit 'stub'
    $tools = Join-Path $kit 'tools\puppet_pack'
    foreach ($d in @((Join-Path $kit 'scripts'), $py, $stub, $tools, (Join-Path $kit 'emulator\user'))) {
        New-Item -ItemType Directory -Force -Path $d | Out-Null
    }
    Copy-Item -LiteralPath $Script -Destination (Join-Path $kit 'scripts\puppet-pack.ps1')
    Copy-Item -LiteralPath (Join-Path $scripts 'kit-paths.ps1') -Destination (Join-Path $kit 'scripts')
    # The PC's Python, isolated: python.exe and its DLLs, and a python._pth naming only the standard library and the stubs.
    Copy-Item -LiteralPath (Join-Path $base 'python.exe') -Destination $py
    Get-ChildItem -LiteralPath $base -Filter '*.dll' | Copy-Item -Destination $py
    $pth = @((Join-Path $base "python$ver.zip"), (Join-Path $base 'Lib'), (Join-Path $base 'DLLs'), $stub)
    [System.IO.File]::WriteAllLines((Join-Path $py 'python._pth'), [string[]]$pth)
    [System.IO.File]::WriteAllText((Join-Path $stub 'zstandard.py'), "# stub`n")
    if ($WithNumpy) { [System.IO.File]::WriteAllText((Join-Path $stub 'numpy.py'), "# stub`n") }
    # The game check: finds the game (romfs=<path>). The pack tool: records its arguments and says it made the pack.
    [System.IO.File]::WriteAllText((Join-Path $tools 'dumpcheck.py'), @'
import sys
a = sys.argv[1:]
print("romfs=" + (a[a.index("--romfs") + 1] if "--romfs" in a else "C:/game/romfs"))
'@)
    [System.IO.File]::WriteAllText((Join-Path $tools 'build.py'), @'
import os, sys
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "built.txt"), "w") as f:
    f.write(" ".join(sys.argv[1:]))
print("pack made")
'@)
    return $kit
}

function Invoke-Pack([string]$Kit) {
    $ps = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $env:PIP_NO_INDEX = '1'
    $ErrorActionPreference = 'Continue'
    $out = @(& $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Kit 'scripts\puppet-pack.ps1') 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    Remove-Item Env:\PIP_NO_INDEX
    foreach ($l in $out) { Write-Host "     $l" }
    $built = Join-Path $Kit 'tools\puppet_pack\built.txt'
    $args_ = if (Test-Path -LiteralPath $built) { [System.IO.File]::ReadAllText($built) } else { $null }
    return @{ Code = $code; Out = $out; Built = $args_ }
}

$kits = @()
try {
    # The staged Python really has no numpy (else this test proves nothing).
    $kit = New-Kit $false; $kits += $kit
    $exe = Join-Path $kit 'python\python.exe'
    $ErrorActionPreference = 'Continue'
    & $exe -c 'import zstandard' 2>$null; $z = $LASTEXITCODE
    & $exe -c 'import numpy' 2>$null; $n = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    Check ($z -eq 0) 'staged Python: zstandard (stub) imports'
    Check ($n -ne 0) 'staged Python: no numpy'

    Write-Host 'Without numpy:'
    $r = Invoke-Pack $kit
    Check ($r.Code -eq 0) "puppet-pack.ps1 exits 0 (got $($r.Code))"
    Check ($null -ne $r.Built) 'the pack tool ran'
    Check ($r.Built -like '*--romfs C:/game/romfs --user-dir *') "the pack tool got the game and the user folder ($($r.Built))"
    Check (@($r.Out | Where-Object { $_ -like '*characters are silent*' }).Count -eq 1) 'says once that the characters are silent'
    Check (@($r.Out | Where-Object { $_ -like '*-m pip install numpy*' }).Count -eq 1) 'says how to get their voices'
    Check (@($r.Out | Where-Object { $_ -like 'Done.*' }).Count -eq 1) 'ends with Done'

    Write-Host 'With numpy:'
    $kit2 = New-Kit $true; $kits += $kit2
    $r2 = Invoke-Pack $kit2
    Check ($r2.Code -eq 0) "puppet-pack.ps1 exits 0 (got $($r2.Code))"
    Check (@($r2.Out | Where-Object { $_ -like '*numpy*' }).Count -eq 0) 'no numpy line'
    Check ($r2.Built -eq $r.Built.Replace($kit, $kit2)) 'the pack tool is called the same way'
} finally {
    foreach ($k in $kits) { Remove-Item -LiteralPath $k -Recurse -Force -ErrorAction SilentlyContinue }
}
if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'all checks passed'
