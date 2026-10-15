#!/usr/bin/env bash
# Builds Scarab with I-Fuse. If the patched tree does not exist yet,
# build.sh creates it with patch.sh. It then fetches the Scarab
# submodules and runs make.
#
# Usage: i-fuse/scripts/build.sh <tree> [make_target]
#   tree         Patched Scarab tree, created if missing
#   make_target  Scarab make target, default opt (see src/Makefile)
#
# The binary lands in <tree>/src/build/<make_target>/scarab.

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 <tree> [make_target]" >&2
  exit 1
fi
TREE=$1
TARGET=${2:-opt}
SCRIPTS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

if [[ ! -e $TREE ]]; then
  "$SCRIPTS/patch.sh" "$TREE"
fi
git -C "$TREE" submodule update -q --init --recursive

# Memtrace support is required, since every I-Fuse run uses
# --frontend memtrace.
SCARAB_ENABLE_PT_MEMTRACE=1 make -C "$TREE/src" "$TARGET"
