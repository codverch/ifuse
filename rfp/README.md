# Register File Prefetching (RFP)

This is the RFP baseline from the I-Fuse HPCA 2027 evaluation, ported from the
`hpca2027-rfp` branch of our Scarab fork. RFP predicts a load's address at
rename and reads the L1 into the load's destination register before the load
dispatches, so a correctly predicted L1 hit costs its dependents almost
nothing. It implements a version of [Register File Prefetching][rfp-paper]
(ISCA 2022) on top of upstream Scarab [`40803f796`][base], the commit the
branch forked from, not on this repository's newer `src/`.

## Overview

A load that hits the L1 still waits `DCACHE_CYCLES` (5 here) after dispatch
before its dependents can run. Many loads, however, have addresses that are
predictable well before dispatch. RFP uses the time between rename and
dispatch to read the predicted address early. A right, timely prediction lets
the load skip the L1. A wrong or late one only costs the L1 bandwidth the
prefetch used. Nothing is flushed.

## Design

RFP has three parts: a predictor, a prefetch queue, and a tracker per physical
register. A load passes through them in four steps.

1. **Train at retire.** The prediction table (PT) is a PC-indexed stride
   predictor. Each committed load updates its entry. A repeated stride raises
   a 1-bit confidence with probability `1/2^rfp_prob_shift`, so only loads
   with a long-stable stride are predicted. Training at retire keeps wrong-path
   addresses out. To keep entries small, an entry stores the 12-bit page offset
   of the last address and a pointer into a page address table (PAT) of page
   frames shared by many entries.

2. **Predict at rename.** Once the destination register is allocated, a
   confident entry predicts `last + stride * (inflight + 1)`. `inflight`
   counts older instances of the same PC that are renamed but not retired,
   which have not trained the PT yet. The request, tagged with the destination
   register, goes into a 64-entry RFP FIFO beside the load queue.

3. **Read the L1 at lowest priority.** Each cycle, after all demand accesses,
   the dcache stage tries the oldest live FIFO entry. It waits if an older
   store may write the predicted bytes or no read port is free, and it blocks
   the entries behind it. An L1 hit marks the register ready `DCACHE_CYCLES`
   later. An L1 miss is dropped.

4. **Check at dispatch.** The load's real address is now known.
   - Exact match, prefetch done: the load reads the register instead of the
     L1, uses no L1 port, and completes at `max(now + 1, ready_cycle)`. That
     hides all of the L1 latency if the data is there, and part of it if the
     data is still in flight.
   - Exact match, prefetch not yet done: the prefetch is dropped and the load
     runs normally.
   - Mismatch: the tracker entry is cleared, confidence is reset, and the load
     runs normally. A right-line, wrong-offset prediction is a mismatch,
     because RFP fetches a value, not a line.

On a recovery, each removed op clears its tracker entry, so a recycled register
cannot inherit a squashed load's prefetch.

Scarab does not model register values, so the tracker holds no data. It
records which dynamic load a prefetch belongs to, which line it read, and when
the value is usable.

## Paper correspondence

> Sudhanshu Shukla, Sumeet Bandishte, Jayesh Gaur, and Sreenivas Subramoney.
> Register File Prefetching. ISCA 2022, pages 410–423.
> [doi:10.1145/3470496.3527398][rfp-paper]

```bibtex
@inproceedings{shukla2022register,
  title     = {Register File Prefetching},
  author    = {Shukla, Sudhanshu and Bandishte, Sumeet and Gaur, Jayesh and Subramoney, Sreenivas},
  booktitle = {Proceedings of the 49th Annual International Symposium on Computer Architecture},
  pages     = {410--423},
  year      = {2022},
  doi       = {10.1145/3470496.3527398}
}
```

The implementation broadly follows the paper's mechanism (§3): a PC-indexed
stride table trained at retire, launch after rename into a FIFO at lowest L1
priority, an exact address check at the load, and no flush on a wrong
prefetch. It differs in how it handles stores, L1 misses, and scheduling
timing, and the HPCA runs tune the predictor per application. The table
below was checked against both the paper and the code.

| Paper component | Implementation | What is modeled | Differences or omissions |
|---|---|---|---|
| Prefetch Table (§3.1): PC-indexed stride predictor with 16-bit tag, 2-bit utility, 1-bit confidence, 7-bit inflight; trained at retire; confidence raised with probability 1/16; stride change resets confidence and utility; low-utility entries evicted | `rfp.c`: `rfp_train_entry`, `rfp_victim`, `rfp_predict` | All of it | The paper's default is 1/16 and 1K entries. The HPCA runs use 4K entries (512×8) and tune the probability (1/2^0 to 1/2^3) and stride width (5 or 16 bits) per application, which the paper does not do. |
| Inflight counter (§3.1): incremented at allocation, decremented at commit and for each squashed load; prefetch address from base, stride, and inflight | `rfp.c`: `rfp_prefetch_launch`, `rfp_train_retire`, `rfp_track_squash` | Predicts `base + stride * (inflight + 1)` | None material |
| Compressed storage (§3.5, Table 1): 5-bit stride, 12-bit page offset, 6-bit pointer into a 64-entry, 4-way PAT of 44-bit page addresses; stale pointers relearned after a misprediction | `rfp.c`: `rfp_pat_alloc_ptr`, `rfp_entry_va` | Same layout and stale-pointer behavior | The paper's headline results record the full 64-bit address and use the PAT only in a sensitivity study (§5.5.4, −0.09%). The code always uses the PAT, with way-0 replacement, which the paper leaves unspecified. |
| Launch (§3.2): after rename, packet carries VA and prfid, lowest priority against regular loads, 64-entry FIFO with older requests first | `rfp.c`: `rfp_prefetch_launch_prf`; `lsq.cc` FIFO; `dcache_stage.c`: `dcache_process_lsq_rfp_queue` | All of it | The code serves at most one prefetch per cycle, and a stalled head blocks the rest. The paper says only that RFP uses free load ports. |
| In-flight stores (§3.2.1): scan older stores youngest first; on a match, wait and take the store's data; for unknown store addresses, follow the memory-disambiguation prediction and flush if it is wrong | `rfp.c`: `rfp_blocked_by_older_stores`; `lsq.cc`: `older_store_overlaps` | Overlap detection against the store queue and post-retire store buffer | No store-to-RFP forwarding: the prefetch waits until the store leaves the buffers. No disambiguation prediction: any older store with an unknown address blocks. Both make RFP later than in the paper and never cause a flush. |
| L1 misses (§3.2.2): the prefetch continues to lower levels | `dcache_stage.c` | Not modeled | Dropped. The paper measures that path at a 0.02% upside (§5.5.5). |
| DTLB misses (§3.2.2): the prefetch is dropped | none | Not modeled | Scarab's dcache stage has no DTLB, so no RFP is ever dropped for a TLB miss |
| RFP-inflight bit and scheduling (§3.3, Fig. 9): bit set when the prefetch starts its L1 lookup; a load that sees the bit at wake-up schedules its dependents to meet the prefetch; a later bit saves part of the latency | `dcache_stage.c`: `update_dcache_stage`; `rfp_prf.c`: `rfp_prf_ready` | The prefetch's L1 hit marks the register ready `DCACHE_CYCLES` later. The load completes at `max(now + 1, ready)`, giving full or partial savings. | The check happens when the load reaches the dcache stage, not at its wake-up, and dependents are not speculatively scheduled. A full hit saves 4 of 5 cycles in both, matching Fig. 9. |
| Drop when the load becomes ready first (§3.3) | `dcache_stage.c`; `rfp_prf.c`: `rfp_prf_mark_dropped` | Same | None |
| Wrong address (§3.3, §3.5): the load re-reads the cache and speculatively woken dependents are cancelled | `dcache_stage.c`: `update_dcache_stage` | Exact-address check, then a normal demand access. Confidence is reset. | No dependents are woken early, so there is no cancellation cost. The paper does not say whether confidence resets on a mismatch. |
| L1 bandwidth (§3, §5.6): a correct prefetch replaces the load's L1 access | `dcache_stage.c` | A served load takes no L1 port | None |
| Context (path-based) prefetcher (§3.1, §5.5.3, +0.3%) | none | Not modeled | |
| Value prediction combined with RFP (§5.3) | none | Not modeled | |
| Storage and power (§3.5, §5.6, qualitative in the paper) | `power_scarab_config.cc`: `power_print_core_rfp` | PT and PAT given to McPAT as SRAMs | Scarab addition. Entries are rounded up to 8 bytes. |

## Implementation map

`src/prefetcher/` holds the new files and is copied into Scarab's `src/`.
`scarab.patch` edits existing Scarab files. Each RFP function's comment names
its caller.

| Step | Scarab caller | RFP code |
|---|---|---|
| init | `memory.c:init_memory` | `rfp_init`, `rfp_prf_init` |
| fetch | `icache_stage.c:icache_process_ops` | `rfp_predict_at_fetch`, which only resets per-op fields |
| rename | `map_stage.c:stage_process_op`, after `reg_file_rename` | `rfp_prefetch_launch` → `lsq_rfp_enqueue` |
| L1 read | end of `dcache_stage.c:update_dcache_stage` | `dcache_process_lsq_rfp_queue` (patch) |
| dispatch | `dcache_stage.c:update_dcache_stage`, before the port check | `rfp_prf_ready`, `rfp_prf_clear`, `rfp_prf_mark_dropped` |
| retire | `node_stage.c:node_retire` | `rfp_train_retire` |
| commit | `lsq.cc:LSQ_Unit::commit` | `LSQ::drop_rfp_upto` (patch) |
| recovery | `thread.c:recover_seq_op_list` | `rfp_track_squash`, `rfp_prf_clear_op` |

Besides those hooks, `scarab.patch` carries:

- per-op RFP fields (`op.h`), the export of `reg_file_get_reg_type`
  (`map_rename.{c,h}`), and parameter, stat, and McPAT wiring;
- an inert memory-request path (`memory/`): no request is ever marked as RFP,
  so `mem_cancel_rfp_prefetch` never acts, but it is kept to match the source;
- changes that are not RFP but were in effect for the RFP results:
  `LSQ::free` searches instead of popping the front, `LSQ::recover` drops two
  assertions, `pref_common.c` zero-initializes `Pref_Req_Info`, new
  `LD_*_LATENCY`, `REG_FILE_*`, and `DCACHE_ACCESS_*` stats, and a
  `bin/power/power_intf.pl` fix that passes `%params` and `%stats` by
  reference.

Left out because they don't affect simulated timing: `.gitignore` edits, a
trace-patching `warn()`, a `PIN_ROOT` path in the PIN makefile, the
reformatting of `map_stage.c` and `node_stage.c`, and a `PARAMS.golden_cove`
edit (below).

## Configuration

Scarab reads `PARAMS.in` from its working directory, and command-line flags
override it. This `PARAMS.in` is the branch's `src/PARAMS.in`, whose values
equal upstream `PARAMS.golden_cove` at the base commit: a 48 KB, 12-way,
8-bank L1D with one read port and 5-cycle latency. Only its header changed.

The branch also edited `PARAMS.golden_cove` to `dcache_assoc 8`, and its
archived `rfp-final-results` used that. Later runs, including the 2026-10-11
`sf-rfp` reruns, used 12 ways. Don't compare numbers across the two.

`PARAMS.in` sets no RFP flags. RFP is **on by default**, so a baseline must
pass `--rfp_on 0`. The HPCA runs used:

```
--rfp_on 1 --rfp_conf_max 1 --rfp_stride_signed 1
--rfp_pt_num_sets 512 --rfp_pt_num_ways 8 --rfp_pat_num_sets 64 --rfp_pat_num_ways 4
```

plus, per app (from scarab-infra `json/hpca2027/rfp.sh`):

| Apps | `--rfp_prob_shift` | `--rfp_stride_bits` |
|---|---|---|
| appworld, clickhouse, core_bench, duckdb, grpc, leveldb, memcached, rocksdb, sqlite, terminal_bench | 2 | 5 |
| bfs-init, bfs_web-google | 3 | 16 |
| pagerank-init, pagerank_gnutella31, sssp-init, sssp_ego-facebook | 1 | 16 |
| dfs-init, dfs_web-google | 0 | 16 |

All flags and defaults are in [`src/prefetcher/rfp.param.def`][params].

## Build and execution

You need the Scarab build toolchain (CMake and GCC; the HPCA builds used the
scarab-infra Docker image) and network access for Scarab's DynamoRIO and XED
submodules. Exec-driven runs also need Intel PIN 3.15 and `PIN_ROOT`.

```sh
rfp/scripts/build.sh /path/to/scarab-rfp    # optional second argument: make target, default opt
```

This clones this repository, checks out `40803f796`, fetches submodules,
copies in `src/`, applies `scarab.patch`, and runs `make`. The binary is
`/path/to/scarab-rfp/src/build/opt/scarab`.

To run one SimPoint as in the HPCA runs:

```sh
mkdir run && cd run
cp /path/to/ifuse/rfp/PARAMS.in .
/path/to/scarab-rfp/src/build/opt/scarab \
  --frontend memtrace --cbp_trace_r0 <trace.zip> \
  --inst_limit 30000000 --full_warmup 20000000 --use_fetched_count 1 \
  --rfp_on 1 --rfp_conf_max 1 --rfp_stride_signed 1 \
  --rfp_pt_num_sets 512 --rfp_pt_num_ways 8 \
  --rfp_pat_num_sets 64 --rfp_pat_num_ways 4 \
  --rfp_prob_shift 2 --rfp_stride_bits 5
```

RFP counters go to `rfp.stat.0.out`. Start with `RFP_COVERAGE`,
`RFP_PRED_CORRECT`, `RFP_FULL_MITIGATED`, and `RFP_PARTIAL_MITIGATED`.

## Validation

- **Source.** All 28 files RFP touches (7 new, 21 patched) match
  `hpca2027-rfp` at `bc84b5c35` once comments, whitespace, and split string
  literals are normalized. RFP source did not change after `57cfe922e`, the
  commit behind the HPCA binary.
- **Configuration.** The non-comment lines of `PARAMS.in` match the branch's
  `src/PARAMS.in`.
- **Build and behavior.** `scripts/build.sh` builds cleanly in the
  scarab-infra Docker image, as does `hpca2027-rfp` itself. Run on
  terminal_bench SimPoint 5 with the submitted flags (20M warmup, 30M
  instructions), the port, the branch build, and the submitted
  `hpca2027-main` result agree exactly: 15,403,840 cycles, IPC 1.94757, and
  identical `rfp.stat.0.out` counters (e.g. 1,501,743 loads served from the
  PRF).

Known limitations:

- `scarab.patch` applies only to `40803f796`, not to this repository's `src/`,
  whose memory system has since been rewritten.
- With `--rfp_stride_signed 0 --rfp_stride_bits 16`, strides above 32767 wrap
  and never confirm. The HPCA runs use signed strides.
- Confidence draws use libc `rand()`, which other Scarab code also calls, so
  changing those callers can shift RFP results.

## Credits

RFP was written by Ved Lakshminarayanan (`veds-changes`) and codverch
(`hpca2027-rfp`). Scarab is copyright HPS/SAFARI Research Groups and is
distributed under the repository's [`LICENSE`][license]. Patched Scarab files
keep their original headers.

[rfp-paper]: https://doi.org/10.1145/3470496.3527398
[base]: https://github.com/litz-lab/scarab/commit/40803f7964d686e39527eaf08a48a7c996967508
[params]: src/prefetcher/rfp.param.def
[license]: ../LICENSE
