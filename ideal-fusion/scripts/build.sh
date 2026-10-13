#!/usr/bin/env bash
# Build Scarab with ideal fusion. Creates the patched tree with patch.sh if
# it does not exist yet, then fetches the Scarab submodules and runs make.
#
# Usage: ideal-fusion/scripts/build.sh <tree> [make_target]
#   tree         patched Scarab tree, created if missing
#   make_target  Scarab make target, default opt (see src/Makefile)
#
# The binary lands in <tree>/src/build/<make_target>/scarab. Both passes use
# this one binary. --ideal_fusion_pass selects the pass at run time.

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

# Memtrace support is required: every ideal-fusion run uses --frontend memtrace.
SCARAB_ENABLE_PT_MEMTRACE=1 make -C "$TREE/src" "$TARGET"
