# Starts the Squadala! bug-report bot (tools/discord-bot/bot.py) and restarts it if it stops.
# Config: coord-multiplayer\secrets\discord-bot.json (never committed). See README.md.
param(
    [string]$Config = (Join-Path $PSScriptRoot '..\..\..\coord-multiplayer\secrets\discord-bot.json'),
    [string]$Python = (Join-Path $PSScriptRoot '..\..\..\oot-remake-recomp\.venv\Scripts\python.exe')
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Python)) { Write-Error "Python not found: $Python"; exit 1 }
if (-not (Test-Path $Config)) { Write-Error "Config not found: $Config (see tools\discord-bot\README.md)"; exit 1 }
$bot = Join-Path $PSScriptRoot 'bot.py'
$delay = 10
while ($true) {
    $started = Get-Date
    & $Python $bot --config $Config
    $code = $LASTEXITCODE
    # 2 = bad config or token, 3 = discord.py missing: restarting will not help.
    if ($code -eq 2 -or $code -eq 3) { Write-Host "discord-bot: stopped (exit $code); fix the config and start again."; exit $code }
    if (((Get-Date) - $started).TotalMinutes -gt 10) { $delay = 10 } else { $delay = [Math]::Min($delay * 2, 600) }
    Write-Host "discord-bot: exited ($code); restarting in $delay s"
    Start-Sleep -Seconds $delay
}
