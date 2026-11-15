#!/bin/sh
# Builds and runs the host unit test for the Xbox OS layer (src/xbox/os_xbox.cpp).
# Needs only a C and a C++ compiler; no Xbox toolchain or game assets.
#   usage: tools/xbox/run_os_test.sh     (CC / CXX override the compilers; EXTRA adds flags, e.g. sanitizers)
#
# The layer is linked with the SDK files it reuses as they are (OSAlloc.c, OSArena.c, OSStopwatch.c, and the
# thread files OSInit now starts: OSThread.c, OSMutex.c, OSSemaphore.c). The SDK's
# calendar functions are cut out of src/lib/OSTime.c and built under other names as the reference; the rest of
# that file is PowerPC assembly and does not build here.
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/re4_os_test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-cc}
CXX=${CXX:-c++}
EXTRA=${EXTRA:-}

# On this 64 bit host the SDK's `unsigned long` u32 is 64 bits wide, which changes the arithmetic of the
# time macros (s64 % u32 turns negative times into huge ones) and the heap's address masks. The console
# is 32 bit, so everything here is built against a copy of dolphin/types.h with 32 bit s32 / u32.
mkdir -p "$OUT/inc/dolphin"
sed -e 's/typedef signed   long          s32;/typedef signed   int           s32;/' \
    -e 's/typedef unsigned long          u32;/typedef unsigned int           u32;/' \
    "$ROOT/include/dolphin/types.h" > "$OUT/inc/dolphin/types.h"
grep -q 'unsigned int           u32' "$OUT/inc/dolphin/types.h" || { echo "could not make the 32 bit types header" >&2; exit 1; }

# tables + helpers + the two functions
{
    echo '#include <dolphin/os.h>'
    echo '#define ASSERTLINE(...)'
    sed -n '/^static int YearDays/,/334};/p' "$ROOT/src/lib/OSTime.c"
    sed -n '/^static int LeapYearDays/,/335};/p' "$ROOT/src/lib/OSTime.c"
    sed -n '/^static int IsLeapYear/,$p' "$ROOT/src/lib/OSTime.c"
} > "$OUT/orig_time.c"
grep -q 'OSCalendarTimeToTicks' "$OUT/orig_time.c" || { echo "could not extract the calendar code from OSTime.c" >&2; exit 1; }

"$CC" -c -w -std=gnu99 -DRE4_XBOX -I "$OUT/inc" -I "$ROOT/include" \
    -DOSTicksToCalendarTime=orig_OSTicksToCalendarTime -DOSCalendarTimeToTicks=orig_OSCalendarTimeToTicks \
    "$OUT/orig_time.c" -o "$OUT/orig.o"
for f in OSAlloc OSArena OSStopwatch OSThread OSMutex OSSemaphore; do
    "$CC" -c -w -D__declspec'(x)=' -Dasm= -std=gnu99 -no-pie -fno-pie -D__cntlzw=xos_cntlzw -DRE4_XBOX -I "$OUT/inc" -I "$ROOT/include" -I "$ROOT/src/lib" "$ROOT/src/lib/$f.c" -o "$OUT/$f.o"
done

"$CXX" -std=gnu++11 -Wall -Wextra -Wno-unused-parameter -no-pie -fno-pie $EXTRA -DRE4_XBOX -I "$OUT/inc" -I "$ROOT/include" \
    "$ROOT/tools/xbox/os_test.cpp" "$ROOT/src/xbox/os_xbox.cpp" "$ROOT/src/xbox/thread_xbox.cpp" "$ROOT/src/xbox/xthread_backend_sys.cpp" "$OUT/orig.o" \
    "$OUT/OSAlloc.o" "$OUT/OSArena.o" "$OUT/OSStopwatch.o" "$OUT/OSThread.o" "$OUT/OSMutex.o" "$OUT/OSSemaphore.o" -lpthread -o "$OUT/os_test" 2>&1 | grep -E 'tools/xbox|src/xbox|error|undefined' || true
[ -x "$OUT/os_test" ] || { echo "build failed" >&2; exit 1; }

timeout ${TEST_TIMEOUT:-30} "$OUT/os_test"
