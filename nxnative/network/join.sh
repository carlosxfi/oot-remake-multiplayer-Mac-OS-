#!/bin/sh
set -eu
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: $0 /path/to/game.app [/path/to/puppet-mod/romfs]" >&2
  exit 2
fi
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
library="$here/../probe/build/libcoop_nxnative_probe.dylib"
if [ ! -f "$library" ] || ! grep -aq 'nxnative-lan-probe' "$library"; then
  echo 'Build the LAN client first: sh nxnative/network/build.sh' >&2
  exit 2
fi
if [ "$#" -eq 2 ]; then
  if [ ! -s "$2/Pack/Actor/CoopPuppet_Child.pack.zs" ] ||
     [ ! -s "$2/Pack/Actor/CoopPuppet_Adult.pack.zs" ]; then
    echo 'The overlay must contain both built co-op Link actor packs under Pack/Actor.' >&2
    exit 2
  fi
  overlay=$(CDPATH= cd -- "$2" && pwd)
  if [ -n "${DM_ROMFS_OVERLAY:-}" ] && [ "$DM_ROMFS_OVERLAY" != "$overlay" ]; then
    echo 'DM_ROMFS_OVERLAY already points to a different mod folder.' >&2
    exit 2
  fi
  export DM_ROMFS_OVERLAY="$overlay"
  export COOP_NXNATIVE_PUPPET_PREFLIGHT=1
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
if [ -n "${COOP_NXNATIVE_SHARED_MODULE:-}" ]; then
  if [ "$#" -ne 2 ] || [ ! -f "$COOP_NXNATIVE_SHARED_MODULE" ]; then
    echo 'Shared lifecycle mode needs the built module and the puppet overlay.' >&2
    exit 2
  fi
  if [ ! -f "$overlay/../coop_puppet.json" ]; then
    echo 'Shared lifecycle mode needs coop_puppet.json beside the romfs folder.' >&2
    exit 2
  fi
  state=$(mktemp -d "${TMPDIR:-/tmp}/oot-coop-nxnative.XXXXXX")
  trap 'rm -rf "$state"' EXIT
  pack="$state/mods/contents/0400486024DEC000/coop-puppet"
  mkdir -p "$pack"
  cp "$overlay/../coop_puppet.json" "$pack/"
  export COOP_NXNATIVE_SHARED_DIR="$state"
  export LICHII_COOP_SERVER="$host" LICHII_COOP_PORT="$port"
  export LICHII_COOP_NAME="$nickname" LICHII_COOP_PASSWORD="$password"
  export LICHII_COOP_PUPPET=1 LICHII_COOP_HOST_SAVE=0 HOS_COOP_MODS_CODE=0 HOS_COOP_NULL_READS=0
fi

COOP_NXNATIVE_HOST="$host" COOP_NXNATIVE_PORT="$port" \
COOP_NXNATIVE_NICKNAME="$nickname" COOP_NXNATIVE_PASSWORD="$password" \
COOP_NXNATIVE_LOOPBACK=0 sh "$here/../probe/run.sh" "$1"
