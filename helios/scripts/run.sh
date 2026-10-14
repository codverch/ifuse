#!/usr/bin/env bash
# Run one Helios simulation on one memtrace SimPoint, with an application's
# knobs from knobs.tsv.
#
# Usage: helios/scripts/run.sh [--baseline] <scarab> <trace.zip> <out_dir> <app> \
#          [roi_begin roi_end warmup]
#   --baseline  same run with Helios off
#   scarab      binary from scripts/build.sh
#   trace.zip   a SimPoint trace, e.g. <traces>/memcached/traces_simp/trace/149.zip
#   out_dir     run directory; PARAMS.in, sim.log, and the stat files go here
#   app         a row of knobs.tsv
#   roi_*       instruction window inside the zip. Default 1..30000000 with a
#               20000000-instruction warmup, as in the paper runs. For six apps
#               that window repeats one region in every zip; see README.md.
#
# Exits nonzero if Scarab fails.

set -euo pipefail

DEFAULT_ROI_BEGIN=1
DEFAULT_ROI_END=30000000
DEFAULT_WARMUP=20000000

FUSION=1
if [[ ${1:-} == --baseline ]]; then
  FUSION=0
  shift
fi
if [[ $# -ne 4 && $# -ne 7 ]]; then
  echo "usage: $0 [--baseline] <scarab> <trace.zip> <out_dir> <app> [roi_begin roi_end warmup]" >&2
  exit 1
fi
SCARAB=$(realpath "$1")
TRACE=$(realpath "$2")
OUT=$3
APP=$4
ROI_BEGIN=${5:-$DEFAULT_ROI_BEGIN}
ROI_END=${6:-$DEFAULT_ROI_END}
WARMUP=${7:-$DEFAULT_WARMUP}
HELIOS=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

read -r T I D W < <(awk -v app="$APP" '$1 == app { print $2, $3, $4, $5 }' "$HELIOS/scripts/knobs.tsv")
if [[ -z ${T:-} ]]; then
  echo "no knobs for $APP in scripts/knobs.tsv" >&2
  exit 1
fi

mkdir -p "$OUT"
cp "$HELIOS/../src/PARAMS.in" "$OUT/PARAMS.in"
cd "$OUT"
"$SCARAB" --frontend memtrace --cbp_trace_r0="$TRACE" \
  --memtrace_roi_begin="$ROI_BEGIN" --memtrace_roi_end="$ROI_END" \
  --inst_limit=$((ROI_END - ROI_BEGIN + 1)) --full_warmup="$WARMUP" --use_fetched_count=1 \
  --icache_size 32768 --power_intf_on 0 \
  --helios_do_fusion "$FUSION" --helios_confidence_threshold "$T" --helios_confidence_increment "$I" \
  --helios_confidence_decrement "$D" --helios_fusion_window "$W" \
  > sim.log 2>&1
grep -m1 'Core 0 Finished' sim.log
