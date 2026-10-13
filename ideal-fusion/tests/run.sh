#!/usr/bin/env bash
# Check the ideal-fusion pass logic with harness.c, without traces.
#
# Usage: ideal-fusion/tests/run.sh <scarab_tree> [dump_dir]
#   scarab_tree  a tree from scripts/build.sh; supplies headers and, unless
#                IDEAL_FUSION_C is set, src/ideal-fusion/ideal_fusion.c
#   dump_dir     if given, also write outputs for a 50k-op random stream here.
#                Diff two dump dirs to compare two implementations.
#
# Hand-written cases check each pass-1 rule, the pass-2 Load2 buffer, the
# distance filter, and parameter validation. Exit status is nonzero on failure.

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 <scarab_tree> [dump_dir]" >&2
  exit 1
fi
TREE=$(cd "$1" && pwd)/src
DUMP=${2:+$(mkdir -p "$2" && cd "$2" && pwd)}
SRC=${IDEAL_FUSION_C:-$TREE/ideal-fusion/ideal_fusion.c}
TESTS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

gcc -O2 -I"$TREE" -o "$WORK/harness" "$TESTS/harness.c" "$SRC"
cd "$WORK"

fails=0

# Pass-1 pairs as "load1,load2" micro-op numbers, one per line.
pairs() {
  rm -f ideal_fusion_candidates.csv
  printf "$1" | ./harness 1 > /dev/null
  [[ -f ideal_fusion_candidates.csv ]] || return 0
  tail -n +2 ideal_fusion_candidates.csv | cut -d, -f5,10
}

expect() {
  local name=$1 want=$2 got=$3
  if [[ "$got" == "$want" ]]; then
    echo "ok   $name"
  else
    echo "FAIL $name"
    echo "  want: $(echo "$want" | tr '\n' ' ')"
    echo "  got:  $(echo "$got" | tr '\n' ' ')"
    fails=$((fails + 1))
  fi
}

# Stream lines: kind pc va mem_size num_dest_regs latency off_path.
A='0x1000'
expect "same block pairs" "1,2" \
  "$(pairs "L 0x10 $A 8 1 4 0\nL 0x14 0x1010 8 1 4 0\n")"
expect "LOAD1 fuses once; LOAD2 never becomes LOAD1" $'1,2\n3,4' \
  "$(pairs "L 0 $A 8 1 4 0\nL 0 $A 8 1 4 0\nL 0 $A 8 1 4 0\nL 0 $A 8 1 4 0\n")"
expect "different blocks do not pair" "" \
  "$(pairs "L 0 $A 8 1 4 0\nL 0 0x1040 8 1 4 0\n")"
expect "store to the block blocks the pair" "" \
  "$(pairs "L 0 $A 8 1 4 0\nS 0 0x1028 8 0 1 0\nL 0 $A 8 1 4 0\n")"
expect "store to another block does not" "1,3" \
  "$(pairs "L 0 $A 8 1 4 0\nS 0 0x2000 8 0 1 0\nL 0 $A 8 1 4 0\n")"
expect "block-crossing LOAD2 does not pair" "" \
  "$(pairs "L 0 $A 8 1 4 0\nL 0 0x103c 8 1 4 0\n")"
expect "block-crossing LOAD1 is skipped" "2,3" \
  "$(pairs "L 0 0x103c 8 1 4 0\nL 0 $A 8 1 4 0\nL 0 0x1008 8 1 4 0\n")"
expect "load without a destination is ignored" "2,3" \
  "$(pairs "L 0 $A 8 0 4 0\nL 0 $A 8 1 4 0\nL 0 $A 8 1 4 0\n")"
expect "off-path ops are invisible and unnumbered" "1,2" \
  "$(pairs "L 0 $A 8 1 4 0\nL 0 $A 8 1 4 1\nO 0 0 0 1 1 1\nL 0 $A 8 1 4 0\n")"
expect "distance bound is exclusive (3 < 3 fails)" "" \
  "$(IF_DISTANCE=3 pairs "L 0 $A 8 1 4 0\nO 0 0 0 1 1 0\nO 0 0 0 1 1 0\nL 0 $A 8 1 4 0\n")"
expect "distance 2 < 3 pairs" "1,3" \
  "$(IF_DISTANCE=3 pairs "L 0 $A 8 1 4 0\nO 0 0 0 1 1 0\nL 0 $A 8 1 4 0\n")"

# Pass 2: LOAD2's dependents wake when LOAD1 completes. LOAD1 maps at cycle 1.
pass2() {
  printf "$1" | ./harness 1 > /dev/null
  printf "$1" | IF_DISTANCE=${2:-512} ./harness 2 | grep -v '^Ideal fusion'
}
expect "pass 2: LOAD1 completes after LOAD2 maps" \
  $'1 role=1 partner=2\n2 role=2 partner=1 woke_at=41 wake=41 done=41\nLOAD1_TAGGED 1\nLOAD2_TAGGED 1' \
  "$(pass2 "L 0 $A 8 1 40 0\nL 0 0x1010 8 1 4 0\n")"
expect "pass 2: LOAD1 completes before LOAD2 maps" \
  $'1 role=1 partner=3\n3 role=2 partner=1 woke_at=3 wake=2 done=2\nLOAD1_TAGGED 1\nLOAD2_TAGGED 1' \
  "$(pass2 "L 0 $A 8 1 1 0\nO 0 0 0 1 1 0\nL 0 0x1010 8 1 4 0\n")"
expect "pass 2 distance drops farther pairs" \
  $'1 role=1 partner=2\n2 role=2 partner=1 woke_at=5 wake=5 done=5\nLOAD1_TAGGED 1\nLOAD2_TAGGED 1' \
  "$(pass2 "L 0 $A 8 1 4 0\nL 0 $A 8 1 4 0\nL 0 0x2000 8 1 4 0\nO 0 0 0 1 1 0\nL 0 0x2000 8 1 4 0\n" 2)"

# Invalid settings must stop the run with a clear message.
rejects() {
  local name=$1 pass=$2
  shift 2
  if printf "L 0 $A 8 1 4 0\n" | env "$@" ./harness "$pass" > out 2> err; then
    echo "FAIL $name: accepted"
    fails=$((fails + 1))
  elif grep -q "invalid configuration" err; then
    echo "ok   $name"
  else
    echo "FAIL $name: $(cat err)"
    fails=$((fails + 1))
  fi
}
rejects "rejects pass 4" 4
rejects "rejects distance 0" 1 IF_DISTANCE=0
rejects "rejects empty log" 1 IF_LOG=
rejects "rejects empty measure log in pass 3" 3 IF_MEASURE_LOG=

if [[ -n "$DUMP" ]]; then
  python3 "$TESTS/gen_stream.py" 50000 1 > stream
  for d in 512 64; do
    rm -f ideal_fusion_candidates.csv ideal_fusion_measure.csv
    IF_DISTANCE=$d ./harness 1 < stream > /dev/null
    cp ideal_fusion_candidates.csv "$DUMP/pass1-d$d.csv"
    IF_DISTANCE=$d ./harness 2 < stream > "$DUMP/pass2-d$d.txt"
    IF_DISTANCE=$d ./harness 3 < stream > /dev/null
    cp ideal_fusion_measure.csv "$DUMP/pass3-d$d.csv"
  done
  echo "wrote $DUMP"
fi

if [[ $fails -ne 0 ]]; then
  echo "$fails failure(s)"
  exit 1
fi
echo "all passed"
