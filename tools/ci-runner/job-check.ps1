# The checks behind the runner's job-started hook (docs/ci-runner.md). Functions
# only, no top-level code: job-started.ps1, rights.ps1 and the tests dot-source it.

# Top-level folders Windows and installed programs need; oot-ci keeps the default
# access there. Every other top-level folder must be closed to it.
$OotCiSystemDirs = @('Windows', 'Program Files', 'Program Files (x86)', 'ProgramData', 'Users',
    '$Recycle.Bin', 'System Volume Information', 'Recovery', 'Documents and Settings', 'Config.Msi')
# Folders under Users that are not a person's files.
$OotCiProfileSkip = @('Default', 'Default User', 'All Users', 'desktop.ini')

# True only for daurosk's own push to daurosk/oot-remake-multiplayer on main,
# integrate-*, release-* or a tag, or daurosk's manual run (workflow_dispatch: only
# people with write access can start one) of any branch or tag there.
# pull_request and everything else: false. $Env holds the GITHUB_* values.
function Test-OotCiJob {
    param([hashtable]$Env)
    $ref = [string]$Env['GITHUB_REF']
    $ev = [string]$Env['GITHUB_EVENT_NAME']
    $who = ([string]$Env['GITHUB_REPOSITORY'] -ceq 'daurosk/oot-remake-multiplayer') -and
        ([string]$Env['GITHUB_ACTOR'] -ceq 'daurosk') -and
        ([string]$Env['GITHUB_TRIGGERING_ACTOR'] -ceq 'daurosk')
    if (-not $who) { return $false }
    if ($ev -ceq 'workflow_dispatch') { return ($ref -clike 'refs/heads/?*') -or ($ref -clike 'refs/tags/?*') }
    return ($ev -ceq 'push') -and
        (($ref -ceq 'refs/heads/main') -or ($ref -clike 'refs/heads/integrate-?*') -or ($ref -clike 'refs/heads/release-?*') -or ($ref -clike 'refs/tags/?*'))
}

# Memory and disk guard (docs/ci-runner.md): $null when a job may start now, else why not.
# 'disk' reasons refuse at once; 'memory' reasons are waited out (job-started.ps1).
$OotCiMinCommitGB = 75   # a stock game pair needs ~62 GB of commit
$OotCiMinDiskGB = 60
function Get-OotCiGuardReason {
    param([double]$FreeCommitGB, [double]$FreeDiskGB)
    if ($FreeDiskGB -lt $OotCiMinDiskGB) { return "disk: C: has $([math]::Floor($FreeDiskGB)) GB free (< $OotCiMinDiskGB GB)" }
    if ($FreeCommitGB -lt $OotCiMinCommitGB) { return "memory: free commit $([math]::Floor($FreeCommitGB)) GB < $OotCiMinCommitGB GB (a game test is running)" }
    return $null
}

# The folders that must be closed to oot-ci: every non-system top-level folder of
# every fixed drive and every profile folder under Users (Public included), less
# $Skip (the runner's own folders) and the user's own profile.
function Get-OotCiGuardedFolders {
    param([string[]]$Skip = @(), [string]$User = 'oot-ci')
    $skipFull = $Skip | Where-Object { $_ } | ForEach-Object { [IO.Path]::GetFullPath($_).TrimEnd('\') }
    $drives = [IO.DriveInfo]::GetDrives() | Where-Object { $_.DriveType -eq 'Fixed' -and $_.IsReady }
    foreach ($d in $drives) {
        foreach ($dir in Get-ChildItem -LiteralPath $d.RootDirectory.FullName -Directory -Force -ErrorAction SilentlyContinue) {
            if ($dir.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
            if ($OotCiSystemDirs -contains $dir.Name) { continue }
            if ($skipFull -contains $dir.FullName.TrimEnd('\')) { continue }
            $dir.FullName
        }
    }
    $users = Join-Path $env:SystemDrive 'Users'
    foreach ($dir in Get-ChildItem -LiteralPath $users -Directory -Force -ErrorAction SilentlyContinue) {
        if ($dir.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
        if ($OotCiProfileSkip -contains $dir.Name -or $dir.Name -eq $User) { continue }
        $dir.FullName
    }
}

# The folders of $Paths this process can list. Run as oot-ci, every one returned
# is a hole: a folder the Deny ACEs do not cover (made after the install).
function Get-OotCiOpenFolders {
    param([string[]]$Paths)
    foreach ($p in $Paths) {
        try {
            $e = [IO.Directory]::EnumerateFileSystemEntries($p).GetEnumerator()
            [void]$e.MoveNext()
            $p
        } catch [UnauthorizedAccessException] {
        } catch [IO.DirectoryNotFoundException] {
        } catch {
            $p  # anything else: count it as open (fail closed)
        }
    }
}
