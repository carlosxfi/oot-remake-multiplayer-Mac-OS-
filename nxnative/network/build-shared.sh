#!/bin/sh
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../.." && pwd)
build="$here/build/shared-core"

cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCOOP_BUILD_HOOKSHOT=ON \
  -DCOOP_BUILD_TESTS=OFF -DCOOP_BUILD_PROGRAMS=OFF
cmake --build "$build" --target hookshot_coop --parallel 4
sh "$here/build.sh"
printf 'Shared module: %s\n' "$build/hookshot/hookshot_coop.dylib"
