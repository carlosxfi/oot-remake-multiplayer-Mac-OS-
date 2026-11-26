# Job-completed hook for the Windows runner (docs/ci-runner.md). install.ps1 copies it to
# C:\ProgramData\oot-ci and points ACTIONS_RUNNER_HOOK_JOB_COMPLETED at it. Runs as oot-ci
# after every job: empties the job's workspace and temp folder (only under the runner's
# _work) so build trees never pile up (disk guard), then logs C: free space. Never fails the job.
$ErrorActionPreference = 'Continue'
foreach ($d in @($env:GITHUB_WORKSPACE, $env:RUNNER_TEMP)) {
    if (-not $d) { continue }
    $full = [IO.Path]::GetFullPath($d)
    if ($full -notlike 'C:\actions-runner\_work\?*') { "oot-ci cleanup: skipped $full (not under C:\actions-runner\_work)"; continue }
    Get-ChildItem -LiteralPath $full -Force -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    if (@(Get-ChildItem -LiteralPath $full -Force -ErrorAction SilentlyContinue).Count) { "oot-ci cleanup: could not empty all of $full" }
}
"oot-ci cleanup: C: $([math]::Floor((New-Object IO.DriveInfo 'C').AvailableFreeSpace / 1GB)) GB free"
exit 0
