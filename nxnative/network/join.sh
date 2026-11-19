#!/bin/sh
set -eu
if [ "$#" -ne 1 ]; then
  echo "usage: $0 /path/to/game.app" >&2
  exit 2
fi
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
library="$here/../probe/build/libcoop_nxnative_probe.dylib"
if [ ! -f "$library" ] || ! grep -aq 'nxnative-lan-probe' "$library"; then
  echo 'Build the LAN client first: sh nxnative/network/build.sh' >&2
  exit 2
fi
printf 'Windows host IPv4 address (Same network screen): '
IFS= read -r host
printf 'Port [27510]: '
IFS= read -r port
port=${port:-27510}
printf 'Nickname [MacLink]: '
IFS= read -r nickname
nickname=${nickname:-MacLink}
printf 'Room password (visible): '
IFS= read -r password
if [ -z "$host" ] || [ -z "$password" ]; then
  echo 'The host address and room password are required.' >&2
  exit 2
fi
COOP_NXNATIVE_HOST="$host" COOP_NXNATIVE_PORT="$port" \
COOP_NXNATIVE_NICKNAME="$nickname" COOP_NXNATIVE_PASSWORD="$password" \
COOP_NXNATIVE_LOOPBACK=0 sh "$here/../probe/run.sh" "$1"
