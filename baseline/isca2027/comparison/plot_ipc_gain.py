#!/usr/bin/env python3
"""Weighted-IPC gain over the baseline for I-Fuse, ideal fusion and RFP.

IPC per application is scarab-infra's own `IPC` row in collected_stats.csv:
sum(w * Periodic_Instructions) / sum(w * Periodic_Cycles) over the
application's simpoints (measured 10M windows only, SimPoint weights).
Gain = IPC_config / IPC_baseline - 1. The last group is the geomean of the
per-application IPC ratios.

Baseline IPC comes from the `nofuse` configuration of the I-Fuse experiment,
which reproduces ../collected_stats.csv cycle-for-cycle on all 40 simpoints.
"""
import csv
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
import numpy as np

HERE = Path(__file__).resolve().parent
csv.field_size_limit(sys.maxsize)

# (label, stats file, configuration name, colour)
SERIES = [
    ("RFP",          "rfp.csv",         "rfp",         "#999999"),
    ("I-Fuse",       "ifuse.csv",       "ifuse",       "#0072B2"),
    ("Ideal fusion", "idealfusion.csv", "ideal_pass2", "#D55E00"),
]
BASE = ("ifuse.csv", "nofuse")


def weighted_ipc(path, config):
    rows = {}
    with open(HERE / "stats" / path, newline="") as f:
        for r in csv.reader(f):
            if r and r[0] in ("IPC", "Configuration", "Workload"):
                rows[r[0]] = r[3:]
    out = {}
    for ipc, cfg, wl in zip(rows["IPC"], rows["Configuration"], rows["Workload"]):
        if cfg == config:
            out[wl.rsplit("/", 1)[-1]] = float(ipc)
    return out


def main():
    font = HERE / "fonts" / "LiberationSerif-Regular.ttf"
    if font.exists():
        font_manager.fontManager.addfont(str(font))
        family = "Liberation Serif"
    else:
        family = "DejaVu Serif"
    plt.rcParams.update({
        "font.family": family, "font.size": 9, "axes.labelsize": 10,
        "pdf.fonttype": 42, "axes.linewidth": 0.8,
    })

    base = weighted_ipc(*BASE)
    apps = sorted(base)
    gains, table = [], []
    for label, path, cfg, _ in SERIES:
        ipc = weighted_ipc(path, cfg)
        ratio = np.array([ipc[a] / base[a] for a in apps])
        geo = float(np.exp(np.log(ratio).mean()))
        g = np.append(ratio, geo) * 100 - 100
        gains.append(g)
        table.append((label, g))

    names = [a.replace("_", "\n").replace("cachebench", "cache\nbench").replace("clickhouse", "click\nhouse") for a in apps] + ["Geomean"]
    x = np.arange(len(names), dtype=float)
    x[-1] += 0.5                       # separate the geomean group
    w = 0.26

    fig, ax = plt.subplots(figsize=(7.0, 2.4))
    for i, ((label, _, _, colour), g) in enumerate(zip(SERIES, gains)):
        ax.bar(x + (i - 1) * w, g, w, color=colour, edgecolor="black",
               linewidth=0.5, label=label, zorder=3)
    # value labels on the geomean group only
    for i, g in enumerate(gains):
        v = g[-1]
        ax.text(x[-1] + (i - 1) * w, v + (0.4 if v >= 0 else -0.4),
                f"{v:.1f}", ha="center", va="bottom" if v >= 0 else "top",
                fontsize=7)

    ax.axhline(0, color="black", linewidth=0.8, zorder=4)
    ax.axvline((x[-2] + x[-1]) / 2, color="black", linewidth=0.5,
               linestyle=":", zorder=2)
    ax.set_xticks(x)
    ax.set_xticklabels(names, fontsize=7.5)
    ax.tick_params(length=0)
    ax.set_xlim(x[0] - 0.6, x[-1] + 0.6)
    lo = min(g.min() for g in gains)
    hi = max(g.max() for g in gains)
    ax.set_ylim(min(-2, np.floor(lo) - 1), np.ceil(hi / 5) * 5 + 5)
    ax.set_ylabel("Weighted IPC gain\nover baseline (%)")
    ax.yaxis.grid(True, linestyle=":", color="black", linewidth=0.5, zorder=0)
    ax.set_axisbelow(True)
    ax.legend(loc="upper left", ncol=3, frameon=True, fancybox=False,
              edgecolor="black", framealpha=1, fontsize=8)
    fig.tight_layout(pad=0.3)

    out = HERE / "ipc_gain"
    fig.savefig(out.with_suffix(".png"), dpi=400)
    fig.savefig(out.with_suffix(".pdf"))

    with open(HERE / "ipc_gain.csv", "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["app", "baseline_ipc"] + [s[0] + "_gain_pct" for s in SERIES])
        for j, a in enumerate(apps):
            wr.writerow([a, f"{base[a]:.4f}"] + [f"{g[j]:.2f}" for _, g in table])
        wr.writerow(["geomean", ""] + [f"{g[-1]:.2f}" for _, g in table])
    print(open(HERE / "ipc_gain.csv").read())


if __name__ == "__main__":
    main()
