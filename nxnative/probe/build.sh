#!/bin/sh
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out="$here/build"
mkdir -p "$out"

xcrun --sdk macosx clang++ \
  -std=c++20 -O2 -Wall -Wextra -Werror \
  -arch arm64 -dynamiclib \
  "$here/link_position_probe.cpp" \
  -o "$out/libcoop_nxnative_probe.dylib"

printf '%s\n' "$out/libcoop_nxnative_probe.dylib"
