#!/usr/bin/env bash
# Runs one trace without fusion and with I-Fuse. Reports the speedup
# and what fusion did for LD2s and their consumers.
#
# Usage: i-fuse/scripts/run.sh <scarab> <trace.zip> <roi_begin> <roi_end>
#          <out_dir> [scarab_flag ...]
#   scarab     Binary from build.sh
#   trace.zip  Memtrace SimPoint trace
#   roi_*      Values for --memtrace_roi_begin and --memtrace_roi_end
#   out_dir    New directory that holds nofuse/ and ifuse/
#
# Both runs use the paper's settings (PARAMS.in, 20M warmup, 30M
# instructions, and 32 KB I-cache) and run in parallel. The no-fusion
# run turns off training, so the FCT stays empty and nothing is fused.
# Extra flags go to both runs and come last, so they override these
# settings. For example, --ifuse_ld2_bank_conflict 0 turns off the
# L1-D bank model for LD2's word.

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
mkdir -p "$OUT/nofuse" "$OUT/ifuse"
OUT=$(cd "$OUT" && pwd)

flags=(--frontend memtrace --cbp_trace_r0 "$TRACE"
  --memtrace_roi_begin "$ROI_BEGIN" --memtrace_roi_end "$ROI_END"
  --full_warmup 20000000 --inst_limit 30000000 --icache_size 32768)

run() {
  local name=$1
  shift
  cp "$IF_DIR/PARAMS.in" "$OUT/$name/PARAMS.in"
  (cd "$OUT/$name" && "$SCARAB" "${flags[@]}" "$@" > sim.log 2>&1)
}

echo "running nofuse/ and ifuse/ in $OUT"
run nofuse --ifuse_runtime_training_enabled 0 "$@" &
nofuse_pid=$!
run ifuse "$@" &
ifuse_pid=$!
status=0
wait $nofuse_pid || { echo "nofuse failed, see $OUT/nofuse/sim.log" >&2; status=1; }
wait $ifuse_pid || { echo "ifuse failed, see $OUT/ifuse/sim.log" >&2; status=1; }
[[ $status -eq 0 ]] || exit 1

# Column 2 of a stat file counts only the measured region (after warmup)
stat_of() {
  awk -v name="$2" '$1 == name { print $2 }' "$OUT/$1"
}
ratio() {
  awk -v a="$1" -v b="$2" 'BEGIN { if (b == 0) print "-"; else printf "%.2f", a / b }'
}
pct() {
  awk -v a="$1" -v b="$2" 'BEGIN { if (b == 0) print "-"; else printf "%.1f%%", 100 * a / b }'
}

c0=$(stat_of nofuse/core.stat.0.out NODE_CYCLE)
c1=$(stat_of ifuse/core.stat.0.out NODE_CYCLE)
s=ifuse/ifuse.stat.0.out
ld2=$(stat_of $s IFUSE_PMU_LD2_RETIRED)
fused=$(stat_of $s IFUSE_PMU_LD2_FUSED)
early=$(stat_of $s IFUSE_PMU_LD2_EARLY)
cons=$(stat_of $s IFUSE_PMU_CONSUMERS)
cons_early=$(stat_of $s IFUSE_PMU_CONSUMERS_EARLY)

echo "cycles:     $c0 (no fusion), $c1 (I-Fuse)"
echo "speedup:    $(awk -v a="$c0" -v b="$c1" 'BEGIN { printf "%.3f", a / b }')x"
echo "LD2s:       $ld2 retired, $fused fused ($(pct "$fused" "$ld2"))"
echo "LD2 data:   $(pct "$early" "$fused") of fused LD2s earlier," \
  "by $(ratio "$(stat_of $s IFUSE_PMU_LD2_EARLY_CYCLES)" "$early") cycles on average"
saved=$(stat_of $s IFUSE_PMU_LD2_EARLY_CYCLES)
lost=$(stat_of $s IFUSE_PMU_LD2_LATE_CYCLES)
echo "LD2 saves:  $(awk -v a="$saved" -v b="$lost" -v n="$fused" 'BEGIN { if (n == 0) print "-"; else printf "%.2f", (a - b) / n }')" \
  "cycles per fused LD2 on average ($(ratio "$saved" "$fused") saved, $(ratio "$lost" "$fused") lost)"
echo "consumers:  $(pct "$cons_early" "$cons") of $cons ready earlier," \
  "by $(ratio "$(stat_of $s IFUSE_PMU_CONSUMERS_EARLY_CYCLES)" "$cons_early") cycles on average"
echo "            $(pct "$(stat_of $s IFUSE_PMU_CONSUMERS_OTHER_SOURCE)" "$cons") waited on another source"
held=$(stat_of $s IFUSE_LD2_BANK_RESERVED)
if [[ $held -gt 0 ]]; then
  echo "banks:      $held fused accesses held LD2's bank," \
    "$(stat_of $s IFUSE_LD2_BANK_CONFLICT_DELAYS) conventional-load delays"
fi
