# I-Fuse: correlated load µop fusion

This directory holds the Scarab model of I-Fuse, the mechanism of "I-Fuse:
Correlated Load Micro-Op Fusion" (HPCA 2027 submission). I-Fuse learns pairs
of loads that read the same cache block and fuses each pair into one cache
access. The older load (LD1) performs the access. The younger load (LD2) does
no L1-D access, and its consumers wake one cycle after LD1's. LD2 still takes
a ROB entry, a load queue entry, and a reservation station entry, and computes
its address on a load port, so that it retires in order and verifies the
predicted address. We refer to this configuration as *1-cycle-delayed I-Fuse*.
It is the default here.

The code ports Scarab branch `ifuse-realistic-ld2` at `ae923d935` with its
`--ifuse_ld2_realistic 1`, then changes eight details to match the paper's
design ([Changes](#changes-from-the-scarab-branch)). It adds a model of
[L1-D bank conflicts](#l1-d-bank-conflicts) for LD2's word and
[PMU counters](#pmu-counters) that show what fusion did for each LD2 and its
consumers.

## Use

`make` patches upstream Scarab with I-Fuse and builds it in `build/scarab`.
`scripts/run.sh` runs one SimPoint with and without fusion and summarizes the
difference:

```console
$ make
$ scripts/run.sh build/scarab/src/build/opt/scarab \
    /dev/shm/ifuse/duckdb/traces_simp/trace/5.zip 30000001 60000000 /dev/shm/out
running nofuse/ and ifuse/ in /dev/shm/out
cycles:     3548050 (no fusion), 3464514 (I-Fuse)
speedup:    1.024x
LD2s:       853543 retired, 849600 fused (99.5%)
LD2 data:   34.2% of fused LD2s earlier, by 6.83 cycles on average
consumers:  25.4% of 726543 ready earlier, by 7.81 cycles on average
            2.7% waited on another source
banks:      731369 fused accesses held LD2's bank, 87055 conventional-load delays
```

`nofuse/` and `ifuse/` hold Scarab's usual stat files. The I-Fuse counters are
in `ifuse.stat.0.out`. Extra arguments to `run.sh` go to both runs; for
example, `--ifuse_ld2_bank_conflict 0` removes L1-D bank conflicts and
`--ifuse_ld2_wake_delay 0` lets LD2's consumers wake with LD1's.

`make test` runs the unit tests without traces or a Scarab build. `make clean`
removes `build/`.

## Setup

The build needs:

- This repository with its upstream Scarab history. `scripts/patch.sh` checks
  out the Scarab commit I-Fuse was developed on (`40803f7`).
- Network access for Scarab's submodules (DynamoRIO, XED, mbuild).
- Scarab's toolchain: GCC, CMake, and Python 3.

A build takes about 5 minutes and 3 GB of disk. `build/` can be a symlink to
larger storage. `scripts/patch.sh <dir>` and `scripts/build.sh <dir>` create
and build a tree elsewhere.

`run.sh` needs memtrace SimPoint traces. The paper simulates 20M instructions
of warmup and 10M measured, so the ROI spans 30M instructions ending at the
end of the SimPoint. I-Fuse's tables train during warmup and keep their state.

## Overview

### Motivation

Loads are a major source of backend pressure. A load that misses the L1-D
holds a ROB entry, a load queue entry, and a reservation station entry until
its data returns. Yet two loads often read the same 64-byte block within a few
hundred µops. Each performs its own L1-D access, even though the first access
already reads the whole block. An oracle that fuses every such pair improves
IPC by about 20% on the paper's datacenter applications.

### Key idea

Our key idea is to learn, at retirement, which pairs of static loads
repeatedly read the same block at a fixed offset, and to fuse their future
instances at fetch. A pair is fused if and only if a confident table entry
predicts it and the younger load arrives with exactly the predicted address
and size.

### Mechanism

I-Fuse has three steps. We use one running example: LD1 at PC `A` loads 8
bytes at offset 0 of a block, and LD2 at PC `B` loads 8 bytes at offset 16 of
the same block, once per loop iteration.

**Training at retirement.** Each retired load searches the *retired-load
buffer* (RLB, 16 sets × 8 ways, indexed by block address) for an older load
that read the same block, fewer than 512 µops earlier, with no store to the
block in between. On a hit, the two loads form a pair and the entry is cleared;
on a miss, the load writes its own entry, evicting the oldest in its set. A
match is a candidate pair, keyed by LD1's PC, LD2's PC, the offset between
them, and LD2's size. The *training table* (TT, 32 sets × 4 ways) counts how
often each pair repeats. After 32 sightings, the pair moves to the *fusion
candidate table* (FCT, 128 sets × 4 ways, tree-PLRU). In the example, after
32 iterations retire, the FCT holds (A, B, +16, 8 bytes).

**Prediction at fetch.** When a fetched load's PC hits a confident FCT row, the
load becomes LD1. I-Fuse predicts LD2's PC and address, LD1's address plus the
learned offset. It records the prediction in the *active pair table* (APT,
32 sets × 8 ways, keyed by LD2's PC) and the *access check index* (ACI,
64 sets × 4 ways, keyed by LD2's block). The APT holds one prediction per LD2
PC: when two instances of a pair are in flight, the younger LD1 replaces the
older one's entry and frees its register, so LD2 maps to the youngest
instance. A prediction expires once the fetched stream is more than 512 µops
past LD1. A later load whose PC hits the APT becomes LD2 if its address and
size match the prediction. Otherwise the
prediction is wrong: the FCT row loses confidence, and the younger consumers
are flushed once the load executes. In the example, every fetched A predicts B
at A's address + 16.

**Execution.** LD1 executes normally. LD2 is renamed and takes a ROB entry, a
load queue entry, and a reservation station entry, like any load. It issues
to a load port and computes its address, which verifies the prediction, but it
does no L1-D access: LD1's access already returned its data. When LD1's data
arrives, LD2's consumers wake one cycle later (`--ifuse_ld2_wake_delay 1`). LD2
completes once both its value and its address are ready, and then retires in
program order. The delay pays for splitting the second value
off the wide access and writing it through an existing register file write
port. In the example, if LD1's data arrives in cycle 300, LD1's consumers can
issue in cycle 300 and LD2's in cycle 301.

### Hardware cost

The five tables follow Table 3 of the paper:

| Table | Organization | Index | Bits per entry | Storage |
| ----- | ------------ | ----- | -------------- | ------- |
| FCT | 128 sets × 4 ways | LD1 PC | 101 | 6.31 KB |
| APT | 32 sets × 8 ways | LD2 PC | 53 | 1.66 KB |
| ACI | 64 sets × 4 ways | block address | 55 | 1.72 KB |
| RLB | 16 sets × 8 ways | block address | 102 | 1.59 KB |
| TT | 32 sets × 4 ways | LD1 PC | 104 | 1.63 KB |

Every table picks its set from the low bits of its index, with no hash, as a
cache does: the low PC bits for the FCT, APT, and TT, and the low block-number
bits for the ACI and RLB. The FCT, APT, and ACI use tree-PLRU replacement, the
TT uses 2-bit RRIP, and the RLB evicts the oldest entry in a set.

### L1-D bank conflicts

A fused access reads two words, and with 8 banks interleaved every 4 bytes,
LD2's word usually sits in another bank. As the paper requires, LD2's word goes
through the same bank check as a conventional load
(`--ifuse_ld2_bank_conflict 1`, the default): the fused access holds LD2's bank
read port for its cycle, ahead of conventional accesses. A conventional load to that bank finds the port
taken and retries the next cycle.

In the example, LD1 reads offset 0 (bank 0) and LD2's word is at offset 16
(bank 4). In the cycle LD1 accesses the L1-D, a conventional load to bank 4
waits one cycle.

At the start of each D-cache cycle, every fused LD1 in the stage reserves
LD2's bank port; then all accesses arbitrate in program order as before. If LD1
misses its own bank port, it gives LD2's port back. A word in LD1's own bank
needs no second port.

### PMU counters

The counters answer three questions: how often LD1 serves LD2, how much
earlier LD2's value arrives, and which consumers wake earlier. A consumer
gains only if all its sources are ready earlier.

The last two need the time LD2's value would have arrived without fusion. The
counters estimate it per LD2: LD2 issues once its address sources are ready,
generates its address the next cycle, and then sees the L1-D hit latency, but
no earlier than LD1's line arrives. For example, LD1 misses at cycle 100 with
data at 300, and LD2's address is ready at 150. Without fusion, LD2 reaches the
cache at 151, merges with the miss, and gets data at 300. With fusion, its
value is visible at 301, one cycle later.

A consumer's gain is its ready cycle without fusion minus its ready cycle with
fusion. Its ready cycle is when its last source arrives, but no earlier than
the cycle after it entered the scheduler.

| Counter (`IFUSE_PMU_...`) | Meaning |
| ------------------------- | ------- |
| `LD2_RETIRED` | Retired loads predicted as LD2 |
| `LD2_FUSED`, `LD2_ISSUED`, `LD2_MISPREDICTED` | Served by LD1, issued on their own after losing their pair, or mispredicted |
| `LD2_EARLY`, `LD2_AVG_EARLY_CYCLES` | Fused LD2s whose value arrived earlier, and by how much |
| `LD2_NOT_EARLY`, `LD2_LATE_CYCLES` | Fused LD2s whose value arrived no earlier, and the total cycles later |
| `LD2_AVG_SAVED_CYCLES`, `LD2_AVG_LOST_CYCLES` | Cycles saved by early LD2s and lost by late ones, each averaged over all fused LD2s; their difference is the average net saving per fused LD2 |
| `CONSUMERS` | Retired ops with a fused LD2 source |
| `CONSUMERS_EARLY`, `CONSUMERS_AVG_EARLY_CYCLES` | Ready earlier because of fusion, and by how much |
| `CONSUMERS_OTHER_SOURCE` | Another source arrived no earlier than LD2 would have |
| `CONSUMERS_LD2_NOT_EARLY` | LD2's value itself was not earlier |
| `CONSUMERS_BOUND_BY_ISSUE` | Entered the scheduler after both times |
| `CONSUMERS_EARLY_<class>`, `CONSUMERS_EARLY_CYCLES_<class>` | Early consumers by class: `LOAD`, `STORE`, `BRANCH`, `INT_ALU`, `INT_MULDIV`, `FP_VECTOR`, `OTHER` |

The bank model adds `IFUSE_LD2_BANK_*` counters: fused accesses that held
LD2's bank, LD2 words in LD1's bank, LD2 banks already held, and the
conventional-load delays they caused.

## Configuration

All parameters are in [`src/ifuse/ifuse.param.def`](src/ifuse/ifuse.param.def),
which documents each one. The defaults are the paper's 1-cycle-delayed
I-Fuse, so a run with no I-Fuse flags reproduces it.

| Parameter | Default | Meaning |
| --------- | ------- | ------- |
| `--ifuse_runtime_training_enabled` | `1` | Learn pairs at retirement. 0 is the no-fusion baseline |
| `--ifuse_fusion_distance` | `512` | LD2 is fewer than this many µops after LD1, in training and for a live prediction |
| `--ifuse_training_insert_threshold` | `32` | Sightings before a pair enters the FCT |
| `--ifuse_fct_hash_bits`, `--ifuse_fct_ways` | `9`, `4` | 2^9 = 512 FCT rows in 4-way sets |
| `--ifuse_ld2_wake_delay` | `1` | Cycles after LD1's data before LD2's consumers wake |
| `--ifuse_ld2_bank_conflict` | `1` | Model L1-D bank conflicts for LD2's word |

### Changes from the Scarab branch

- The branch's realistic-LD2 mode (`--ifuse_ld2_realistic 1`) is the only
  mode here, so LD2 keeps its ROB, load queue, and reservation station
  entries. The branch's default, in which LD2 skips them, its two ablations,
  and its offline (PGO) FCT preload are not ported.
- The APT keeps only the youngest LD1 per LD2 PC, as the paper describes and as
  `hpca2027-revision-ifuse` later did. The ported build kept several and
  matched the oldest (`--ifuse_apt_match_policy 0` in
  the paper's runs); that parameter is gone.
- A live prediction expires after 512 µops, as training uses. The branch
  counted 512 loads.
- The L1-D bank model is new and on by default, as the paper describes. The
  paper's runs had no bank model.
- The RLB is set-associative (16 sets × 8 ways, a 32-bit partial line tag), as
  in the paper and in the later `hpca2027-revision-ifuse` branch. The ported
  build used a fully associative RLB. On the two SimPoints below, this changes
  no cycle count.
- The TT is indexed by LD1's PC, as in the paper. The branch hashed the whole
  pair (both PCs, the offset, and LD2's size).
- Every table indexes by the low bits of its PC or block number. The branch
  mixed the FCT, APT, ACI, and TT indexes with a 64-bit multiplicative hash,
  which hardware would not compute on this path.
- LD2 takes LD1's reserved register at rename and allocates none of its own.
  The branch allocated one and freed it at once.
- A store between LD1 and LD2 that writes LD2's bytes makes LD2 flush at
  execute, as the paper's LSQ check does (`IFUSE_RAW_STORE_FLUSHES`). The
  branch never caught it, so LD2 kept LD1's stale value.
- `--ifuse_training_insert_threshold` defaults to 32 (the branch defaulted to
  1000, and the paper's runs passed 100). CoreBench has about 1,850 static
  pairs that recur, far more than the TT's 128 entries, so at 100 most pairs
  are evicted before they are promoted. At 32, the SimPoint-weighted average
  speedup over the 11 applications rises from 11.6% to 12.0%, and CoreBench
  rises from 17.9% to 20.4%.
- The PMU counters are new.

## Implementation

- [`src/ifuse/`](src/ifuse/): the I-Fuse model, copied into Scarab's `src/` by
  `patch.sh`.
- [`scarab.patch`](scarab.patch): changes to shared Scarab files.
- [`PARAMS.in`](PARAMS.in): the Golden Cove configuration the paper used.
- [`scripts/`](scripts/): `patch.sh` and `build.sh` create and build a Scarab
  tree; `run.sh` runs one trace with and without fusion.
- [`tests/`](tests/): unit tests for the retired-load history, tree-PLRU, and
  PMU counters.
- [`Makefile`](Makefile): `make`, `make test`, and `make clean`.

The sources, by step:

| Files | Contents |
| ----- | -------- |
| `ifuse_retired_load_history.c`, `ifuse_training_table.c`, `ifuse_train_retire.c` | Training at retirement: RLB, TT, promotion into the FCT |
| `ifuse_fct.c`, `ifuse_plru.c` | FCT with tree-PLRU replacement |
| `ifuse_apt.c`, `ifuse_aci.c` | Live predictions, matched against arriving loads |
| `ifuse_rename.c`, `ifuse_exec_pair.c`, `ifuse_pool.h` | LD2's register at rename, and its value when LD1 completes |
| `ifuse_recovery.c` | Flush after an LD2 misprediction |
| `ifuse_pmu.c` | PMU counters |
| `ifuse_stats.c`, `ifuse_load_tracker.c` | Other counters |

`scarab.patch` hooks these into Scarab:

- fetch (`ft.cc` and the frontend files): LD1 and LD2 prediction;
- rename (`map.c`, `map_rename.c`): LD2's register, and the PMU's wake hook in
  `simple_wake`;
- the D-cache (`dcache_stage.c`): LD2 completes its address generation
  without an L1-D access, and the bank model;
- retirement (`node_stage.c`): training and the PMU;
- recovery (`cmp_model.c`, `exec_stage.c`): misprediction flushes.

The patch also carries the branch's backend occupancy and stall counters
(`backend_occupancy.c`, `topdown.c`, `core.stat.def`), because the paper's
results include them, and drops `pin_exec` from the build, which memtrace runs
do not use.

## Validation

**Port.** We built the original branch at `ae923d935` and this port before
the changes above, and ran both with the paper's settings: the original
with the paper's I-Fuse flags and `--ifuse_ld2_realistic 1`, the port with
the bank model off. On duckdb SimPoint 5 and memcached SimPoint 149, with
fusion on and off, every simulated counter in every stat file is identical.
The only differences are host wall time, the two counters of the removed FCT
preload, and the new PMU and bank counters.

**Changes.** Each change alters timing, so the current model no longer
matches the branch. Cycles for the measured 10M instructions:

| SimPoint | No fusion | Port of the branch | This model |
| -------- | --------- | ------------------ | ---------- |
| duckdb 5, ROI 30000001-60000000 | 3,548,050 | 3,456,824 (1.026x) | 3,464,514 (1.024x) |
| memcached 149, ROI 1-30000000 | 6,130,180 | 5,711,171 (1.073x) | 5,651,996 (1.085x) |

On duckdb, the bank model alone costs the port about 1 point (1.026x to
1.017x).

**Tests.** `make test` checks the retired-load history, tree-PLRU, and the
PMU's no-fusion estimate in its three cases and each consumer class.

## Notes

- I-Fuse can slow a program down. When LD2's address is ready before LD1's,
  LD2 still waits for LD1's fused access, as the paper specifies. On pagerank
  SimPoint 97 (ROI 40000001-70000000), a third of the fused LD2s are 4.5 cycles
  ahead of LD1 on average; their late values hold registers longer, rename
  stalls on a full integer register file, and I-Fuse runs at 0.80x.

- The PMU's no-fusion time is an estimate. It ignores port and scheduler
  contention that LD2 would have faced, so it can understate how late LD2
  would have been. It also assumes LD2 would merge with LD1's miss rather than
  miss again.
- A consumer that renames after its LD2 retired is not counted, since LD2's
  value was already in the register file either way.
- Under the bank model, an older conventional load can lose a port to a fused
  access whose LD1 then misses its own port. It still waits, although LD2's
  word is never read. `IFUSE_LD2_BANK_CONFLICT_DELAYS_RELEASED` counts these.
- Training state is single-core. Multi-core runs must first partition the RLB
  and TT by core.
