# Registers a Windows Scheduled Task that starts the bug-report bot when you log on.
# Run it yourself once (no admin needed):  powershell -ExecutionPolicy Bypass -File install-task.ps1
# Remove it again:                         powershell -ExecutionPolicy Bypass -File install-task.ps1 -Remove
param([switch]$Remove, [string]$TaskName = 'Squadala report bot')
$ErrorActionPreference = 'Stop'
if ($Remove) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "Removed task '$TaskName'."
    exit 0
}
$script = (Resolve-Path (Join-Path $PSScriptRoot 'run-bot.ps1')).Path
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$script`"" `
    -WorkingDirectory $PSScriptRoot
$trigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 5)
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings `
    -Description 'Squadala! Discord bug-report bot (tools/discord-bot)' | Out-Null
Write-Host "Registered task '$TaskName': starts $script at logon. Start it now with: Start-ScheduledTask -TaskName '$TaskName'"
