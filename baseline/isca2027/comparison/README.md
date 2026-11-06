# I-Fuse, ideal fusion and RFP vs. the ISCA 2027 baseline

Same 11 applications, 40 simpoints and windows as `../` (20M warmup + 10M measured),
same flags (`--icache_size 32768 --power_intf_on 1`). Each mechanism is ported onto
the baseline Scarab `89b5145`:

| Config | Scarab branch | Commit | scarab-infra descriptor |
|---|---|---|---|
| nofuse, ifuse | `i-fuse-on-baseline` | 1532c62 | `json/isca2027/ifuse.json` |
| ideal fusion (pass 1 log, pass 2 apply) | `ideal-fusion-on-baseline` | 240d66c | `json/isca2027/idealfusion_pass{1,2}.json` |
| RFP | `rfp-on-baseline` | 6592d84 | `json/isca2027/rfp.json` |

`nofuse` (I-Fuse runtime training off) reproduces the baseline `Cumulative_Cycles`
exactly on all 40 simpoints, so it is used as the baseline here.

- `stats/*.csv` — `./sci --collect-stats` per experiment. `idealfusion.csv` holds
  only pass 2 (`ideal_pass2`); pass 2 overwrote pass 1's collection.
- `plot_ipc_gain.py` → `ipc_gain.{png,pdf,csv}`: weighted IPC gain per application.
  Weighted IPC is scarab-infra's `IPC` row, sum(w * instructions) / sum(w * cycles)
  over the measured windows with SimPoint weights. Geomean is over applications.

| | RFP | I-Fuse | Ideal fusion |
|---|---|---|---|
| Geomean weighted IPC gain | +0.8% | +9.8% | +15.6% |
