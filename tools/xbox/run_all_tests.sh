#!/bin/sh
# Runs every host test of the Xbox port. usage: tools/xbox/run_all_tests.sh   (EXTRA="-fsanitize=address,undefined" for sanitizers)
cd "$(dirname "$0")/../.." || exit 1
fail=0
for t in pad card dvd os thread xmem rel math chp gx; do
    printf '%-7s ' "$t"
    out=$(sh "tools/xbox/run_${t}_test.sh" 2>&1)
    rc=$?
    echo "$out" | tail -1
    [ $rc -eq 0 ] || { fail=1; echo "$out" | tail -15; }
done
exit $fail
