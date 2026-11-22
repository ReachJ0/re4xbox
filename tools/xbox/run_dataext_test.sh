#!/bin/sh
# Builds and runs the host test of the GetDataExt archive header access (src/xbox/xdataext.cpp).
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/re4_dataext_test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
${CXX:-c++} -std=gnu++11 -Wall -Wextra -g -fsanitize=address,undefined ${EXTRA:-} -DRE4_XBOX -I "$ROOT/include" \
    "$ROOT/tools/xbox/dataext_test.cpp" "$ROOT/src/xbox/xdataext.cpp" -o "$OUT/dataext_test"
"$OUT/dataext_test"
