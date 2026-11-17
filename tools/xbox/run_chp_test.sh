#!/bin/bash
# Host test of the CharPipeline list/string helpers (src/xbox/chp_xbox.cpp), with ASan + UBSan.
set -e
cd "$(dirname "$0")/../.."
T=$(mktemp -d); mkdir -p $T/inc/charPipeline/structures
cat > $T/inc/charPipeline/structures/List.h <<'H'
#include <stddef.h>
typedef unsigned char u8; typedef signed char s8; typedef unsigned int u32; typedef char* Ptr;
typedef struct { Ptr Prev; Ptr Next; } DSLink, *DSLinkPtr;
typedef struct { u32 Offset; Ptr Head; Ptr Tail; } DSList, *DSListPtr;
H
: > $T/inc/charPipeline/structures/dolphinString.h
g++ -std=c++11 -fsanitize=address,undefined -DRE4_XBOX -I$T/inc -w -o $T/chp tools/xbox/chp_test.cpp src/xbox/chp_xbox.cpp
$T/chp; r=$?; rm -rf $T; exit $r
