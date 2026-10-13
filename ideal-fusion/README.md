# Ideal fusion

Ideal fusion is an oracle model of load µop fusion. It fuses every pair of
load µops that satisfies three correctness constraints, so it bounds what any
practical fusion mechanism, including I-Fuse, can gain. It is the "ideal"
bar in the characterization of the HPCA 2027 submission, compared against a
no-fusion baseline, Helios [1], and RFP [2].

This directory is a port of Scarab branch `hpca2027-ideal-fusion`
(simulator code as of `740c40604`, the commit the paper binary
`scarab_740c40604` was built from). Later commits on the branch add only
results and two power-script fixes, which are included. The algorithm is unchanged. See
[Validation](#validation) for the evidence.

## Design

A load pair is *fusible* when LOAD1 (older) and LOAD2 (younger, in program
order) satisfy all of the following. The first three are the paper's
constraints. The rest are additional rules the code applies.

1. Both accesses lie in the same L1-D cache block (`--dcache_line_size`).
2. LOAD2 − LOAD1 < `--ideal_fusion_distance` on-path µops (default 512, the
   ROB size).
3. No store to that block is fetched between them.
4. Neither access crosses a block boundary.
5. Both loads write a register and have a nonzero address and size.
6. Each load joins at most one pair. A load that becomes LOAD2 is never a
   LOAD1 later.

Knowing which future load will pair with the current one takes oracle
knowledge. The model gets it by simulating twice.

**Pass 1: discover** (`--ideal_fusion_pass 1`). A normal simulation. At
fetch, every on-path µop gets a sequence number. Loads that could be a LOAD1
are kept, bucketed by cache block. Each new load searches its block for a
LOAD1 that satisfies the rules above, and each store discards every
candidate in its block. Matched pairs go to the pair CSV. Pass 1 changes no
pipeline state, so its timing equals a no-fusion run of the same binary.

**Pass 2: apply** (`--ideal_fusion_pass 2`). Reads the pair CSV and renumbers
on-path µops the same way, so each logged number names the same dynamic
µop. At fetch, each load is tagged LOAD1, LOAD2, or neither. LOAD1 executes
normally and performs the one L1-D access for the pair. LOAD2 is modeled as
follows:

| Resource                                   | LOAD2 uses it? |
| ------------------------------------------ | -------------- |
| Fetch, decode, and rename bandwidth        | yes            |
| A physical destination register            | yes            |
| Retire bandwidth                           | yes            |
| ROB capacity (`node_count`)                | no             |
| Load queue entry                           | no             |
| Reservation-station entry, execution port  | no             |
| L1-D access                                | no             |

LOAD2 enters the ROB already done. Its dependents wake when LOAD1 wakes, and
it takes LOAD1's wake and done cycles. A per-pair *Load2 buffer* handles
either order. If LOAD1 completes first, LOAD2 completes when it is mapped.
Otherwise LOAD2 waits in the buffer until LOAD1 wakes. In-order retirement
keeps LOAD2 from retiring before LOAD1.

**Pass 3: measure** (`--ideal_fusion_pass 3`). Not part of the two-pass
design, but in the original code. It loads the same pairs as pass 2, fuses
nothing, and logs when each LOAD1 and LOAD2 really completes. This checks
how often the older LOAD1 actually finishes before LOAD2, which pass 2's
forwarding assumes.

## Data flow

```
          trace, ROI, warmup (identical in every pass)
                 |                           |
                 v                           v
   pass 1: number + match ------> pair CSV ------> pass 2: tag + fuse
   (baseline timing)            (--ideal_fusion_log)     (fused timing)
                                       |
                                       +---------> pass 3: tag + log cycles
                                                   (--ideal_fusion_measure_log)
```

The only state shared between passes is the pair CSV. Its header is fixed:

```
load1_pc,load1_data_addr,load1_block_offset,load1_mem_size,load1_micro_op_num,
load2_pc,load2_data_addr,load2_block_offset,load2_mem_size,load2_micro_op_num,
micro_op_distance
```

(one line in the file). Passes 2 and 3 match on the two `micro_op_num`
columns only. They re-check every row (header, field count, ordering,
distance, block consistency under the current `--dcache_line_size`) and
abort on any error. Rows with `micro_op_distance >= --ideal_fusion_distance`
are skipped, so a smaller pass-2 distance applies a subset of the pass-1
pairs.

Pass 3 writes `role,this_micro_op_num,load1_micro_op_num,load2_micro_op_num,done_cycle,wake_cycle,cur_cycle`,
one row per completion of a paired load.

## Implementation map

New files, in `src/ideal-fusion/` (Scarab `src/ideal-fusion/`):

| File | Contents |
| ---- | -------- |
| `ideal_fusion.c` | Pass 1 matching (`find_matching_load1`, `track_load`, `invalidate_loads_for_store`, `log_matched_pair`); CSV loading (`load_pair_indexes`); pass 2 tagging (`classify_load`) and Load2 buffer (`ideal_fusion_on_map`, `ideal_fusion_on_load1_wake`); pass 3 (`ideal_fusion_measure_on_wake`); parameter checks (`check_params`) |
| `ideal_fusion.h` | Pass and policy enums, Load2 buffer, hook declarations |
| `ideal_fusion.param.def`, `.param.h` | Parameters (new location, see below) |
| `ideal_fusion.stat.def` | Tag, fusion, load-latency, and L1-D access counters |

Hooks in shared Scarab files, in `scarab.patch`:

| Site | Effect |
| ---- | ------ |
| `ft.cc` `FT::build` | Clears per-op fusion fields; calls `ideal_fusion_on_fetch_op` for every fetched op |
| `map_stage.c` `stage_process_op` | Calls `ideal_fusion_on_map` after wake-up lists are built |
| `map.c` `wake_up_ops` | On a register wake-up, calls `ideal_fusion_on_load1_wake` and `ideal_fusion_measure_on_wake` |
| `node_stage.c` `node_fill_rob` | LOAD2 skips the LSQ check and `node_count`, and enters as `OS_DONE` |
| `node_stage.c` `node_retire`, `flush_window`, `is_node_table_empty`, `node_precommit_update`, `debug_print_node_table` | LOAD2 is not counted in the ROB, not committed to the LSQ, and not waited on for the D-cache; retired-load stats |
| `issue_queue.cc` `IssueQueues::dispatch` | LOAD2 is never dispatched to an RS |
| `map_rename.c` `reg_table_entry_read`, `reg_file_release_prev` | LOAD2 does not register as a source consumer |
| `map_rename.c` `reg_table_entry_produce`, `reg_file_release_prev` | A second produce or commit of a register is ignored instead of asserting. This applies to every op; it changes nothing in runs that did not assert before |
| `op.h` | `ideal_fusion_micro_op_num`, `ideal_fusion_load_role`, `ideal_fusion_partner_micro_op_num` |
| `param_parser.c/.h` | Parser for `--ideal_fusion_type` names |
| `param_files.def`, `stat_files.def`, `CMakeLists.txt` | Register the new files |

The patch also carries the branch's non-fusion changes, because the paper
results were produced with them: register-file occupancy stats
(`map_rename.c`, `map_stage.c`, `core.stat.def`), the L1-D access counter
(`dcache_stage.c`), decoder entries for SHA256 and RDPID (`x86_decoder.cc`),
a memtrace reader fix for reused encodings
(`memtrace_trace_reader_memtrace.cc`), power-script fixes
(`bin/power/power_intf.pl`, `bin/scarab_globals/scarab_paths.py`), and
dropping `pin_exec` from the Scarab build (`Makefile`,
`pin/pin_exec/makefile`). The branch's `.gitignore` entries for staged
binaries are left out.

`PARAMS.in` is the Golden Cove configuration the runs used, unchanged.

## Configuration

All knobs are Scarab command-line parameters, defined in
`src/ideal-fusion/ideal_fusion.param.def`. They are checked when the first
op is fetched. An invalid value stops the run with
`Ideal fusion: invalid configuration: ...`.

| Parameter | Type | Default | Valid | Meaning |
| --------- | ---- | ------- | ----- | ------- |
| `--ideal_fusion_pass` | uns | `1` | 0–3 | 0 off, 1 discover, 2 apply, 3 measure |
| `--ideal_fusion_distance` | uns | `512` | ≥ 1 | Fusion window: LOAD2 − LOAD1 < distance, in on-path µops |
| `--ideal_fusion_log` | string | `ideal_fusion_candidates.csv` | nonempty | Pair CSV: written by pass 1, read by passes 2 and 3 |
| `--ideal_fusion_type` | name | `oldest-first` | `oldest-first`, `most-recent` | Pass 1 choice among qualifying LOAD1s. See Limitations: currently has no effect |
| `--ideal_fusion_measure_log` | string | `ideal_fusion_measure.csv` | nonempty in pass 3 | Pass 3 output |

Existing Scarab parameters that shape the model: `--dcache_line_size`
(fusion granularity, must match across passes), `--node_table_size` (the
ROB size the distance mirrors, not linked automatically), and
`--dcache_read_ports` (the paper also ran pass 2 with 2 and 3 ports).

Migration from the Scarab branch: the first four parameters moved from
`general.param.def` to `ideal_fusion.param.def` with the same names and
defaults, so existing command lines work unchanged. The pass-3 output path
was the environment variable `IDEAL_FUSION_MEASURE_OUT`. It is now
`--ideal_fusion_measure_log`, with the same default. Values that were
accepted before but silently disabled the model (a pass above 3, a distance
of 0) are now rejected.

Hash-table sizes in `ideal_fusion.c` and `ideal_fusion.h` are fixed. They
change lookup speed, not results.

## Execution

Build. This needs the I-Fuse repository's upstream Scarab history, network
access for the Scarab submodules, and Scarab's usual toolchain:

```
ideal-fusion/scripts/build.sh /path/to/tree
```

The binary is `/path/to/tree/src/build/opt/scarab`. Run both passes from
separate directories, with identical trace, ROI, and warmup flags. The paper
runs used these settings, taken from their `PARAMS.out`:

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

Pass 2 prints `Ideal fusion pass 2: loaded N candidate pair(s) ...` (pass 3
prints the same line). Results are the usual Scarab stat files. The
ideal-fusion counters are in `ideal_fusion.stat.0.out`, e.g.
`IDEAL_FUSION_FUSED_LOADS`. The paper ran many SimPoints through
scarab-infra (`json/hpca2027/ideal-fusion.sh` with descriptors
`ideal-fusion-pass1.json` and `ideal-fusion.json`), which issues the same
commands per SimPoint and adds `--power_intf_on 1`.

Tests, against any tree from `build.sh`:

```
ideal-fusion/tests/run.sh /path/to/tree
```

## Validation

Performed on this port:

- **Same algorithm.** The patched tree differs from the branch tip only in
  `src/ideal-fusion/` and in where parameters are registered
  (`general.param.def`, `param_files.def`), plus one Makefile comment.
  `ideal_fusion.c` changes are: named pass constants instead of `1`/`2`/`3`,
  the measure path parameter, `check_params`, comments, and deleted
  commented-out debug prints.
- **Same outputs.** `tests/run.sh <tree> <dump_dir>` drives the real
  `ideal_fusion.c` through all three passes on a 50,000-op synthetic stream.
  Run against the original branch tree and against this port, its 12 output
  files (pass-1 CSVs, pass-2 tags and wake cycles, pass-3 logs, at distances
  512 and 64 and both policies) are byte-identical.
- **Rules and parameters.** The same script checks each pass-1 rule, both
  Load2 buffer orders, the pass-2 distance filter, and the four validation
  errors with hand-written streams. All pass. Changing the distance changes
  the pairs (8,517 at 512 vs 6,290 at 64 on the synthetic stream).
- **Build script.** `scripts/build.sh` runs end to end from a fresh clone
  and produces a working binary. `tests/run.sh` passes on its tree.
- **Same build and parameters.** Both trees build with `make opt`. Without
  a trace, both binaries dump identical `PARAMS.out` apart from the new
  `IDEAL_FUSION_MEASURE_LOG` line, and stop at the same point.

Not performed: end-to-end simulation. No memtrace traces are available in
the environment where this port was made, so IPC equivalence on real
workloads is argued from the identical sources and harness outputs above,
not measured.

Limitations and discrepancies:

- **`--ideal_fusion_type` has no effect.** For two qualifying LOAD1s X and Y
  to coexist in one block, Y would have had to skip pairing with X when Y
  was fetched, which rule 6 and the matching order rule out. So at most one
  candidate ever qualifies, and both policies pick it. The harness confirms
  identical output under both.
- **Exclusive bound.** The paper allows pairs "separated by at most 512
  µops". The code requires distance < 512, i.e. at most 511.
- **Alignment is not checked.** Pass 2 trusts that µop *n* is the same
  dynamic µop as in pass 1. A different trace, ROI, warmup, or frontend
  setting shifts the numbering, and pass 2 then fuses the wrong loads
  without an error.
- **No pairs, no file.** Pass 1 creates the CSV only when it finds a pair.
  If it finds none, pass 2 aborts because the file is missing.
- **Stores.** A store that crosses a block boundary discards candidates
  only in its first block. Stores with address 0 are ignored.
- **Default is pass 1.** Any run of this binary writes a pair CSV unless it
  passes `--ideal_fusion_pass 0`.
- Pass 3 also logs "Ideal fusion pass 2: loaded ...". The text is left as
  is because scarab-infra checks for it.

## References

[1] S. Singh, A. Perais, A. Jimborean, A. Ros. Exploring Instruction Fusion
Opportunities in General Purpose Processors. MICRO 2022. (Helios)

[2] S. Shukla, S. Bandishte, J. Gaur, S. Subramoney. Register File
Prefetching. ISCA 2022.
