#!/usr/bin/env bash
# Build this repository's Scarab, which includes Helios, with memtrace support.
#
# Usage: helios/scripts/build.sh [build_dir] [type]
#   build_dir  CMake build directory, default src/build/<type>
#   type       opt (default) or dbg
#
# Upstream `cd src && make opt` does the same but also builds the PIN frontend,
# which needs PIN_ROOT. Memtrace runs do not need PIN.

set -euo pipefail

TYPE=${2:-opt}
case $TYPE in
  opt) CMAKE_TYPE=SCARABOPT ;;
  dbg) CMAKE_TYPE=Debug ;;
  *) echo "type must be opt or dbg" >&2; exit 1 ;;
esac
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../src" && pwd)
BUILD=$(mkdir -p "${1:-$SRC/build/$TYPE}" && cd "${1:-$SRC/build/$TYPE}" && pwd)

export SCARAB_ENABLE_PT_MEMTRACE=1
make -C "$SRC" --no-print-directory gitrev  # src/version.c includes the generated src/gitrev
(cd "$BUILD" && CC=cc CXX=c++ ASM=as cmake "$SRC" -DCMAKE_BUILD_TYPE=$CMAKE_TYPE)
make -C "$BUILD" -j"$(nproc)" scarab
echo "built $BUILD/scarab"
