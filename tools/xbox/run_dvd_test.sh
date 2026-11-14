#!/bin/sh
# Builds and runs the host unit test for the Xbox disc layer (src/xbox/dvd_xbox.cpp, ar_xbox.cpp).
# Needs only a C and a C++ compiler; no Xbox toolchain, SDL or game assets.
#   usage: tools/xbox/run_dvd_test.sh     (CC / CXX override the compilers; EXTRA adds flags, e.g. sanitizers)
#
# The SDK's DVDCompareDiskID / DVDGenerateDiskID are compiled from src/lib/dvdidutils.c under other names
# and used as the reference for the Xbox versions. The file needs a stub <dolphin.h> (no asserts) and
# strict C99 (so libc does not declare strnlen, which the file defines itself); both are test-only.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/re4_dvd_test.$$
mkdir -p "$OUT/stub"
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-cc}
CXX=${CXX:-c++}
EXTRA=${EXTRA:-}

printf '#include <string.h>\n#include <dolphin/types.h>\n#define ASSERTMSGLINE(...)\n' > "$OUT/stub/dolphin.h"
: > "$OUT/stub/__dvd.h"

"$CC" -c -w -std=c99 -I "$OUT/stub" -I "$ROOT/include" \
    -DDVDCompareDiskID=orig_DVDCompareDiskID -DDVDGenerateDiskID=orig_DVDGenerateDiskID \
    "$ROOT/src/lib/dvdidutils.c" -o "$OUT/orig.o"

"$CXX" -std=gnu++11 -Wall -Wextra -Wno-unused-variable -Wno-unused-but-set-variable $EXTRA -DRE4_XBOX -I "$ROOT/include" \
    "$ROOT/tools/xbox/dvd_test.cpp" "$ROOT/src/xbox/dvd_xbox.cpp" "$ROOT/src/xbox/ar_xbox.cpp" "$OUT/orig.o" \
    -o "$OUT/dvd_test" 2>&1 | grep -E 'tools/xbox|src/xbox|error' || true
[ -x "$OUT/dvd_test" ] || { echo "build failed" >&2; exit 1; }

"$OUT/dvd_test"

# the stdio backend on a real folder, with the layer on top
"$CXX" -std=gnu++11 -Wall -Wextra $EXTRA -DRE4_XBOX -I "$ROOT/include" \
    "$ROOT/tools/xbox/dvd_file_test.cpp" "$ROOT/src/xbox/dvd_xbox.cpp" "$ROOT/src/xbox/ar_xbox.cpp" \
    "$ROOT/src/xbox/xdvd_backend_file.cpp" -o "$OUT/dvd_file_test" 2>&1 | grep -E 'tools/xbox|src/xbox|error' || true
[ -x "$OUT/dvd_file_test" ] || { echo "build failed (file backend test)" >&2; exit 1; }
"$OUT/dvd_file_test"
