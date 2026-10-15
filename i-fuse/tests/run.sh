#!/usr/bin/env bash
# Runs the I-Fuse unit tests, without traces or a Scarab build.
#
# Usage: i-fuse/tests/run.sh <scarab_tree>
#   scarab_tree  Tree from scripts/patch.sh or build.sh. It supplies
#                Scarab's headers and the I-Fuse sources under test.
#
# test_runtime_history.c checks the retired-load history and the
# tree-PLRU that runtime training uses. test_pmu.c checks the PMU
# counters in ifuse_pmu.c. test_store_window.c checks the RAW check on
# fused LD2s.

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <scarab_tree>" >&2
  exit 1
fi
SRC=$(cd "$1" && pwd)/src
TESTS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

gcc -O2 -DNO_STAT -I"$SRC" -o "$WORK/test_runtime_history" \
  "$TESTS/test_runtime_history.c" "$SRC/ifuse/ifuse_retired_load_history.c" \
  "$SRC/ifuse/ifuse_plru.c"
gcc -O2 -I"$SRC" -o "$WORK/test_pmu" "$TESTS/test_pmu.c" "$SRC/ifuse/ifuse_pmu.c"
gcc -O2 -I"$SRC" -o "$WORK/test_store_window" "$TESTS/test_store_window.c" \
  "$SRC/ifuse/ifuse_store_window.c"

"$WORK/test_runtime_history"
"$WORK/test_pmu"
"$WORK/test_store_window"
