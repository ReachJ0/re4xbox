#!/bin/sh
# Host test of the video-interface layer (src/xbox/vi_xbox.cpp) over a fake clock. EXTRA adds flags (sanitizers).
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/re4_vi_test.$$
mkdir -p "$OUT/inc/dolphin"
trap 'rm -rf "$OUT"' EXIT
sed -e 's/typedef signed   long          s32;/typedef signed   int           s32;/' \
    -e 's/typedef unsigned long          u32;/typedef unsigned int           u32;/' \
    "$ROOT/include/dolphin/types.h" > "$OUT/inc/dolphin/types.h"
${CXX:-c++} -std=gnu++11 -w -no-pie -fno-pie ${EXTRA:-} -DRE4_XBOX -I "$OUT/inc" -I "$ROOT/include" -I "$ROOT/src/lib" \
    "$ROOT/tools/xbox/vi_test.cpp" "$ROOT/src/xbox/vi_xbox.cpp" -o "$OUT/vi_test"
"$OUT/vi_test"
