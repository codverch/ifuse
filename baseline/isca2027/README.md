# ISCA 2027 baseline results

Scarab from this branch at `89b5145` (`src/PARAMS.in` == `PARAMS.golden_cove`),
run with `--icache_size 32768 --power_intf_on 1` through scarab-infra
(`codverch/scarab-infra`, branch `isca-2027`, descriptor `json/isca2027/baseline.json`;
window rules and exceptions in `json/isca2027/README.md` and `windows_audit.json`).

11 applications, 40 simpoints, all finished. Each simpoint: 20M warmup, then
10M measured on its own segment (appworld segment 9: 9.35M, program end).
Measured windows never overlap within an application.

cachebench and clickhouse use 5 evenly spaced 10M segments at weight 0.2 each
(systematic sampling): SimPoint with k=5 filled only 2 and 3 clusters, so each
is one phase. cachebench: segments 2, 4, 6, 7, 9 of the released trace.
clickhouse: re-traced from the released binary and query; the first 260M
instructions of the worker thread (ARCH_REGDEPS), segments 2, 8, 14, 19, 25.

- `collected_stats.csv` — `./sci --collect-stats isca2027/baseline`
- `runs/isca2027/datacenter/<app>/<cluster_id>/` — measured (`*.stat.0.csv`)
  and warmup (`*.stat.0.csv.warmup`) stats, `PARAMS.out`, `mcpat.out`
