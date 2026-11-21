#!/bin/sh
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out="$here/build"
mkdir -p "$out"
repo=$(CDPATH= cd -- "$here/../.." && pwd)

compile_shared() {
  xcrun --sdk macosx clang++ \
  -std=c++20 -O2 -Wall -Wextra -Werror \
  -arch arm64 -dynamiclib \
  -I"$repo/coop/host/include" -I"$repo/coop/protocol/include" \
  "$here/link_position_probe.cpp" \
  "$@" \
  -o "$out/libcoop_nxnative_probe.dylib"
}
. "$here/shared_sources.sh"
shared_sources

printf '%s\n' "$out/libcoop_nxnative_probe.dylib"
