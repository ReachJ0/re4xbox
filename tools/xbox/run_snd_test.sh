#!/bin/sh
# Builds and runs the host test of the sound file endianness conversion (src/xbox/xsnd_endian.cpp).
#   usage: tools/xbox/run_snd_test.sh      (CXX overrides the compiler; EXTRA adds flags, e.g. sanitizers)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/re4_snd_test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
${CXX:-c++} -std=gnu++11 -Wall -Wextra -g -fsanitize=address,undefined ${EXTRA:-} -DRE4_XBOX -I "$ROOT/include" \
    "$ROOT/tools/xbox/snd_endian_test.cpp" "$ROOT/src/xbox/xsnd_endian.cpp" -o "$OUT/snd_test"
"$OUT/snd_test"
