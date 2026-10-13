#!/usr/bin/env bash
# Build Scarab with ideal fusion: check out the upstream Scarab commit ideal
# fusion was developed on, add the ideal-fusion sources, apply the integration
# patch, and run make.
#
# Usage: ideal-fusion/scripts/build.sh <out_dir> [make_target]
#   out_dir      new directory for the patched Scarab tree (must not exist)
#   make_target  Scarab make target, default opt (see src/Makefile)
#
# The binary lands in <out_dir>/src/build/<make_target>/scarab. Both passes
# use this one binary; --ideal_fusion_pass selects the pass at run time.

set -euo pipefail

# Upstream Scarab "issue_queue: silence Clang -Wunused-private-field for
# queue_id (#513)". The hpca2027-ideal-fusion branch forked here, and
# scarab.patch applies to this tree only.
BASE=40803f7964d686e39527eaf08a48a7c996967508

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 <out_dir> [make_target]" >&2
  exit 1
fi
OUT=$1
TARGET=${2:-opt}
IF_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
REPO=$(git -C "$IF_DIR" rev-parse --show-toplevel)

if [[ -e $OUT ]]; then
  echo "$OUT already exists" >&2
  exit 1
fi
if ! git -C "$REPO" cat-file -e "$BASE^{commit}" 2>/dev/null; then
  echo "commit $BASE not found in $REPO; fetch upstream Scarab history first" >&2
  exit 1
fi

git clone -q --no-checkout "$REPO" "$OUT"
git -C "$OUT" checkout -q --detach "$BASE"
git -C "$OUT" submodule update -q --init --recursive

cp -r "$IF_DIR/src/." "$OUT/src/"
git -C "$OUT" apply "$IF_DIR/scarab.patch"

# Memtrace support is required: every ideal-fusion run uses --frontend memtrace.
SCARAB_ENABLE_PT_MEMTRACE=1 make -C "$OUT/src" "$TARGET"
