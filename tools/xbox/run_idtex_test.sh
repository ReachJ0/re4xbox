#!/bin/sh
# Builds and runs the host test of the id data conversion (src/xbox/xidtex_endian.cpp).
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/re4_idtex_test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
${CXX:-c++} -std=gnu++11 -Wall -Wextra -g -fsanitize=address,undefined ${EXTRA:-} -DRE4_XBOX -I "$ROOT/include" \
    "$ROOT/tools/xbox/idtex_test.cpp" "$ROOT/src/xbox/xidtex_endian.cpp" "$ROOT/src/xbox/xtex_endian.cpp" -o "$OUT/idtex_test"
"$OUT/idtex_test"
