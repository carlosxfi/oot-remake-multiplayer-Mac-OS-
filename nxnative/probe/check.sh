#!/bin/sh
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../.." && pwd)
check_dir=$(mktemp -d "${TMPDIR:-/tmp}/coop-nxnative-check.XXXXXX")
trap 'rm -f "$check_dir/module-check" "$check_dir/sampler-check"; rmdir "$check_dir"' EXIT

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
  "$here/module_detection_test.cpp" -o "$check_dir/module-check"
"$check_dir/module-check"

compile_shared() {
  "${CXX:-c++}" -std=c++20 -O1 -Wall -Wextra -Werror -pthread \
    -I"$repo/coop/host/include" -I"$repo/coop/protocol/include" \
    "$here/local_sampler_test.cpp" "$@" -o "$check_dir/sampler-check"
}
. "$here/shared_sources.sh"
shared_sources
"$check_dir/sampler-check"
