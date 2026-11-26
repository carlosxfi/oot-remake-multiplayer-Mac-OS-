# Feeds the WSL runners' memory and disk guard (docs/ci-runner.md, "Linux runners in WSL"). install-wsl.ps1 copies
# it to %LOCALAPPDATA%\squadala-ci and starts it at every logon (scheduled task 'squadala-ci status feed', hidden).
# It runs `wsl.exe -d <distro> -u root -- /opt/squadala-ci/status-sink.sh` and writes "<free commit GB> <C: free GB>"
# to its stdin every 15 s; job-started.sh waits while that is < 75 GB / refuses at < 60 GB. That wsl.exe also
# keeps the distro, and so the runners, running. If it exits it is started again after 15 s.
param([string]$Distro = 'squadala-ci')
$ErrorActionPreference = 'Continue'
$log = Join-Path $PSScriptRoot 'status-feed.log'
function Write-FeedLog($m) { Add-Content -Encoding utf8 $log ("{0:u} {1}" -f (Get-Date).ToUniversalTime(), $m) }
while ($true) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $env:WINDIR 'System32\wsl.exe'
    $psi.Arguments = "-d $Distro -u root -- /opt/squadala-ci/status-sink.sh"
    $psi.UseShellExecute = $false; $psi.RedirectStandardInput = $true; $psi.CreateNoWindow = $true
    try {
        $p = [Diagnostics.Process]::Start($psi)
        Write-FeedLog "started wsl.exe pid $($p.Id)"
        while (-not $p.HasExited) {
            $os = Get-CimInstance Win32_OperatingSystem
            $commit = [math]::Floor($os.FreeVirtualMemory / 1MB)
            $disk = [math]::Floor((New-Object IO.DriveInfo 'C').AvailableFreeSpace / 1GB)
            $p.StandardInput.WriteLine("$commit $disk"); $p.StandardInput.Flush()
            Start-Sleep -Seconds 15
        }
        Write-FeedLog "wsl.exe exited ($($p.ExitCode))"
    } catch { Write-FeedLog "error: $_" }
    Start-Sleep -Seconds 15
}
