#!/bin/sh
# Builds and runs the host unit test for the Xbox thread layer (src/xbox/thread_xbox.cpp and
# xthread_backend_sys.cpp) with the SDK's own scheduler, mutex and semaphore files linked unchanged.
#   usage: tools/xbox/run_thread_test.sh   (CC / CXX override the compilers; EXTRA adds flags)
#
# Host workarounds, test-only: dolphin/types.h gets 32 bit s32 / u32 (the console is 32 bit; see
# run_os_test.sh), and the program is linked non-PIE so that code and data addresses fit the SDK's 32 bit
# context fields.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/re4_thread_test.$$
mkdir -p "$OUT/inc/dolphin"
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-cc}
CXX=${CXX:-c++}
EXTRA=${EXTRA:-}

sed -e 's/typedef signed   long          s32;/typedef signed   int           s32;/' \
    -e 's/typedef unsigned long          u32;/typedef unsigned int           u32;/' \
    "$ROOT/include/dolphin/types.h" > "$OUT/inc/dolphin/types.h"
grep -q 'unsigned int           u32' "$OUT/inc/dolphin/types.h" || { echo "could not make the 32 bit types header" >&2; exit 1; }

CFLAGS="-c -w -D__declspec(x)= -Dasm=  -std=gnu99 -no-pie -fno-pie -DRE4_XBOX -I $OUT/inc -I $ROOT/include -I $ROOT/src/lib"
for f in OSThread OSMutex OSSemaphore OSAlloc OSArena; do
    "$CC" $CFLAGS -D__cntlzw=xos_cntlzw "$ROOT/src/lib/$f.c" -o "$OUT/$f.o"
done

"$CXX" -std=gnu++11 -Wall -Wextra -Wno-unused-parameter -no-pie -fno-pie $EXTRA -DRE4_XBOX -I "$OUT/inc" -I "$ROOT/include" \
    "$ROOT/tools/xbox/thread_test.cpp" "$ROOT/src/xbox/thread_xbox.cpp" "$ROOT/src/xbox/xthread_backend_sys.cpp" \
    "$ROOT/src/xbox/os_xbox.cpp" "$ROOT/src/xbox/xmem_xbox.cpp" "$OUT/OSThread.o" "$OUT/OSMutex.o" "$OUT/OSSemaphore.o" "$OUT/OSAlloc.o" "$OUT/OSArena.o" \
    -lpthread -o "$OUT/thread_test" 2>&1 | grep -E 'tools/xbox|src/xbox|error|undefined' || true
[ -x "$OUT/thread_test" ] || { echo "build failed" >&2; exit 1; }

[ -n "${KEEP:-}" ] && cp "$OUT/thread_test" "$KEEP"
timeout ${TEST_TIMEOUT:-60} "$OUT/thread_test"
