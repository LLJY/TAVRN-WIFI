#!/usr/bin/env python3
"""
TAVRN Temporal Analysis Plotter

Generates publication-quality figures from temporal sweep data.
Reads time-series (#TS lines) from .log files and summary CSV.

Usage:
    python3 scripts/plot-temporal.py <sweep-tmpdir-or-results-dir> [--csv=summary.csv]

Produces figures in results/figures/
"""

import argparse
import csv
import os
import re
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np
import pandas as pd

# ─── Style ──────────────────────────────────────────────────────────────────

plt.rcParams.update(
    {
        "figure.figsize": (10, 6),
        "figure.dpi": 150,
        "font.size": 11,
        "axes.titlesize": 13,
        "axes.labelsize": 12,
        "legend.fontsize": 10,
        "lines.linewidth": 2,
        "axes.grid": True,
        "grid.alpha": 0.3,
        "savefig.bbox": "tight",
        "savefig.pad_inches": 0.15,
    }
)

PROTO_COLORS = {
    "TAVRN": "#2196F3",
    "OLSR": "#FF5722",
    "OLSR-Tuned": "#FF9800",
    "AODV": "#4CAF50",
    "AODV-Tuned": "#8BC34A",
    "DSDV": "#9C27B0",
}

PROTO_STYLES = {
    "TAVRN": "-",
    "OLSR": "--",
    "OLSR-Tuned": "-.",
    "AODV": ":",
    "AODV-Tuned": ":",
    "DSDV": "--",
}

PROTO_ORDER = ["TAVRN", "OLSR", "OLSR-Tuned", "AODV", "AODV-Tuned", "DSDV"]


# ─── Data loading ───────────────────────────────────────────────────────────


def parse_timeseries_from_logs(log_dir):
    """Parse #TS and #RUN lines from .log files in a directory."""
    ts_data = defaultdict(lambda: defaultdict(list))  # scenario -> proto -> [runs]
    run_data = defaultdict(
        lambda: defaultdict(list)
    )  # scenario -> proto -> [run_dicts]

    log_files = sorted(Path(log_dir).glob("*.log"))
    if not log_files:
        # Try looking in subdirectories (tmpdir structure)
        log_files = sorted(Path(log_dir).rglob("*.log"))

    for logfile in log_files:
        # Extract scenario and protocol from filename: {scenario}_{proto}.log
        name = logfile.stem
        parts = name.rsplit("_", 1)
        if len(parts) != 2:
            continue
        scenario, proto = parts

        samples_by_seed = defaultdict(list)
        with open(logfile) as f:
            for line in f:
                line = line.strip()
                if line.startswith("#TS,"):
                    fields = line.split(",")
                    if len(fields) >= 13:
                        seed = int(fields[2])
                        samples_by_seed[seed].append(
                            {
                                "t": float(fields[3]),
                                "overhead_bps": float(fields[4]),
                                "pdr": float(fields[5]),
                                "latency_ms": float(fields[6]),
                                "precision": float(fields[7]),
                                "recall": float(fields[8]),
                                "stale_ratio": float(fields[9]),
                                "alive": int(fields[10]),
                                "known": int(fields[11]),
                                "stale": int(fields[12]),
                            }
                        )
                elif line.startswith("#RUN,"):
                    fields = line.split(",")
                    run_dict = {}
                    for f in fields[3:]:
                        if "=" in f:
                            k, v = f.split("=", 1)
                            try:
                                run_dict[k] = float(v)
                            except ValueError:
                                run_dict[k] = v
                    run_data[scenario][proto].append(run_dict)

        for seed, samples in samples_by_seed.items():
            ts_data[scenario][proto].append(samples)

    return ts_data, run_data


def load_summary_csv(csv_path):
    """Load the summary CSV into a pandas DataFrame."""
    df = pd.read_csv(csv_path)
    return df


def average_timeseries(runs):
    """Average multiple time-series runs into mean + CI."""
    if not runs or not runs[0]:
        return None

    # Align on time steps (all runs should have same time points)
    time_points = [s["t"] for s in runs[0]]
    n_runs = len(runs)

    result = {"t": time_points}
    metrics = [
        "overhead_bps",
        "pdr",
        "latency_ms",
        "precision",
        "recall",
        "stale_ratio",
    ]

    for metric in metrics:
        values_at_t = []
        for t_idx in range(len(time_points)):
            vals = []
            for run in runs:
                if t_idx < len(run):
                    v = run[t_idx][metric]
                    if v >= 0:  # skip -1 sentinel values
                        vals.append(v)
            values_at_t.append(vals)

        means = []
        ci95s = []
        for vals in values_at_t:
            if vals:
                m = np.mean(vals)
                if len(vals) >= 2:
                    ci = 1.96 * np.std(vals, ddof=1) / np.sqrt(len(vals))
                else:
                    ci = 0
                means.append(m)
                ci95s.append(ci)
            else:
                means.append(np.nan)
                ci95s.append(0)

        result[f"{metric}_mean"] = means
        result[f"{metric}_ci"] = ci95s

    # Alive count (same across protocols)
    result["alive"] = [
        runs[0][i].get("alive", 0) if i < len(runs[0]) else 0
        for i in range(len(time_points))
    ]

    return result


# ─── Plotting functions ─────────────────────────────────────────────────────


def plot_timeseries_metric(
    ts_data,
    scenario,
    metric,
    ylabel,
    title,
    outpath,
    protocols=None,
    log_scale=False,
    show_alive=False,
    failure_time=None,
):
    """Plot a single metric over time for multiple protocols in one scenario."""
    if protocols is None:
        protocols = [p for p in PROTO_ORDER if p in ts_data.get(scenario, {})]

    fig, ax = plt.subplots()

    for proto in protocols:
        runs = ts_data.get(scenario, {}).get(proto, [])
        if not runs:
            continue
        avg = average_timeseries(runs)
        if avg is None:
            continue

        t = avg["t"]
        mean = avg[f"{metric}_mean"]
        ci = avg[f"{metric}_ci"]

        # Filter NaN
        valid = [i for i in range(len(mean)) if not np.isnan(mean[i])]
        if not valid:
            continue

        tv = [t[i] for i in valid]
        mv = [mean[i] for i in valid]
        cv = [ci[i] for i in valid]

        color = PROTO_COLORS.get(proto, "gray")
        style = PROTO_STYLES.get(proto, "-")

        ax.plot(tv, mv, style, color=color, label=proto)
        ax.fill_between(
            tv,
            [m - c for m, c in zip(mv, cv)],
            [m + c for m, c in zip(mv, cv)],
            alpha=0.15,
            color=color,
        )

    if failure_time is not None:
        ax.axvline(
            x=failure_time, color="red", linestyle="--", alpha=0.5, label="Node failure"
        )

    if show_alive and protocols:
        # Show alive count on secondary axis
        runs = ts_data.get(scenario, {}).get(protocols[0], [])
        if runs:
            avg = average_timeseries(runs)
            if avg:
                ax2 = ax.twinx()
                ax2.plot(avg["t"], avg["alive"], "k:", alpha=0.3, linewidth=1)
                ax2.set_ylabel("Alive nodes", color="gray", alpha=0.5)
                ax2.tick_params(axis="y", colors="gray")

    ax.set_xlabel("Time (s)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    if log_scale:
        ax.set_yscale("log")
    ax.legend(loc="best")
    fig.savefig(outpath)
    plt.close(fig)
    print(f"  Saved: {outpath}")


def plot_overhead_comparison_grid(
    ts_data, scenarios, outpath, protocols=None, failure_times=None
):
    """Multi-panel grid: overhead over time for multiple scenarios."""
    if protocols is None:
        protocols = ["TAVRN", "OLSR", "OLSR-Tuned"]

    n = len(scenarios)
    ncols = min(3, n)
    nrows = (n + ncols - 1) // ncols
    fig, axes = plt.subplots(
        nrows, ncols, figsize=(5.5 * ncols, 4 * nrows), squeeze=False
    )

    for idx, scenario in enumerate(scenarios):
        row, col = divmod(idx, ncols)
        ax = axes[row][col]

        for proto in protocols:
            runs = ts_data.get(scenario, {}).get(proto, [])
            if not runs:
                continue
            avg = average_timeseries(runs)
            if avg is None:
                continue

            t = avg["t"]
            mean = avg["overhead_bps_mean"]
            valid = [i for i in range(len(mean)) if not np.isnan(mean[i])]
            if not valid:
                continue

            tv = [t[i] for i in valid]
            mv = [mean[i] for i in valid]
            color = PROTO_COLORS.get(proto, "gray")
            style = PROTO_STYLES.get(proto, "-")
            ax.plot(tv, mv, style, color=color, label=proto, linewidth=1.5)

        if failure_times and scenario in failure_times:
            ax.axvline(
                x=failure_times[scenario],
                color="red",
                linestyle="--",
                alpha=0.4,
                linewidth=1,
            )

        ax.set_title(scenario, fontsize=11, fontweight="bold")
        ax.set_xlabel("Time (s)", fontsize=9)
        ax.set_ylabel("Overhead (B/s)", fontsize=9)
        ax.tick_params(labelsize=8)
        if idx == 0:
            ax.legend(fontsize=8, loc="best")

    # Hide unused subplots
    for idx in range(n, nrows * ncols):
        row, col = divmod(idx, ncols)
        axes[row][col].set_visible(False)

    fig.suptitle("Control Overhead Over Time", fontsize=14, fontweight="bold", y=1.01)
    fig.tight_layout()
    fig.savefig(outpath)
    plt.close(fig)
    print(f"  Saved: {outpath}")


def plot_precision_comparison_grid(
    ts_data, scenarios, outpath, protocols=None, failure_times=None
):
    """Multi-panel: topology precision over time."""
    if protocols is None:
        protocols = ["TAVRN", "OLSR", "OLSR-Tuned"]

    n = len(scenarios)
    ncols = min(3, n)
    nrows = (n + ncols - 1) // ncols
    fig, axes = plt.subplots(
        nrows, ncols, figsize=(5.5 * ncols, 4 * nrows), squeeze=False
    )

    for idx, scenario in enumerate(scenarios):
        row, col = divmod(idx, ncols)
        ax = axes[row][col]

        for proto in protocols:
            runs = ts_data.get(scenario, {}).get(proto, [])
            if not runs:
                continue
            avg = average_timeseries(runs)
            if avg is None:
                continue

            t = avg["t"]
            mean = avg["precision_mean"]
            valid = [i for i in range(len(mean)) if not np.isnan(mean[i])]
            if not valid:
                continue

            tv = [t[i] for i in valid]
            mv = [mean[i] for i in valid]
            color = PROTO_COLORS.get(proto, "gray")
            style = PROTO_STYLES.get(proto, "-")
            ax.plot(tv, mv, style, color=color, label=proto, linewidth=1.5)

        if failure_times and scenario in failure_times:
            ax.axvline(
                x=failure_times[scenario],
                color="red",
                linestyle="--",
                alpha=0.4,
                linewidth=1,
            )

        ax.set_title(scenario, fontsize=11, fontweight="bold")
        ax.set_xlabel("Time (s)", fontsize=9)
        ax.set_ylabel("Precision", fontsize=9)
        ax.set_ylim(0.5, 1.05)
        ax.tick_params(labelsize=8)
        if idx == 0:
            ax.legend(fontsize=8, loc="best")

    for idx in range(n, nrows * ncols):
        row, col = divmod(idx, ncols)
        axes[row][col].set_visible(False)

    fig.suptitle(
        "Topology Precision Over Time (higher = fewer stale entries)",
        fontsize=14,
        fontweight="bold",
        y=1.01,
    )
    fig.tight_layout()
    fig.savefig(outpath)
    plt.close(fig)
    print(f"  Saved: {outpath}")


def plot_summary_bars(
    df,
    metric_mean,
    metric_ci,
    ylabel,
    title,
    outpath,
    scenarios=None,
    protocols=None,
    log_scale=False,
):
    """Grouped bar chart of a summary metric across scenarios."""
    if protocols is None:
        protocols = [p for p in PROTO_ORDER if p in df["protocol"].unique()]
    if scenarios is None:
        scenarios = sorted(df["scenarioLabel"].unique())

    # Filter valid data
    df_filtered = df[
        (df["protocol"].isin(protocols))
        & (df["scenarioLabel"].isin(scenarios))
        & (df[metric_mean] != -1)
    ].copy()

    if df_filtered.empty:
        print(f"  SKIP (no data): {outpath}")
        return

    n_scenarios = len(scenarios)
    n_protos = len(protocols)
    x = np.arange(n_scenarios)
    width = 0.8 / n_protos

    fig, ax = plt.subplots(figsize=(max(10, n_scenarios * 1.2), 6))

    for i, proto in enumerate(protocols):
        pdf = df_filtered[df_filtered["protocol"] == proto]
        values = []
        errors = []
        for sc in scenarios:
            row = pdf[pdf["scenarioLabel"] == sc]
            if not row.empty:
                values.append(row[metric_mean].values[0])
                errors.append(
                    row[metric_ci].values[0] if metric_ci in row.columns else 0
                )
            else:
                values.append(0)
                errors.append(0)

        offset = (i - n_protos / 2 + 0.5) * width
        color = PROTO_COLORS.get(proto, "gray")
        ax.bar(
            x + offset,
            values,
            width,
            yerr=errors,
            label=proto,
            color=color,
            alpha=0.85,
            capsize=2,
        )

    ax.set_xlabel("Scenario")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.set_xticks(x)
    ax.set_xticklabels(scenarios, rotation=45, ha="right", fontsize=9)
    if log_scale:
        ax.set_yscale("log")
    ax.legend(loc="best")
    fig.tight_layout()
    fig.savefig(outpath)
    plt.close(fig)
    print(f"  Saved: {outpath}")


def plot_overhead_vs_pdr_scatter(df, outpath, protocols=None):
    """Scatter plot: overhead vs PDR for each protocol across scenarios."""
    if protocols is None:
        protocols = PROTO_ORDER

    fig, ax = plt.subplots(figsize=(10, 7))

    for proto in protocols:
        pdf = df[
            (df["protocol"] == proto)
            & (df["steadyOverheadBps_mean"] > 0)
            & (df["pdr_mean"] > 0)
        ]
        if pdf.empty:
            continue

        color = PROTO_COLORS.get(proto, "gray")
        ax.scatter(
            pdf["steadyOverheadBps_mean"],
            pdf["pdr_mean"],
            c=color,
            label=proto,
            s=60,
            alpha=0.8,
            edgecolors="white",
            linewidth=0.5,
        )

        # Label select points
        for _, row in pdf.iterrows():
            sc = row["scenarioLabel"]
            if sc in ("node-failure", "sparse", "large", "mobile-fast"):
                ax.annotate(
                    sc,
                    (row["steadyOverheadBps_mean"], row["pdr_mean"]),
                    fontsize=7,
                    alpha=0.7,
                    textcoords="offset points",
                    xytext=(5, 5),
                )

    ax.set_xlabel("Steady-State Overhead (B/s)")
    ax.set_ylabel("Packet Delivery Ratio")
    ax.set_xscale("log")
    ax.set_title("Overhead vs PDR Tradeoff (lower-right is better)")
    ax.legend(loc="lower left")
    fig.tight_layout()
    fig.savefig(outpath)
    plt.close(fig)
    print(f"  Saved: {outpath}")


def plot_convergence_comparison(df, outpath, protocols=None, scenarios=None):
    """Bar chart of first-packet delivery time."""
    if "firstDelivery_mean" not in df.columns:
        print(f"  SKIP (no firstDelivery data): {outpath}")
        return

    plot_summary_bars(
        df,
        "firstDelivery_mean",
        "firstDelivery_ci95",
        "First Packet Delivery (ms)",
        "Route Convergence: Time to First Packet Delivery",
        outpath,
        scenarios=scenarios,
        protocols=protocols,
        log_scale=True,
    )


def plot_stale_ratio_comparison(df, outpath, protocols=None, scenarios=None):
    """Bar chart of average stale ratio."""
    if "avgStaleRatio_mean" not in df.columns:
        print(f"  SKIP (no stale ratio data): {outpath}")
        return

    plot_summary_bars(
        df,
        "avgStaleRatio_mean",
        "avgStaleRatio_ci95",
        "Avg Stale Route Ratio",
        "Topology Staleness (lower = fewer outdated entries)",
        outpath,
        scenarios=scenarios,
        protocols=protocols,
    )


def plot_pdr_over_time_grid(
    ts_data, scenarios, outpath, protocols=None, failure_times=None
):
    """Multi-panel: PDR over time."""
    if protocols is None:
        protocols = ["TAVRN", "OLSR", "OLSR-Tuned"]

    n = len(scenarios)
    ncols = min(3, n)
    nrows = (n + ncols - 1) // ncols
    fig, axes = plt.subplots(
        nrows, ncols, figsize=(5.5 * ncols, 4 * nrows), squeeze=False
    )

    for idx, scenario in enumerate(scenarios):
        row, col = divmod(idx, ncols)
        ax = axes[row][col]

        for proto in protocols:
            runs = ts_data.get(scenario, {}).get(proto, [])
            if not runs:
                continue
            avg = average_timeseries(runs)
            if avg is None:
                continue

            t = avg["t"]
            mean = avg["pdr_mean"]
            valid = [i for i in range(len(mean)) if not np.isnan(mean[i])]
            if not valid:
                continue

            tv = [t[i] for i in valid]
            mv = [mean[i] for i in valid]
            color = PROTO_COLORS.get(proto, "gray")
            style = PROTO_STYLES.get(proto, "-")
            ax.plot(tv, mv, style, color=color, label=proto, linewidth=1.5)

        if failure_times and scenario in failure_times:
            ax.axvline(
                x=failure_times[scenario],
                color="red",
                linestyle="--",
                alpha=0.4,
                linewidth=1,
            )

        ax.set_title(scenario, fontsize=11, fontweight="bold")
        ax.set_xlabel("Time (s)", fontsize=9)
        ax.set_ylabel("PDR", fontsize=9)
        ax.set_ylim(-0.05, 1.1)
        ax.tick_params(labelsize=8)
        if idx == 0:
            ax.legend(fontsize=8, loc="best")

    for idx in range(n, nrows * ncols):
        row, col = divmod(idx, ncols)
        axes[row][col].set_visible(False)

    fig.suptitle(
        "Packet Delivery Ratio Over Time", fontsize=14, fontweight="bold", y=1.01
    )
    fig.tight_layout()
    fig.savefig(outpath)
    plt.close(fig)
    print(f"  Saved: {outpath}")


def plot_recall_comparison_grid(
    ts_data, scenarios, outpath, protocols=None, failure_times=None
):
    """Multi-panel: topology recall over time."""
    if protocols is None:
        protocols = ["TAVRN", "OLSR", "OLSR-Tuned"]

    n = len(scenarios)
    ncols = min(3, n)
    nrows = (n + ncols - 1) // ncols
    fig, axes = plt.subplots(
        nrows, ncols, figsize=(5.5 * ncols, 4 * nrows), squeeze=False
    )

    for idx, scenario in enumerate(scenarios):
        row, col = divmod(idx, ncols)
        ax = axes[row][col]

        for proto in protocols:
            runs = ts_data.get(scenario, {}).get(proto, [])
            if not runs:
                continue
            avg = average_timeseries(runs)
            if avg is None:
                continue

            t = avg["t"]
            mean = avg["recall_mean"]
            valid = [i for i in range(len(mean)) if not np.isnan(mean[i])]
            if not valid:
                continue

            tv = [t[i] for i in valid]
            mv = [mean[i] for i in valid]
            color = PROTO_COLORS.get(proto, "gray")
            style = PROTO_STYLES.get(proto, "-")
            ax.plot(tv, mv, style, color=color, label=proto, linewidth=1.5)

        if failure_times and scenario in failure_times:
            ax.axvline(
                x=failure_times[scenario],
                color="red",
                linestyle="--",
                alpha=0.4,
                linewidth=1,
            )

        ax.set_title(scenario, fontsize=11, fontweight="bold")
        ax.set_xlabel("Time (s)", fontsize=9)
        ax.set_ylabel("Recall", fontsize=9)
        ax.set_ylim(0.0, 1.1)
        ax.tick_params(labelsize=8)
        if idx == 0:
            ax.legend(fontsize=8, loc="best")

    for idx in range(n, nrows * ncols):
        row, col = divmod(idx, ncols)
        axes[row][col].set_visible(False)

    fig.suptitle(
        "Topology Recall Over Time (higher = knows more alive nodes)",
        fontsize=14,
        fontweight="bold",
        y=1.01,
    )
    fig.tight_layout()
    fig.savefig(outpath)
    plt.close(fig)
    print(f"  Saved: {outpath}")


# ─── Main ───────────────────────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(description="TAVRN Temporal Analysis Plotter")
    parser.add_argument("log_dir", help="Directory containing .log files with #TS data")
    parser.add_argument("--csv", help="Summary CSV file path", default=None)
    parser.add_argument(
        "--outdir", help="Output directory for figures", default="results/figures"
    )
    args = parser.parse_args()

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    print(f"Loading time-series from: {args.log_dir}")
    ts_data, run_data = parse_timeseries_from_logs(args.log_dir)
    print(f"  Found {len(ts_data)} scenarios with time-series data:")
    for sc in sorted(ts_data.keys()):
        protos = list(ts_data[sc].keys())
        runs_info = ", ".join(f"{p}({len(ts_data[sc][p])})" for p in protos)
        print(f"    {sc}: {runs_info}")

    # Load summary CSV if provided
    df = None
    if args.csv:
        print(f"Loading summary CSV: {args.csv}")
        df = load_summary_csv(args.csv)
        print(
            f"  {len(df)} rows, {df['scenarioLabel'].nunique()} scenarios, "
            f"{df['protocol'].nunique()} protocols"
        )

    # Known failure times
    failure_times = {
        "node-failure": 300,
        "node-rejoin": 300,
    }

    # ─── Scenario groups for multi-panel plots ───────────────────────────

    all_scenarios = sorted(ts_data.keys())

    # Static scenarios (no disruption)
    static_scenarios = [
        s
        for s in ["small", "medium", "large", "sensor-light", "sensor-dense", "linear"]
        if s in all_scenarios
    ]

    # Disruption scenarios
    disruption_scenarios = [
        s
        for s in [
            "node-failure",
            "node-rejoin",
            "mobile-churn",
            "mobile-fast",
            "mobile-slow",
        ]
        if s in all_scenarios
    ]

    # Home scenarios
    home_scenarios = [
        s
        for s in ["home-small", "home-medium", "home-large", "home-hell"]
        if s in all_scenarios
    ]

    # Key comparison scenarios (for the "why TAVRN" argument)
    key_scenarios = [
        s
        for s in [
            "medium",
            "node-failure",
            "mobile-fast",
            "sparse",
            "bursty",
            "home-medium",
        ]
        if s in all_scenarios
    ]

    focus_protos = ["TAVRN", "OLSR", "OLSR-Tuned"]

    # ─── Generate figures ────────────────────────────────────────────────

    print(f"\nGenerating figures in {outdir}/")

    # 1. Overhead over time — multi-panel grids
    if key_scenarios:
        plot_overhead_comparison_grid(
            ts_data,
            key_scenarios,
            outdir / "01_overhead_time_key.png",
            protocols=focus_protos,
            failure_times=failure_times,
        )

    if static_scenarios:
        plot_overhead_comparison_grid(
            ts_data,
            static_scenarios,
            outdir / "02_overhead_time_static.png",
            protocols=focus_protos,
        )

    if disruption_scenarios:
        plot_overhead_comparison_grid(
            ts_data,
            disruption_scenarios,
            outdir / "03_overhead_time_disruption.png",
            protocols=focus_protos,
            failure_times=failure_times,
        )

    if home_scenarios:
        plot_overhead_comparison_grid(
            ts_data,
            home_scenarios,
            outdir / "04_overhead_time_home.png",
            protocols=focus_protos,
        )

    # 2. Precision over time — disruption scenarios (this is the "stale" story)
    if disruption_scenarios:
        plot_precision_comparison_grid(
            ts_data,
            disruption_scenarios,
            outdir / "05_precision_time_disruption.png",
            protocols=focus_protos,
            failure_times=failure_times,
        )

    # 3. Recall over time — all scenarios
    if key_scenarios:
        plot_recall_comparison_grid(
            ts_data,
            key_scenarios,
            outdir / "06_recall_time_key.png",
            protocols=focus_protos,
            failure_times=failure_times,
        )

    # 4. PDR over time — disruption scenarios
    if disruption_scenarios:
        plot_pdr_over_time_grid(
            ts_data,
            disruption_scenarios,
            outdir / "07_pdr_time_disruption.png",
            protocols=focus_protos,
            failure_times=failure_times,
        )

    # 5. Overhead over time — ALL protocols for key scenarios
    if key_scenarios:
        plot_overhead_comparison_grid(
            ts_data,
            key_scenarios,
            outdir / "08_overhead_time_all_protos.png",
            protocols=PROTO_ORDER,
            failure_times=failure_times,
        )

    # ─── Summary bar charts (from CSV) ───────────────────────────────────

    if df is not None:
        print()

        # 6. Overhead bar chart
        plot_summary_bars(
            df,
            "steadyOverheadBps_mean",
            "steadyOverheadBps_ci95",
            "Steady-State Overhead (B/s)",
            "Control Overhead by Scenario",
            outdir / "09_overhead_bars.png",
            protocols=focus_protos,
            log_scale=True,
        )

        # 7. PDR bar chart
        plot_summary_bars(
            df,
            "pdr_mean",
            "pdr_ci95",
            "Packet Delivery Ratio",
            "PDR by Scenario",
            outdir / "10_pdr_bars.png",
            protocols=focus_protos,
        )

        # 8. Overhead vs PDR scatter
        plot_overhead_vs_pdr_scatter(
            df, outdir / "11_overhead_vs_pdr_scatter.png", protocols=focus_protos
        )

        # 9. Convergence (first delivery)
        plot_convergence_comparison(
            df, outdir / "12_convergence_bars.png", protocols=focus_protos
        )

        # 10. Stale ratio
        plot_stale_ratio_comparison(
            df,
            outdir / "13_stale_ratio_bars.png",
            protocols=focus_protos,
            scenarios=disruption_scenarios + home_scenarios
            if disruption_scenarios
            else None,
        )

        # 11. Bootstrap overhead bar chart
        plot_summary_bars(
            df,
            "bootstrapBytes_mean",
            "bootstrapBytes_ci95",
            "Bootstrap Overhead (bytes)",
            "Bootstrap Phase Overhead",
            outdir / "14_bootstrap_bars.png",
            protocols=focus_protos,
        )

        # 12. TX Energy bar chart
        plot_summary_bars(
            df,
            "txEnergy_mean",
            "txEnergy_ci95",
            "TX Energy (J)",
            "Transmit Energy Consumption",
            outdir / "15_tx_energy_bars.png",
            protocols=focus_protos,
        )

        # 13. All 6 protocols overhead comparison
        plot_summary_bars(
            df,
            "steadyOverheadBps_mean",
            "steadyOverheadBps_ci95",
            "Steady-State Overhead (B/s)",
            "Control Overhead — All 6 Protocols",
            outdir / "16_overhead_bars_all.png",
            protocols=PROTO_ORDER,
            log_scale=True,
        )

        # 14. All 6 protocols PDR
        plot_summary_bars(
            df,
            "pdr_mean",
            "pdr_ci95",
            "PDR",
            "Packet Delivery Ratio — All 6 Protocols",
            outdir / "17_pdr_bars_all.png",
            protocols=PROTO_ORDER,
        )

        # 15. Precision bar chart (time-weighted)
        if "twPrecision_mean" in df.columns:
            plot_summary_bars(
                df,
                "twPrecision_mean",
                "twPrecision_ci95",
                "Time-Weighted Precision",
                "Topology Precision (higher = fewer stale entries)",
                outdir / "18_tw_precision_bars.png",
                protocols=focus_protos,
            )

        # 16. Recall bar chart (time-weighted)
        if "twRecall_mean" in df.columns:
            plot_summary_bars(
                df,
                "twRecall_mean",
                "twRecall_ci95",
                "Time-Weighted Recall",
                "Topology Recall (higher = knows more alive nodes)",
                outdir / "19_tw_recall_bars.png",
                protocols=focus_protos,
            )

    # ─── Per-scenario individual time-series (full detail) ───────────────

    print(f"\nGenerating per-scenario detail plots...")
    detail_dir = outdir / "detail"
    detail_dir.mkdir(exist_ok=True)

    for scenario in all_scenarios:
        ft = failure_times.get(scenario)

        # Overhead
        plot_timeseries_metric(
            ts_data,
            scenario,
            "overhead_bps",
            "Overhead (B/s)",
            f"Overhead — {scenario}",
            detail_dir / f"{scenario}_overhead.png",
            protocols=focus_protos,
            failure_time=ft,
        )

        # Precision
        plot_timeseries_metric(
            ts_data,
            scenario,
            "precision",
            "Precision",
            f"Topology Precision — {scenario}",
            detail_dir / f"{scenario}_precision.png",
            protocols=focus_protos,
            failure_time=ft,
        )

        # Recall
        plot_timeseries_metric(
            ts_data,
            scenario,
            "recall",
            "Recall",
            f"Topology Recall — {scenario}",
            detail_dir / f"{scenario}_recall.png",
            protocols=focus_protos,
            failure_time=ft,
        )

        # PDR
        plot_timeseries_metric(
            ts_data,
            scenario,
            "pdr",
            "PDR",
            f"Packet Delivery Ratio — {scenario}",
            detail_dir / f"{scenario}_pdr.png",
            protocols=focus_protos,
            failure_time=ft,
        )

    print(f"\nDone! {len(list(outdir.rglob('*.png')))} figures generated.")


if __name__ == "__main__":
    main()
