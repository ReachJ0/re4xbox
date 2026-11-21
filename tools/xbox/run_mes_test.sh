#!/bin/sh
# Builds and runs the host test of the archive offset table conversion (src/xbox/xarc_endian.cpp).
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/re4_mes_test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
${CXX:-c++} -std=gnu++11 -Wall -Wextra -g -fsanitize=address,undefined ${EXTRA:-} -DRE4_XBOX -I "$ROOT/include" \
    "$ROOT/tools/xbox/mes_endian_test.cpp" "$ROOT/src/xbox/xmes_endian.cpp" -o "$OUT/mes_test"
"$OUT/mes_test"
