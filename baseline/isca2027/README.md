# ISCA 2027 baseline results

Scarab from this branch at `89b5145` (`src/PARAMS.in` == `PARAMS.golden_cove`),
run with `--icache_size 32768 --power_intf_on 1` through scarab-infra
(`codverch/scarab-infra`, branch `isca-2027`, descriptor `json/isca2027/baseline.json`;
window rules and exceptions in `json/isca2027/README.md` and `windows_audit.json`).

11 applications, 32 simpoints, all finished. Each simpoint: 20M warmup, then
10M measured on its own segment (appworld segment 9: 9.35M, program end;
clickhouse: 10M warmup, all the release holds).

- `collected_stats.csv` — `./sci --collect-stats isca2027/baseline`
- `runs/isca2027/datacenter/<app>/<cluster_id>/` — measured (`*.stat.0.csv`)
  and warmup (`*.stat.0.csv.warmup`) stats, `PARAMS.out`, `mcpat.out`
