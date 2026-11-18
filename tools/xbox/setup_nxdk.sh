#!/bin/sh
# Installs nxdk (pinned to the commit the port was built with) and builds the Xbox version.
# usage: tools/xbox/setup_nxdk.sh [nxdk dir, default ~/nxdk]     (Linux, macOS, or WSL2 on Windows)
# Needs: git clang lld llvm cmake flex bison make python3 (Debian/Ubuntu: sudo apt install git clang lld llvm cmake flex bison build-essential python3)
set -eu
NXDK_COMMIT=fafbe41b48fa00a31b79dd49ad1f86a30adcdd9e
NXDK_DIR=${1:-$HOME/nxdk}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
if [ ! -d "$NXDK_DIR/.git" ]; then
    git clone --recursive https://github.com/XboxDev/nxdk.git "$NXDK_DIR"
fi
git -C "$NXDK_DIR" fetch -q origin
git -C "$NXDK_DIR" checkout -q "$NXDK_COMMIT"
git -C "$NXDK_DIR" submodule update --init --recursive
export NXDK_DIR PATH="$NXDK_DIR/bin:$PATH"
# the first build also builds nxdk's own tools (clang wrappers, cxbe, extract-xiso)
make -C "$ROOT/xbox" -j"$(nproc 2>/dev/null || echo 4)"
echo "ok: $ROOT/xbox/bin/default.xbe"
