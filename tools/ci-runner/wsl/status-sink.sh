#!/bin/bash
# Runs as root in the distro, started by status-feed.ps1 (Windows) as `wsl.exe -u root -- status-sink.sh`, which
# writes one line "<free commit GB> <C: free GB>" to its stdin every 15 s. Keeps /run/squadala-ci/status =
# "<unix time> <commit GB> <disk GB>" for job-started.sh. The distro has no Windows interop or mounts (wsl.conf),
# so this pipe is the only way the Windows numbers get in. Its wsl.exe also keeps the distro (and the runners) up.
set -u
mkdir -p /run/squadala-ci; chmod 755 /run/squadala-ci
while read -r commit disk; do
    [[ "$commit" =~ ^[0-9]+$ && "$disk" =~ ^[0-9]+$ ]] || continue
    printf '%s %s %s\n' "$(date +%s)" "$commit" "$disk" > /run/squadala-ci/status.tmp
    chmod 644 /run/squadala-ci/status.tmp
    mv -f /run/squadala-ci/status.tmp /run/squadala-ci/status
done
