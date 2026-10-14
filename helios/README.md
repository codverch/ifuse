# Helios

This is a Scarab model of Helios, which fuses two load micro-ops into one when
they read the same 64 B region, even if they are not adjacent in the program
and use different address registers. The I-Fuse paper compares against it.

The design is from Singh, Perais, Jimborean, and Ros, "Exploring Instruction
Fusion Opportunities in General Purpose Processors," MICRO 2022 ([PDF][paper]).
Only load pairs are modeled. Store pairs and non-memory fusion are not.

## Use

Build the simulator, run the unit tests, and run one SimPoint with and
without Helios:

```sh
cd helios
make build        # scripts/build.sh: src/build/opt/scarab, no PIN needed
make test         # 32 checks of the UCH, the region tests, and the predictor
scripts/run.sh            ../src/build/opt/scarab <traces>/bfs/traces_simp/trace/103.zip out/bfs bfs
scripts/run.sh --baseline ../src/build/opt/scarab <traces>/bfs/traces_simp/trace/103.zip out/bfs-base bfs
```

`run.sh` prints Scarab's summary line, for example:

```
** Core 0 Finished:    insts:30000000    cycles:...    -- 1.83 IPC (1.83 IPC) --
```

Speedup is the baseline's cycles over Helios's. Each run directory holds
`sim.log`, `PARAMS.out`, and the stat files; Helios's counters are in
`helios.stat.0.out`. For a quick check, pass a short window: `run.sh ... bfs 1
6000000 2000000` simulates 4M instructions after a 2M warmup.

With PIN installed, `cd src && SCARAB_ENABLE_PT_MEMTRACE=1 make opt` builds the
same simulator plus the PIN frontend.

## Design

Helios (paper Sec. IV) adds three things to an out-of-order core.

1. **Training at commit.** The Unfused Committed History (UCH) holds the line
   addresses of the last 6 unfused loads. A committing load that hits the same
   line within 64 micro-ops has found a pair. The distance trains the Fusion
   Predictor for the younger load's PC.
2. **Prediction at decode.** The Fusion Predictor gives a younger load (the
   *tail*) the distance back to an older load (the *head*). The micro-ops in
   between are the *catalyst*. The pair fuses in the allocation queue, which
   sits between decode and rename, if the head is still there. The fused
   micro-op takes one ROB, IQ, and LQ entry and does one cache access.
   Adjacent loads with the same base register fuse statically, without the
   predictor (consecutive fusion).
3. **Correctness after fusion.** The fused micro-op waits in the IQ until the
   tail renames, then also waits for the tail's address registers. If the tail
   depends on the head through the catalyst, or the catalyst holds a
   serializing instruction, the tail unfuses at rename and takes a second
   dispatch slot. The head retires only with its whole group. A pair that does
   not fit one region flushes the pipeline.

## Model

Scarab's IDQ (144 entries) stands in for the paper's allocation queue (140).
The model is in `src/helios/`. This table compares it with the paper.

| Aspect | Paper | Model |
|---|---|---|
| Pairs | Load pairs and store pairs in one 64 B region. | Load pairs only, same region test. |
| Consecutive pairs | Fused statically at decode: same base register, contiguous. | Same, using the address registers and oracle addresses. The tail still takes a rename and dispatch slot; in the paper it disappears before rename. |
| Prediction | At decode. | At fetch, in the decoupled frontend. No timing effect. |
| Head in the allocation queue | Required when the tail is decoded. | Checked when the tail enters the IDQ: the head must not have been renamed. |
| Window | 64 micro-ops. | Same (`--helios_fusion_window`). |
| UCH | 6 entries, line tag match, LRU by commit number. | Same, with full tags and op numbers. |
| Fusion Predictor | Tournament: 512x4 PC table, 512x4 PC xor history table, 2048 selector, 8-bit tags, pseudo-LRU. | Same sizes. True LRU. |
| Confidence | 2-bit counter, fuse at 3, reset on a misprediction. | Threshold, increment, and decrement are parameters. Defaults are the paper's; the paper runs tuned them per application (`scripts/knobs.tsv`). |
| Nesting | At most 2 overlapping pairs. | Same. |
| Deadlock, serializing catalyst | Found at the tail's rename. The head waited in the IQ; the tail unfuses and takes a second dispatch slot. | Same timing. Found with a replay of fetch-order register history. Also follows store-to-load dependences, which Scarab enforces from oracle addresses. |
| Tail sources | The fused micro-op waits for all tail sources, including catalyst producers. | Same. A liveness guard drops the wait after 4096 cycles stuck at the ROB head (counted). |
| Tail resources | No ROB, IQ, LQ entry, no execution, no cache port. Uses a dispatch slot. | Same. The tail still uses a retire slot, like Scarab's fused compare-and-branch. |
| Pairs across two lines | Two serialized accesses, about one cycle. | Tail data one cycle after the head's. |
| Commit | Head retires with its catalyst and tail. | Same. |
| Address misprediction | Found when the fused micro-op executes. Flush, refetch the tail. | Known at fetch from oracle addresses. Flush at the tail's execute, keep the tail's macro-op. The head is not held for such a pair. |
| Catalyst mispredict or fault | Unfuse the pair, flush. | The flush squashes the tail and frees the head. |
| ISA | RISC-V, one micro-op per instruction. | x86 micro-ops. Flushes keep whole macro-ops. |

## Code

| File | Role |
|---|---|
| `src/helios/heliosFusion.c` | Fetch, allocation queue, rename, data delivery, retire, and recovery hooks. The file header walks through the life of a pair. |
| `src/helios/heliosFusionPredictor.c` | Fusion Predictor. |
| `src/helios/heliosHistory.c` | UCH and the region tests. |
| `src/helios/helios.param.def` | Parameters. |
| `src/helios/helios.stat.def` | Counters. |
| `helios/scripts/` | `build.sh`, `run.sh`, and the tuned knobs. |
| `helios/tests/` | Unit tests (`make test`). |

Hooks in shared Scarab files:

| Stage | Scarab function | Helios call |
|---|---|---|
| Init | `init_decoupled_fe` | `heliosFusionInit` |
| Fetch | `FT::predict_ft` | `heliosFetchHook`; snapshot predictor state for a flush |
| Allocation queue | `IDQ_Stage::process_input_stage_data` | `heliosAllocQueueHook` |
| Rename | `stage_process_op` (map_stage.c) | `heliosRenameHook` |
| Dispatch | `node_fill_rob` | tail takes no ROB or LQ entry; `heliosDispatchSlots` |
| IQ | `IssueQueues::dispatch`, `cmp_wake` | tail skips the IQ; head held until its tail renames |
| D-cache | four load completion sites | `heliosCompleteFusedTail` |
| Execute | `update_exec_stage` | `helios_sched_recovery` on an address misprediction |
| Retire | `node_retire` | `heliosUnblockRobHead`, `heliosExtCommitGroupBlocks`, `heliosCommit` |
| Recovery | `cmp_recover` | `flushTables`, predictor and register-map repair |

About half of the shared changes exist so that a Helios flush can squash
correct-path micro-ops, which Scarab otherwise never does: a replay ring in
the memtrace frontend, branch predictor checkpoints, FT teardown, register map
repair, and relaxed off-path asserts in each stage's recovery.

## Configuration

`src/PARAMS.in` is `src/PARAMS.golden_cove`. Helios is off unless
`--helios_do_fusion 1`.

| Parameter | Default | Meaning |
|---|---|---|
| `--helios_do_fusion` | 0 | Enable Helios. |
| `--helios_confidence_threshold` | 3 | Confidence needed to predict. |
| `--helios_confidence_increment` | 1 | Added per UCH training of the same distance. |
| `--helios_confidence_decrement` | 3 | Subtracted per address misprediction. |
| `--helios_fusion_window` | 64 | Largest distance, in micro-ops. |

`run.sh` adds `--icache_size 32768` and the tuned confidence of
`scripts/knobs.tsv`, from scarab-infra `json/hpca2027/helios-tuned-*.json`.
Fixed sizes are in `heliosFusion.h`, `heliosHistory.h`, and
`heliosFusionPredictor.h`.

`helios.stat.def` documents each counter. The main ones:

- `HELIOS_FUSIONS`, split into `HELIOS_FUSED_CSF` and `HELIOS_FUSED_NCSF`.
  Coverage is `HELIOS_FUSIONS_COMMITTED / HELIOS_ONPATH_LOADS`.
- `HELIOS_REJECT_*` and `HELIOS_UNFUSE_*`: why a predicted pair did not fuse.
- `HELIOS_TRAIN_DIST_*` and `HELIOS_MISSED_PAIR_*`: pairs the UCH found at
  commit that Helios did not fuse.
- `HELIOS_FUSED_HEAD_FORCE_RELEASED`: liveness guard firings. Should be near 0.

## Validation

Checks run on this model (Scarab main `bbd4f686`, `src/PARAMS.in`):

- `make test`: 32 unit checks pass.
- Debug build (all asserts on), Helios on, every application, first SimPoint,
  4M instructions after a 1M warmup: all 11 finish.
- Optimized build, first SimPoint, 4M instructions after a 2M warmup, tuned
  knobs. Speedup over Helios off:

  | App | Speedup | App | Speedup |
  |---|---|---|---|
  | bfs | 11.5% | clickhouse | 17.2% |
  | dfs | 9.1% | memcached | 2.2% |
  | pagerank | 6.4% | leveldb | 0.2% |
  | cachebench | 0.0% | terminal_bench | 0.0% |
  | duckdb | -0.2% | appworld | -0.3% |
  | corebench | -0.7% | **mean** | **4.1%** |

  ClickHouse fuses 38% of its loads and bfs 31%. The liveness guard fired at
  most 5 times per run.
- Ablation: letting the tail keep its ROB and LQ entries changes the mean by
  0.02 points. The 512-entry ROB rarely fills in these windows.

Not validated: full 30M-instruction runs on every SimPoint, and the PIN
frontend.

## Limitations

- **Machine.** `PARAMS.golden_cove` decodes and renames 6 wide. The paper
  decodes 8 and renames 5 so that the allocation queue fills; with equal
  widths the queue holds fewer heads, and Helios finds fewer pairs.
- **Oracle addresses.** The model checks a pair at fetch with trace
  addresses, so it knows early which pairs will flush or unfuse. Their timing
  follows the paper: the flush happens at the tail's execute, the unfusion at
  its rename.
- **Single core.** The predictor and head tables are global.
- **Paper runs.** The HPCA 2027 numbers came from the older port on Scarab
  `hpca2027-helios` (830808db1), which lacked several behaviors above: it
  kept the tail's ROB and LQ entries, let the head issue after leaving the
  allocation queue, skipped catalyst-produced tail sources, and had no
  consecutive fusion. The paper runs also used a 1..30000000 window that
  repeats one region for bfs, dfs, pagerank, appworld, duckdb, and leveldb.

[paper]: https://webs.um.es/aros/papers/pdfs/ssingh-micro22.pdf
