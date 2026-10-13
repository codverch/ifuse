#!/usr/bin/env bash
# Run both ideal-fusion passes on one trace and report the speedup.
#
# Usage: ideal-fusion/scripts/run.sh <scarab> <trace.zip> <roi_begin> <roi_end>
#          <out_dir> [scarab_flag ...]
#   scarab     binary from build.sh
#   trace.zip  memtrace SimPoint trace
#   roi_*      --memtrace_roi_begin and --memtrace_roi_end
#   out_dir    new directory; gets pass1/, pass2/, and pairs.csv
#
# Both passes use the paper's settings (PARAMS.in, 20M warmup, 30M
# instructions, 32 KB I-cache). Extra flags go to both passes and come last,
# so they override these settings.

set -euo pipefail

if [[ $# -lt 5 ]]; then
  echo "usage: $0 <scarab> <trace.zip> <roi_begin> <roi_end> <out_dir>" \
    "[scarab_flag ...]" >&2
  exit 1
fi
SCARAB=$(realpath "$1")
TRACE=$(realpath "$2")
ROI_BEGIN=$3
ROI_END=$4
OUT=$5
shift 5
IF_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

if [[ -e $OUT ]]; then
  echo "$OUT already exists" >&2
  exit 1
fi
mkdir -p "$OUT/pass1" "$OUT/pass2"
OUT=$(cd "$OUT" && pwd)

flags=(--frontend memtrace --cbp_trace_r0 "$TRACE"
  --memtrace_roi_begin "$ROI_BEGIN" --memtrace_roi_end "$ROI_END"
  --full_warmup 20000000 --inst_limit 30000000 --icache_size 32768
  --ideal_fusion_log "$OUT/pairs.csv")

for pass in 1 2; do
  echo "pass $pass: running in $OUT/pass$pass"
  cp "$IF_DIR/PARAMS.in" "$OUT/pass$pass/PARAMS.in"
  if ! (cd "$OUT/pass$pass" &&
    "$SCARAB" "${flags[@]}" --ideal_fusion_pass "$pass" "$@" > sim.log 2>&1); then
    echo "pass $pass failed, see $OUT/pass$pass/sim.log" >&2
    exit 1
  fi
done

# Column 2 of core.stat.0.out counts the measured region only, after warmup.
cycles() {
  awk '$1 == "NODE_CYCLE" { print $2 }' "$OUT/pass$1/core.stat.0.out"
}
pairs=$(($(wc -l < "$OUT/pairs.csv") - 1))
c1=$(cycles 1)
c2=$(cycles 2)
echo "pairs:   $pairs"
echo "cycles:  $c1 (pass 1, no fusion), $c2 (pass 2, ideal fusion)"
awk -v a="$c1" -v b="$c2" 'BEGIN { printf "speedup: %.3fx\n", a / b }'
