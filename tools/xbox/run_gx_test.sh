#!/bin/sh
# Host test of the GX layer: the SDK's real GX sources (src/lib/GX*.c) run against the software command processor
# (src/xbox/gp_xbox.cpp). The test calls the SDK API, then checks the CP/XF/BP state and the vertices the processor
# decodes against what the call asked for.
#   usage: tools/xbox/run_gx_test.sh     (CC / CXX override the compilers; EXTRA adds flags, e.g. sanitizers)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/re4_gx_test.$$
mkdir -p "$OUT/inc/dolphin"
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-cc}
CXX=${CXX:-c++}
EXTRA=${EXTRA:-}
sed -e 's/typedef signed   long          s32;/typedef signed   int           s32;/' \
    -e 's/typedef unsigned long          u32;/typedef unsigned int           u32;/' \
    "$ROOT/include/dolphin/types.h" > "$OUT/inc/dolphin/types.h"
grep -q 'unsigned int           u32' "$OUT/inc/dolphin/types.h" || { echo "could not make the 32 bit types header" >&2; exit 1; }
CF="-w -no-pie -fno-pie $EXTRA -DRE4_XBOX -I $OUT/inc -I $ROOT/include -I $ROOT/src/lib"
objs=""
for f in "$ROOT"/src/lib/GX*.c; do
    o="$OUT/$(basename "$f" .c).o"
    src="$f"
    if [ "$(basename "$f")" = GXDraw.c ]; then  # a static table named `index` clashes with glibc's index()
        src="$OUT/GXDraw.c"
        sed 's/\bindex\b/gx_index_tbl/g' "$f" > "$src"
    fi
    "$CC" -std=gnu99 $CF -c "$src" -o "$o"
    objs="$objs $o"
done
"$CC" -std=gnu99 $CF -c "$ROOT/tools/xbox/gx_stubs.c" -o "$OUT/gx_stubs.o"
objs="$objs $OUT/gx_stubs.o"
"$CXX" -std=gnu++11 $CF "$ROOT/tools/xbox/gx_test.cpp" "$ROOT/src/xbox/gp_xbox.cpp" "$ROOT/src/xbox/xmem_xbox.cpp" "$ROOT/src/xbox/gx_xbox.cpp" \
    $objs -o "$OUT/gx_test" -lm > "$OUT/cxx.log" 2>&1 || { grep -E 'error|undefined' "$OUT/cxx.log" | sort -u | head -60; exit 1; }
if [ -n "${KEEP:-}" ]; then cp "$OUT/gx_test" "$KEEP"; fi
"$OUT/gx_test"
