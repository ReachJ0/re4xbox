#!/bin/sh
# Builds and runs the host unit test for the statically linked REL layer (src/xbox/link_xbox.cpp,
# rel_registry.cpp). The SDK's own OSLink.c is built under other names and linked next to it: a module image
# linked by both must end up with the same header (see rel_test.cpp). Also checks that rel_registry.cpp is
# up to date with config/G4BE08/modules/*/rel.json.
#   usage: tools/xbox/run_rel_test.sh     (CC / CXX override the compilers; EXTRA adds flags, e.g. sanitizers)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/re4_rel_test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
CC=${CC:-cc}
CXX=${CXX:-c++}
EXTRA=${EXTRA:-}

python3 "$ROOT/tools/xbox/gen_rel_registry.py" --check

# the console is 32 bit: build against a 32 bit u32 / s32 (as run_os_test.sh)
mkdir -p "$OUT/inc/dolphin"
sed -e 's/typedef signed   long          s32;/typedef signed   int           s32;/' \
    -e 's/typedef unsigned long          u32;/typedef unsigned int           u32;/' \
    "$ROOT/include/dolphin/types.h" > "$OUT/inc/dolphin/types.h"
grep -q 'unsigned int           u32' "$OUT/inc/dolphin/types.h" || { echo "could not make the 32 bit types header" >&2; exit 1; }

# SDK linker under other names (a copy: MWCC lets a cast be assigned to, `(u8*)p += n`, GCC does not)
sed -e 's/(u8\*)p += rel->offset;/p = (u32*)((u8*)p + rel->offset);/' "$ROOT/src/lib/OSLink.c" > "$OUT/OSLink.c"
grep -q '(u8\*)p += ' "$OUT/OSLink.c" && { echo "OSLink.c patch did not apply" >&2; exit 1; }
"$CC" -c -w -std=gnu99 -no-pie -fno-pie -D__declspec'(x)=' -Dasm= -DRE4_XBOX -I "$OUT/inc" -I "$ROOT/include" -I "$ROOT/src/lib" \
    -DOSLink=sdk_OSLink -DOSLinkFixed=sdk_OSLinkFixed -DOSUnlink=sdk_OSUnlink -DOSSearchModule=sdk_OSSearchModule \
    -DOSSetStringTable=sdk_OSSetStringTable -DOSNotifyLink=sdk_OSNotifyLink -DOSNotifyUnlink=sdk_OSNotifyUnlink \
    -D__OSModuleInfoList=sdk_list -D__OSStringTable=sdk_strtab -D__OSModuleInit=sdk_modinit \
    "$OUT/OSLink.c" -o "$OUT/sdklink.o"

# an entry point per generated registry declaration
sed -n 's/^void \([A-Za-z0-9_]*\)_prolog(void); void \([A-Za-z0-9_]*\)_epilog(void); void \([A-Za-z0-9_]*\)_unresolved(void);$/\1/p' \
    "$ROOT/src/xbox/rel_registry.cpp" | while read m; do
    echo "extern \"C\" { void ${m}_prolog(void) {} void ${m}_epilog(void) {} void ${m}_unresolved(void) {} }"
done > "$OUT/stubs.cpp"
[ "$(wc -l < "$OUT/stubs.cpp")" -ge 100 ] || { echo "could not read the registry declarations" >&2; exit 1; }

"$CXX" -std=gnu++11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -no-pie -fno-pie $EXTRA -DRE4_XBOX -I "$OUT/inc" -I "$ROOT/include" \
    "$ROOT/tools/xbox/rel_test.cpp" "$ROOT/src/xbox/link_xbox.cpp" "$ROOT/src/xbox/rel_registry.cpp" "$OUT/stubs.cpp" "$OUT/sdklink.o" \
    -o "$OUT/rel_test" 2>&1 | grep -E 'tools/xbox|src/xbox|error|undefined' || true
[ -x "$OUT/rel_test" ] || { echo "build failed" >&2; exit 1; }
"$OUT/rel_test"
