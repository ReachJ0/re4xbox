#!/bin/sh
# Builds and runs the host test of the TPL / .fnt endianness conversion (src/xbox/xtex_endian.cpp).
#   usage: tools/xbox/run_tex_test.sh      (CXX overrides the compiler; EXTRA adds flags)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/re4_tex_test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
${CXX:-c++} -std=gnu++11 -Wall -Wextra -g -fsanitize=address,undefined ${EXTRA:-} -DRE4_XBOX -I "$ROOT/include" \
    "$ROOT/tools/xbox/tex_endian_test.cpp" "$ROOT/src/xbox/xtex_endian.cpp" -o "$OUT/tex_test"
"$OUT/tex_test"
