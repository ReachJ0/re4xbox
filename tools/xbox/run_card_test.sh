#!/bin/sh
# Builds and runs the host unit test for the Xbox save layer (src/xbox/card_xbox.cpp).
# Needs only a C and a C++ compiler; no Xbox toolchain, SDL or game assets.
#   usage: tools/xbox/run_card_test.sh        (CC / CXX override the compilers)
#
# Two host workarounds, both test-only:
#  * include/types.h makes u32 / s32 `unsigned long` / `long`, which are 64-bit on a 64-bit host and
#    would change the size of the game's CardStat / CardFileInfo. A copy with 32-bit `int` types is
#    force-included first (its include guard then keeps the real one out).
#  * the game's card.h has code newer compilers reject without -fpermissive (a member named like its
#    type). That is a property of the game header, not of the layer under test.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/re4_card_test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-cc}
CXX=${CXX:-c++}

# 32-bit-clean copy of the game's types.h
sed -e 's/typedef signed long s32;/typedef signed int s32;/' \
    -e 's/typedef unsigned long u32;/typedef unsigned int u32;/' \
    "$ROOT/include/types.h" > "$OUT/types32.h"

# The SDK's offsets function, cut out of CARDStat.c as it stands in the repo
sed -n '/^static void UpdateIconOffsets/,/^}/p' "$ROOT/src/lib/CARDStat.c" > "$OUT/orig_update_offsets.inc"
[ -s "$OUT/orig_update_offsets.inc" ] || { echo "could not extract UpdateIconOffsets from CARDStat.c" >&2; exit 1; }

# Stub header: the real constants and accessor macros from dolphin/card.h, plus the two structs it needs
{
    echo '#define TRUE 1'
    echo '#define FALSE 0'
    echo 'typedef int BOOL;'
    echo 'typedef unsigned int u32;'
    grep -E '^#define (CARD_STAT_|CARD_ICON_|CARD_BANNER_|CARDGetBannerFormat|CARDGetIconFormat)' \
        "$ROOT/include/dolphin/card.h"
    echo 'typedef struct CARDDir { unsigned iconAddr; unsigned char bannerFormat; unsigned short iconFormat; unsigned short iconSpeed; } CARDDir;'
    echo 'typedef struct CARDStat { unsigned char bannerFormat; unsigned short iconFormat; unsigned short iconSpeed;'
    echo '  unsigned offsetBanner, offsetBannerTlut, offsetIcon[8], offsetIconTlut, offsetData; } CARDStat;'
} > "$OUT/orig_stub.h"

"$CC" -c -w -I "$OUT" "$ROOT/tools/xbox/card_orig_offsets.c" -o "$OUT/orig.o"

"$CXX" -std=gnu++11 -Wall -Wextra -fpermissive -Wno-changes-meaning -DRE4_XBOX \
    -include "$OUT/types32.h" -I "$ROOT/include" \
    "$ROOT/tools/xbox/card_test.cpp" "$ROOT/src/xbox/card_xbox.cpp" "$OUT/orig.o" \
    -o "$OUT/card_test" 2>&1 | grep -E 'tools/xbox|src/xbox|error' || true
[ -x "$OUT/card_test" ] || { echo "build failed" >&2; exit 1; }

"$OUT/card_test"
