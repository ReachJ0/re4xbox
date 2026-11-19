#!/bin/sh
# Lists the functions of the game that fall off the end without returning a value (clang -Wreturn-type).
# On the Xbox build those become an illegal instruction (the PowerPC code left the result in r3); fix them with
# XRET / XRETV (include/types.h). Needs NXDK_DIR and nxdk's bin on PATH. Line numbers follow the game's #line
# directives, not the physical file.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
: "${NXDK_DIR:?set NXDK_DIR}"
INC="-I$NXDK_DIR/lib/winapi/winmm -I$NXDK_DIR/lib/zlib/zlib -isystem $NXDK_DIR/lib/libjpeg/libjpeg-turbo -DXBOX -I$NXDK_DIR/lib/sdl -I$NXDK_DIR/lib/sdl/SDL2/include -Dmain=re4_main -DRE4_XBOX -DNXDK -Wno-c++11-narrowing -I$ROOT/include -I$ROOT/src/lib"
ls "$ROOT"/src/game/*.cpp "$ROOT"/src/xbox/*.cpp | xargs -P8 -I{} sh -c "nxdk-cxx $INC -Wno-everything -Wreturn-type -fsyntax-only {} 2>&1 | grep -E 'warning:|error:' || true" | sort -u
