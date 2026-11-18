#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! command -v cmake >/dev/null 2>&1; then
  echo 'CMake is needed for the network check. Install it with: brew install cmake' >&2
  exit 2
fi
cmake -S "$here" -B "$here/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build "$here/build" --target coop_nxnative_probe nxnative_network_check nxnative_dylib_load_check --parallel 4
if ! /usr/bin/otool -hv "$here/../probe/build/libcoop_nxnative_probe.dylib" |
  grep -Eq '[[:space:]]DYLIB[[:space:]]'; then
  echo 'The injected library is not a Mach-O dylib; refusing to launch it.' >&2
  exit 1
fi
ctest --test-dir "$here/build" --output-on-failure
