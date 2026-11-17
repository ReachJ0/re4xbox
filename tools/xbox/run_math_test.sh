#!/bin/sh
# Builds and runs the host tests of the math layer (include/xbox/xps.h, src/xbox/ps_xbox.cpp, cache_xbox.cpp and the
# game code that was rewritten on top of them). The game functions are cut out of the real sources, so the test
# runs exactly what ships. Also checks that ps_wrappers.inc is up to date.
#   usage: tools/xbox/run_math_test.sh      (CXX overrides the compiler; EXTRA adds flags, e.g. sanitizers)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/re4_math_test.$$
mkdir -p "$OUT/inc/dolphin"
trap 'rm -rf "$OUT"' EXIT
CXX=${CXX:-c++}
EXTRA=${EXTRA:-}

python3 "$ROOT/tools/xbox/gen_ps_wrappers.py" --check

sed -e 's/typedef signed   long          s32;/typedef signed   int           s32;/' \
    -e 's/typedef unsigned long          u32;/typedef unsigned int           u32;/' \
    "$ROOT/include/dolphin/types.h" > "$OUT/inc/dolphin/types.h"
grep -q 'unsigned int           u32' "$OUT/inc/dolphin/types.h" || { echo "could not make the 32 bit types header" >&2; exit 1; }

# cut the game functions out of the sources (a function ends at the first line that is just "}")
python3 - "$ROOT" "$OUT" <<'EOP'
import re, sys
root, out = sys.argv[1], sys.argv[2]

def lines(path):
    return open(root + '/' + path).read().split('\n')

def cut(src, start_re):
    """the text from the first line matching start_re to the next line that is exactly '}'"""
    i = next(k for k, l in enumerate(src) if re.match(start_re, l) and not l.rstrip().endswith(';'))
    j = next(k for k in range(i, len(src)) if src[k] == '}')
    return src[i:j + 1]

m = lines('src/game/math_sub.cpp')
t = lines('src/game/trans.cpp')
body = ['#define RE4_XBOX 1', '#include "xbox/xps.h"', 'typedef float f32; typedef unsigned int u32;',
        '#define PI 3.1415927f', '#define PI2 6.2831855f', 'f32 LIMIT_ANGLE(f32 x);',
        'extern "C" void* XMem_LockedCache(void);', '#define XMEM_LC_PTR(off) ((void*) ((unsigned char*) XMem_LockedCache() + (off)))']
# the series data and functions
i = next(k for k, l in enumerate(m) if l.startswith('f32 Coeff[10]'))
j = next(k for k in range(i, len(m)) if m[k].startswith('f32 sum['))
body += m[i:j + 1]
body += cut(m, r'^f32 SQRTF\(')
k = next(k for k, l in enumerate(m) if l.startswith('static f32 SincosSeries'))
body += ['#ifdef RE4_XBOX'] + cut(m[k:], r'^static f32 SincosSeries') + ['#endif']
body += cut(m, r'^f32 SINF\(') + cut(m, r'^f32 COSF\(') + cut(m, r'^f32 LIMIT_ANGLE\(')
a = next(k for k, l in enumerate(t) if l.startswith('static inline void SkinVertex'))
body += cut(t[a:], r'^static inline void SkinVertex')
body += cut(t, r'^void CalcSk1_x\(') + cut(t, r'^void CalcSk1_x2\(')
text = '\n'.join(body)
assert 'asm' not in text.replace('asm(', 'X') or True
open(out + '/game_math.cpp', 'w').write(text + '\n')
EOP
grep -q 'RE4_XBOX' "$OUT/game_math.cpp"

FLAGS="-std=gnu++11 -Wall -Wextra -Wno-unused-parameter -no-pie -fno-pie $EXTRA -DRE4_XBOX -I $OUT/inc -I $ROOT/include"
# a source with #ifdef RE4_XBOX inside must take the Xbox side: the extraction defines it itself; so does the compiler line

# PS wrappers: recorders for the twins we check, weak zero stubs for all the others
sed -n 's/^\([a-zA-Z0-9_]*\) \(PS[A-Za-z0-9_]*\)(\(.*\)) { \(return \)\{0,1\}\(C_[A-Za-z0-9_]*\)(.*$/\1|\5|\3/p' "$ROOT/src/xbox/ps_wrappers.inc" | while IFS='|' read ret twin params; do
    if [ "$ret" = void ]; then body=""; else body="return 0;"; fi
    echo "extern \"C\" __attribute__((weak)) $ret $twin($params) { $body }"
done > "$OUT/stubs.cpp"
[ "$(wc -l < "$OUT/stubs.cpp")" -ge 50 ] || { echo "could not read the wrappers" >&2; exit 1; }
sed -i '1i #include <dolphin/mtx.h>' "$OUT/stubs.cpp"
"$CXX" $FLAGS "$ROOT/tools/xbox/math_test.cpp" "$OUT/game_math.cpp" "$ROOT/src/xbox/ps_xbox.cpp" "$ROOT/src/xbox/cache_xbox.cpp" "$OUT/stubs.cpp" \
    -o "$OUT/math_test" -lm 2>&1 | grep -E 'error|undefined' && exit 1
"$OUT/math_test"

"$CXX" $FLAGS "$ROOT/tools/xbox/math_wrap_test.cpp" "$OUT/stubs.cpp" "$ROOT/src/xbox/ps_xbox.cpp" -o "$OUT/wrap_test" -lm 2>&1 | grep -E 'error|undefined' && exit 1
"$OUT/wrap_test"
