#!/bin/sh
set -eu
umask 077
if [ "$#" -ne 2 ]; then
  echo "usage: $0 /path/to/game.app /path/to/puppet-mod/romfs" >&2
  exit 2
fi
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app=$(CDPATH= cd -- "$1" && pwd)
overlay=$(CDPATH= cd -- "$2" && pwd)
module="$here/build/shared-core/hookshot/hookshot_coop.dylib"
if [ ! -f "$app/Contents/Info.plist" ] ||
   [ ! -s "$overlay/Pack/Actor/CoopPuppet_Child.pack.zs" ] ||
   [ ! -s "$overlay/Pack/Actor/CoopPuppet_Adult.pack.zs" ] ||
   [ ! -f "$overlay/../coop_puppet.json" ]; then
  echo 'Choose a working nxnative app and the romfs folder of a built co-op pack.' >&2
  exit 2
fi
sh "$here/build-shared.sh"
if [ ! -f "$module" ]; then
  echo 'Shared co-op module was not built.' >&2
  exit 2
fi

state="$HOME/Library/Application Support/OOT Co-op"
pack="$state/mods/contents/0400486024DEC000/coop-puppet"
mkdir -p "$pack" "$HOME/Applications"
cp "$overlay/../coop_puppet.json" "$pack/"
launcher="$HOME/Applications/OOT Co-op.app"
if [ -e "$launcher" ]; then
  if [ ! -f "$launcher/Contents/Resources/oot-coop-launch.sh" ]; then
    echo "An unrelated app already exists at $launcher." >&2
    exit 2
  fi
else
  /usr/bin/osacompile -o "$launcher" <<'APPLESCRIPT'
on run
    set scriptPath to POSIX path of (path to resource "oot-coop-launch.sh")
    do shell script "/bin/sh " & quoted form of scriptPath & " >/dev/null 2>&1 &"
end run
APPLESCRIPT
fi
resources="$launcher/Contents/Resources"
printf '%s\n' "$here" "$app" "$overlay" "$module" "$state" > "$resources/oot-coop-paths"
cat > "$resources/oot-coop-launch.sh" <<'LAUNCH'
#!/bin/sh
set -eu
directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
{
  IFS= read -r here
  IFS= read -r app
  IFS= read -r overlay
  IFS= read -r module
  IFS= read -r state
} < "$directory/oot-coop-paths"

unset LICHII_COOP_SERVER LICHII_COOP_PORT LICHII_COOP_PASSWORD
export DM_ROMFS_OVERLAY="$overlay" COOP_NXNATIVE_PUPPET_PREFLIGHT=1
export COOP_NXNATIVE_MENU=1 COOP_NXNATIVE_SHARED_MODULE="$module" COOP_NXNATIVE_SHARED_DIR="$state"
export HOS_COOP_MENU=1 LICHII_COOP_PUPPET=1 LICHII_COOP_HOST_SAVE=0
export HOS_COOP_MODS_CODE=0 HOS_COOP_NULL_READS=0
unset COOP_NXNATIVE_PAUSE_PACK
if /usr/bin/plutil -extract pause_menu.0 raw -o - "$overlay/../coop_puppet.json" >/dev/null 2>&1; then
  export COOP_NXNATIVE_PAUSE_PACK=1
fi
exec /bin/sh "$here/../probe/run.sh" "$app"
LAUNCH
printf 'Open %s in Finder, then choose Multiplayer → Host or Join in the game window.\n' "$launcher"
