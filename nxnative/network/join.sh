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
terminal_state=
restore_echo() {
  if [ -n "$terminal_state" ]; then stty "$terminal_state"; fi
}
trap 'restore_echo' EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM
printf 'Room password (hidden): '
if [ -t 0 ]; then
  terminal_state=$(stty -g)
  stty -echo
fi
IFS= read -r password
restore_echo
terminal_state=
printf '\n'
if [ -z "$host" ] || [ -z "$password" ]; then
  echo 'The host address and room password are required.' >&2
  exit 2
fi
COOP_NXNATIVE_HOST="$host" COOP_NXNATIVE_PORT="$port" \
COOP_NXNATIVE_NICKNAME="$nickname" COOP_NXNATIVE_PASSWORD="$password" \
COOP_NXNATIVE_LOOPBACK=0 sh "$here/../probe/run.sh" "$1"
