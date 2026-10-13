#!/usr/bin/env bash
# Create a Scarab tree with ideal fusion applied, without building it: check
# out the upstream Scarab commit ideal fusion was developed on, add the
# ideal-fusion sources, and apply the integration patch.
#
# Usage: ideal-fusion/scripts/patch.sh <out_dir>
#   out_dir  new directory for the patched Scarab tree (must not exist)

set -euo pipefail

# Upstream Scarab "issue_queue: silence Clang -Wunused-private-field for
# queue_id (#513)". The hpca2027-ideal-fusion branch forked here, and
# scarab.patch applies to this tree only.
BASE=40803f7964d686e39527eaf08a48a7c996967508

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <out_dir>" >&2
  exit 1
fi
OUT=$1
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
cp -r "$IF_DIR/src/." "$OUT/src/"
git -C "$OUT" apply "$IF_DIR/scarab.patch"
