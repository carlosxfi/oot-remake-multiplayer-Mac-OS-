#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 /path/to/The Legend of Zelda - Ocarina of Time.app" >&2
  exit 2
fi

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app=$1
plist="$app/Contents/Info.plist"
library="$here/build/libcoop_nxnative_probe.dylib"

if [ ! -f "$library" ]; then
  "$here/build.sh" >/dev/null
fi
if [ ! -f "$plist" ]; then
  echo "not a macOS app: $app" >&2
  exit 2
fi

name=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$plist")
exe="$app/Contents/MacOS/$name"
if [ ! -x "$exe" ]; then
  echo "app executable is missing: $exe" >&2
  exit 2
fi

echo "Starting nxnative with the co-op position probe."
echo "Look for [coop-nxnative] lines in this terminal."
DYLD_INSERT_LIBRARIES="$library" exec "$exe"
