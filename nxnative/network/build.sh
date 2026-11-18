#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! command -v cmake >/dev/null 2>&1; then
  echo 'CMake is needed for the network check. Install it with: brew install cmake' >&2
  exit 2
fi
cmake -S "$here" -B "$here/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build "$here/build" --target coop_nxnative_probe nxnative_network_check --parallel 4
ctest --test-dir "$here/build" --output-on-failure
