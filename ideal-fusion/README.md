# Ideal fusion

Ideal fusion is an oracle model of load µop fusion for Scarab. It fuses every
pair of load µops that a fused load could serve correctly, so it bounds what
any practical fusion mechanism can gain. The HPCA 2027 I-Fuse submission uses
it as the "ideal" bar, against a no-fusion baseline, Helios [1], and RFP [2].

The model needs oracle knowledge. When a load is fetched, real hardware cannot
know whether a younger load will touch the same cache block. Ideal fusion gets
this knowledge by simulating twice. The first run finds the pairs. The second
run fuses them.

For example, suppose µop 100 loads 8 bytes at offset 0 of block A, and µop 160
loads 8 bytes at offset 16 of block A, with no store to A in between. Pass 1
logs the pair (100, 160). Pass 2 runs the same trace again. µop 100 performs
one L1-D access for both loads, and µop 160 takes no ROB entry, load queue
entry, reservation station, or execution port.

This directory ports Scarab branch `hpca2027-ideal-fusion` at the commit the
paper binary was built from (`740c40604`). The algorithm is unchanged.
[Validation](#validation) gives the evidence.

## Usage

Build Scarab with ideal fusion. This needs the upstream Scarab history in
this repository, network access for the Scarab submodules, and Scarab's usual
toolchain.

```
ideal-fusion/scripts/build.sh /path/to/tree
```

Run both passes from separate directories, with identical trace, ROI, and
warmup flags. These are the paper's settings:

```
cp ideal-fusion/PARAMS.in pass1/PARAMS.in
cp ideal-fusion/PARAMS.in pass2/PARAMS.in
FLAGS="--frontend memtrace --cbp_trace_r0 TRACE.zip \
  --memtrace_roi_begin B --memtrace_roi_end E \
  --full_warmup 20000000 --inst_limit 30000000 --icache_size 32768"

(cd pass1 && /path/to/tree/src/scarab $FLAGS --ideal_fusion_pass 1 \
  --ideal_fusion_log ../pairs.csv)
(cd pass2 && /path/to/tree/src/scarab $FLAGS --ideal_fusion_pass 2 \
  --ideal_fusion_log ../pairs.csv)
```

Pass 1 is the baseline. It changes no pipeline state. Pass 2 prints
`Ideal fusion pass 2: loaded N candidate pair(s)` and writes the usual Scarab
stats. The fusion counters, such as `IDEAL_FUSION_FUSED_LOADS`, are in
`ideal_fusion.stat.0.out`. For many SimPoints, the paper used scarab-infra
(`json/hpca2027/ideal-fusion.sh`), which issues these same commands per
SimPoint and adds `--power_intf_on 1`.

Test the pass logic without traces:

```
ideal-fusion/tests/run.sh /path/to/tree
```

## Fusible pairs

Call the older load LOAD1 and the younger load LOAD2. The pair is fusible
when all of the following hold. The first three are the paper's correctness
constraints.

1. Both accesses lie in the same L1-D cache block (`--dcache_line_size`).
2. Both loads fit in the ROB at the same time. With the default
   `--ideal_fusion_distance 512`, the 512-entry ROB size, this means
   LOAD2 − LOAD1 ≤ 511 on-path µops. The code checks LOAD2 − LOAD1 < 512.
3. No store to that block is fetched between them.
4. Neither access crosses a block boundary.
5. Both loads write a register and have a nonzero address and size.
6. Each load joins at most one pair. A load that became a LOAD2 never
   becomes a LOAD1 later.

## Passes

Both passes number every on-path µop in fetch order. The numbers name the
same dynamic µops in both runs, as long as the trace, ROI, and warmup match.

**Pass 1: discover** (`--ideal_fusion_pass 1`). At fetch, each load searches
its cache block for an earlier load that satisfies the rules above. If it
finds one, it logs the pair. If not, it becomes a candidate LOAD1 itself.
Each store discards every candidate in its block. At most one candidate can
ever qualify, because a second one would have paired with the first when it
was fetched.

**Pass 2: apply** (`--ideal_fusion_pass 2`). Reads the pair log and tags each
fetched load as LOAD1, LOAD2, or neither. LOAD1 runs normally and performs
the single L1-D access. LOAD2 is modeled as follows.

| Resource                                   | LOAD2 uses it? |
| ------------------------------------------ | -------------- |
| Fetch, decode, and rename bandwidth        | yes            |
| A physical destination register            | yes            |
| Retire bandwidth                           | yes            |
| ROB capacity (`node_count`)                | no             |
| Load queue entry                           | no             |
| Reservation station entry, execution port  | no             |
| L1-D access                                | no             |

LOAD2 enters the ROB already done. Its dependents wake when LOAD1 wakes. A
per-pair Load2 buffer handles both orders. If LOAD1 completes first, LOAD2
completes as soon as it is renamed. Otherwise LOAD2 waits in the buffer
until LOAD1 wakes. In-order retirement keeps LOAD2 from retiring before
LOAD1.

**Pass 3: measure** (`--ideal_fusion_pass 3`). A diagnostic from the original
code. It loads the same pairs as pass 2 but fuses nothing. It logs when each
LOAD1 and LOAD2 really completes, which shows how often LOAD1 actually
finishes first, as pass 2 assumes.

The pair log is the only state the passes share. Its columns are:

```
load1_pc,load1_data_addr,load1_block_offset,load1_mem_size,load1_micro_op_num,
load2_pc,load2_data_addr,load2_block_offset,load2_mem_size,load2_micro_op_num,
micro_op_distance
```

Passes 2 and 3 match on the two `micro_op_num` columns. They check every row
for consistency and abort on a malformed one. They skip rows at or beyond
`--ideal_fusion_distance`, so a smaller pass-2 distance applies a subset of
the pass-1 pairs. Pass 3 writes
`role,this_micro_op_num,load1_micro_op_num,load2_micro_op_num,done_cycle,wake_cycle,cur_cycle`.

## Configuration

All knobs are Scarab parameters in `src/ideal-fusion/ideal_fusion.param.def`.
They are checked when the first µop is fetched. An invalid value stops the
run with `Ideal fusion: invalid configuration: ...`.

| Parameter | Default | Valid | Meaning |
| --------- | ------- | ----- | ------- |
| `--ideal_fusion_pass` | `1` | 0 to 3 | 0 off, 1 discover, 2 apply, 3 measure |
| `--ideal_fusion_distance` | `512` | ≥ 1 | ROB size the pair must fit in |
| `--ideal_fusion_log` | `ideal_fusion_candidates.csv` | nonempty | Pair log, written by pass 1 |
| `--ideal_fusion_measure_log` | `ideal_fusion_measure.csv` | nonempty | Pass 3 output |

Three existing Scarab parameters matter. `--dcache_line_size` sets the fusion
granularity and must match across passes. `--node_table_size` is the ROB
size, and the distance does not follow it automatically. The paper also ran
pass 2 with `--dcache_read_ports 2` and `3`.

Changes from the Scarab branch:

- The parameters moved from `general.param.def`, with the same names and
  defaults. Existing command lines work, except as noted below.
- `--ideal_fusion_type` is removed. Since at most one candidate qualifies,
  its two policies always chose the same LOAD1.
- The pass-3 output path was the environment variable
  `IDEAL_FUSION_MEASURE_OUT`. It is now `--ideal_fusion_measure_log`.
- A pass above 3 or a distance of 0 used to disable the model silently. Both
  are now errors.

## Implementation

The new sources are in `src/ideal-fusion/`.

| File | Contents |
| ---- | -------- |
| `ideal_fusion.c` | Pass 1 matching (`find_matching_load1`, `track_load`, `invalidate_loads_for_store`), log loading (`load_pair_indexes`), pass 2 tagging (`classify_load`) and Load2 buffer (`ideal_fusion_on_map`, `ideal_fusion_on_load1_wake`), pass 3 (`ideal_fusion_measure_on_wake`), parameter checks (`check_params`) |
| `ideal_fusion.h` | Pass enum, Load2 buffer, hook declarations |
| `ideal_fusion.param.def`, `.param.h` | Parameters |
| `ideal_fusion.stat.def` | Tag, fusion, load latency, and L1-D access counters |

`scarab.patch` adds the hooks to shared Scarab files.

| Site | Effect |
| ---- | ------ |
| `ft.cc` `FT::build` | Clears per-µop fusion fields, calls `ideal_fusion_on_fetch_op` for every fetched µop |
| `map_stage.c` `stage_process_op` | Calls `ideal_fusion_on_map` after wake-up lists are built |
| `map.c` `wake_up_ops` | On a register wake-up, calls `ideal_fusion_on_load1_wake` and `ideal_fusion_measure_on_wake` |
| `node_stage.c` `node_fill_rob` | LOAD2 skips the LSQ check and `node_count`, and enters done |
| `node_stage.c` `node_retire`, `flush_window`, `is_node_table_empty`, `node_precommit_update` | LOAD2 is not counted in the ROB, committed to the LSQ, or waited on for the D-cache |
| `issue_queue.cc` `IssueQueues::dispatch` | LOAD2 is never dispatched to a reservation station |
| `map_rename.c` | LOAD2 does not register as a source consumer. A second produce or commit of a register is ignored instead of asserting, for every µop |
| `op.h` | Three fields: µop number, role, partner µop number |
| `param_files.def`, `stat_files.def`, `CMakeLists.txt` | Register the new files |

The patch also carries the branch's non-fusion changes, because the paper
results were produced with them. These are register file occupancy stats,
an L1-D access counter, decoder entries for SHA256 and RDPID, a memtrace
reader fix for reused encodings, two power script fixes, and dropping
`pin_exec` from the Scarab build. `PARAMS.in` is upstream
`PARAMS.golden_cove` at the base commit, unchanged.

## Validation

`tests/run.sh` drives the real `ideal_fusion.c` through all three passes on
synthetic µop streams. Given a second argument, it also writes the outputs
for a random 50,000-µop stream.

- **Rules.** Hand-written streams check each pairing rule, both Load2 buffer
  orders, the pass-2 distance filter, and each parameter error. All pass.
- **Same outputs.** On the random stream, the port and the original branch
  code produce byte-identical pair logs, pass-2 tags and wake cycles, and
  pass-3 logs, at distances 512 and 64. The original ran with its default
  policy.
- **Same build.** `scripts/build.sh` builds from a fresh clone. Run without
  a trace, the original and ported binaries dump the same parameters, apart
  from the parameter changes listed above.

- **Same simulation.** Both passes of the original and ported binaries ran
  on two SimPoints, duckdb 5 and bfs 94, with the paper's flags. The pair
  logs are byte-identical. Every stat file is identical apart from host wall
  time. Every logged pair is tagged and fused (3,005,925 for duckdb, 454,545
  for bfs). Fusion cuts measured cycles by 6.7% on duckdb and 0.04% on bfs.

## Notes

- Pass 2 never checks that its µop numbers match pass 1. A different trace,
  ROI, warmup, or frontend setting shifts the numbering, and pass 2 then
  fuses the wrong loads without an error.
- Pass 1 creates the log only when it finds a pair. If it finds none, pass 2
  aborts because the file is missing.
- A store that crosses a block boundary discards candidates only in its
  first block. Stores with address 0 are ignored.
- The default pass is 1, so every run of this binary writes a pair log unless
  it passes `--ideal_fusion_pass 0`.
- Pass 3 also prints `Ideal fusion pass 2: loaded ...`. The text stays
  because scarab-infra checks for it.

## References

[1] S. Singh, A. Perais, A. Jimborean, A. Ros. Exploring Instruction Fusion
Opportunities in General Purpose Processors. MICRO 2022.

[2] S. Shukla, S. Bandishte, J. Gaur, S. Subramoney. Register File
Prefetching. ISCA 2022.
