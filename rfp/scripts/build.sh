#!/usr/bin/env bash
# Build Scarab with RFP: check out the upstream Scarab commit RFP was developed
# on, add the RFP sources, apply the integration patch, and run make.
#
# Usage: rfp/scripts/build.sh <out_dir> [make_target]
#   out_dir      new directory for the patched Scarab tree (must not exist)
#   make_target  Scarab make target, default opt (see src/Makefile)
#
# The binary lands in <out_dir>/src/build/<make_target>/scarab.

set -euo pipefail

# Upstream Scarab "issue_queue: silence Clang -Wunused-private-field for
# queue_id (#513)". The hpca2027-rfp branch forked here, and scarab.patch
# applies to this tree only.
BASE=40803f7964d686e39527eaf08a48a7c996967508

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 <out_dir> [make_target]" >&2
  exit 1
fi
OUT=$1
TARGET=${2:-opt}
RFP_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
REPO=$(git -C "$RFP_DIR" rev-parse --show-toplevel)

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

cp -r "$RFP_DIR/src/." "$OUT/src/"
git -C "$OUT" apply "$RFP_DIR/scarab.patch"

make -C "$OUT/src" "$TARGET"
