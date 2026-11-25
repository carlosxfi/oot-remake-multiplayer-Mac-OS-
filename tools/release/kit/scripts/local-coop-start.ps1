# Local co-op from the game (its Local co-op page, Start the games / Start missing games): the module of game 1 runs
# this once game 1 hosts the session (docs/play-lan.md, "Two to four players on one PC"). It starts the games of
# players 2 to -Players that do not run yet, into that session, as Start Squadala's local co-op start does
# (local-coop.ps1: the same folders, files, controllers and frame cap), and writes what happened to -Result.
param(
    [Parameter(Mandatory = $true)][string]$Kit,
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][int]$Players,
    [Parameter(Mandatory = $true)][int]$Port,
    [Parameter(Mandatory = $true)][string]$Password,
    [int]$HostPid = 0,
    [string]$Result = ''
)
$ErrorActionPreference = 'Continue'
try {
    . (Join-Path $PSScriptRoot 'local-coop.ps1')
    $games = @(Add-KitLocalCoop $Kit $Exe $Players $Port $Password $HostPid $Result)
    foreach ($g in $games) { try { & (Join-Path $PSScriptRoot 'problem-send.ps1') -Watch $g.Id } catch { } }
    exit 0
} catch {
    if ($Result) { try { [System.IO.File]::WriteAllText($Result, "started=`r`nrunning=`r`nfailed=`r`nproblem=The launcher stopped: $($_.Exception.Message)`r`n") } catch { } }
    exit 1
}
