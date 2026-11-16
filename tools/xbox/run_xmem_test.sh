#!/bin/sh
# Builds and runs the host unit test for the Xbox memory map (include/xbox/xmem.h, src/xbox/xmem_xbox.cpp).
#   usage: tools/xbox/run_xmem_test.sh     (CXX overrides the compiler; EXTRA adds flags, e.g. sanitizers)
# Also checks that with RE4_XBOX undefined the macros expand to the GameCube expressions.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/re4_xmem_test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
CXX=${CXX:-c++}
EXTRA=${EXTRA:-}
"$CXX" -std=gnu++11 -Wall -Wextra $EXTRA -DRE4_XBOX -I "$ROOT/include" \
    "$ROOT/tools/xbox/xmem_test.cpp" "$ROOT/src/xbox/xmem_xbox.cpp" -o "$OUT/xmem_test"
"$OUT/xmem_test"
# GameCube side: the macros must be the original text
cat > "$OUT/gc.cpp" <<'EOT'
typedef unsigned int u32; typedef int s32;
#include "xbox/xmem.h"
int f(u32 a, u32* q) { return XMEM_LT(a) || XMEM_GT(a); }
int g(u32 a) { return XMEM_SUB_GT(a); }
int h(u32 a) { return XMEM_ISPTR(a); }
u32 t(u32 v) { return XMEM_TAG(v); }
u32 o(u32* p) { return XMEM_OFF(p) + XMEM_TAGOF(p); }
u32 c() { return XMEM_ADDR(0x80350000); }
EOT
"$CXX" -std=gnu++11 -E -P -I "$ROOT/include" "$OUT/gc.cpp" | tr -d ' \n' > "$OUT/gc.txt"
for want in '((u32)(a)<0x80000000||(u32)(a)>0x82FFFFFF)' '(((u32)(a)-0x80000000>0x02FFFFFF))' 'return(((v)|0x80000000))' '(0x80350000)'; do :; done
grep -q '(u32)(a)<0x80000000' "$OUT/gc.txt" && grep -q '(u32)(a)>0x82FFFFFF' "$OUT/gc.txt" && grep -q '(u32)(a)-0x80000000>0x02FFFFFF' "$OUT/gc.txt" && grep -q '(s32)(a)<0' "$OUT/gc.txt" && grep -q '(v)|0x80000000' "$OUT/gc.txt" && grep -q '&0x7FFFFFFF' "$OUT/gc.txt" || { echo "GameCube macros are not the original expressions"; cat "$OUT/gc.txt"; exit 1; }
echo "GameCube expansions ok"
