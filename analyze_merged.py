#!/usr/bin/env python3
import argparse
import json

import pandas as pd
import matplotlib.pyplot as plt


def print_summary(label: str, df: pd.DataFrame) -> None:
    print(f"\n=== {label} ===")
    print(f"Samples: {len(df)}")
    print(f"Power W: mean={df['power_w'].mean():.2f}  p95={df['power_w'].quantile(0.95):.2f}")
    print(f"Temp  C: mean={df['temp_c'].mean():.2f}  p95={df['temp_c'].quantile(0.95):.2f}")
    print(f"SM clk : mean={df['sm_clock_mhz'].mean():.1f}  std={df['sm_clock_mhz'].std():.1f}")
    if df["iters_per_sec"].notna().any():
        print(f"Iters/s: mean={df['iters_per_sec'].mean():.3f}  std={df['iters_per_sec'].std():.3f}")
    if df["perf_per_watt"].notna().any():
        print(f"Perf/W : mean={df['perf_per_watt'].mean():.6f}  std={df['perf_per_watt'].std():.6f}")


def main():
    p = argparse.ArgumentParser(description="Analyze an already-merged CSV (replay mode)")
    p.add_argument("--merged", required=True, help="Merged CSV (e.g., data/run1_merged.csv or data/replay/*.csv)")
    p.add_argument("--out_prefix", required=True, help="Output prefix for plots/report (e.g., data/replay/out_eff)")
    p.add_argument("--phases", default=None, help="Path to phases.json (idle_s/load_s/cooldown_s/steady_trim_s)")
    args = p.parse_args()

    merged = pd.read_csv(args.merged)

    # Parse timestamps
    if "ts_utc" not in merged.columns:
        raise SystemExit("Merged CSV must contain ts_utc")
    merged["ts"] = pd.to_datetime(merged["ts_utc"], utc=True, errors="coerce")
    merged = merged.dropna(subset=["ts"]).sort_values("ts")

    # Ensure perf_per_watt exists
    if "perf_per_watt" not in merged.columns:
        merged["perf_per_watt"] = merged["iters_per_sec"] / merged["power_w"]

    # Plot helper
    def plot_series(df, ycol, ylabel, fname):
        plt.figure()
        plt.plot(df["ts"], df[ycol])
        plt.xlabel("Time (UTC)")
        plt.ylabel(ylabel)
        plt.tight_layout()
        out = f"{args.out_prefix}_{fname}.png"
        plt.savefig(out, dpi=150)
        print(f"Wrote {out}")

    plot_series(merged, "power_w", "Power (W)", "power")
    plot_series(merged, "temp_c", "GPU Temp (C)", "temp")
    plot_series(merged, "sm_clock_mhz", "SM Clock (MHz)", "sm_clock")
    plot_series(merged, "iters_per_sec", "GEMM iters/sec", "iters_per_sec")
    plot_series(merged, "perf_per_watt", "iters/sec/W", "perf_per_watt")

    # ---------------------------
    # Phase slicing
    # ---------------------------
    if args.phases:
        with open(args.phases, "r", encoding="utf-8") as pf:
            ph = json.load(pf)

        idle_s = int(ph.get("idle_s", 0))
        load_s = int(ph.get("load_s", 0))
        cooldown_s = int(ph.get("cooldown_s", 0))
        steady_trim_s = int(ph.get("steady_trim_s", 0))

        t0 = merged["ts"].min()
        t_idle_end = t0 + pd.Timedelta(seconds=idle_s)
        t_load_end = t_idle_end + pd.Timedelta(seconds=load_s)
        t_cool_end = t_load_end + pd.Timedelta(seconds=cooldown_s)

        load = merged[(merged["ts"] >= t_idle_end) & (merged["ts"] < t_load_end)].copy()
        load_ss_start = t_idle_end + pd.Timedelta(seconds=steady_trim_s)
        load_ss = load[load["ts"] >= load_ss_start].copy()

        phase_mode = "phases.json"
    else:
        load = merged[(merged["iters_per_sec"].notna()) & (merged["iters_per_sec"] > 0)].copy()
        load_ss = load.iloc[30:].copy() if len(load) > 40 else load.copy()
        phase_mode = "heuristic"

    if len(load) == 0:
        print("No load samples found.")
        return

    print(f"\nPhase selection mode: {phase_mode}")
    print_summary("LOAD WINDOW SUMMARY (full)", load)
    print_summary("LOAD WINDOW SUMMARY (steady-state)", load_ss)

    # ---------------------------
    # Health scoring (v0, same as analyze.py)
    # ---------------------------
    TEMP_P95_WARN = 80.0
    TEMP_P95_BAD = 90.0
    CLK_STD_WARN = 120.0
    PERF_W_COV_WARN = 0.20
    PWR_HIGH_RATIO = 0.98
    PWR_PENALTY_MAX = 3.0

    score = 100.0
    reasons = []

    if len(load_ss) == 0:
        load_ss = load.copy()

    # Temperature penalty
    temp_p95 = float(load_ss["temp_c"].quantile(0.95))
    if temp_p95 > TEMP_P95_WARN:
        penalty = 25.0 if temp_p95 >= TEMP_P95_BAD else 10.0
        score -= penalty
        reasons.append(f"Thermal: p95 temp {temp_p95:.1f}C (penalty {penalty:.0f})")

    # Clock stability penalty
    clk_std = float(load_ss["sm_clock_mhz"].std())
    if clk_std > CLK_STD_WARN:
        penalty = min(15.0, (clk_std - CLK_STD_WARN) / 20.0)
        score -= penalty
        reasons.append(f"Clocks: SM clock std {clk_std:.1f}MHz (penalty {penalty:.1f})")

    # Efficiency stability penalty
    perf_w = load_ss["perf_per_watt"].dropna()
    cov = None
    if len(perf_w) > 20 and perf_w.mean() > 0:
        cov = float(perf_w.std() / perf_w.mean())
        if cov > PERF_W_COV_WARN:
            penalty = min(10.0, (cov - PERF_W_COV_WARN) * 25.0)
            score -= penalty
            reasons.append(f"Efficiency: perf/W CoV {cov:.3f} (penalty {penalty:.1f})")

    # Power headroom penalty
    pwr_mean = float(load_ss["power_w"].mean())
    pwr_limit = None
    if "power_limit_w" in load_ss.columns and load_ss["power_limit_w"].notna().any():
        pwr_limit = float(load_ss["power_limit_w"].dropna().iloc[0])

    pct_high = None
    if pwr_limit:
        ratio_series = (load_ss["power_w"] / pwr_limit).dropna()
        if len(ratio_series) > 0:
            pct_high = float((ratio_series >= PWR_HIGH_RATIO).mean())
            penalty = PWR_PENALTY_MAX * pct_high
            score -= penalty
            reasons.append(
                f"Power headroom: {pct_high*100:.1f}% samples ≥ {PWR_HIGH_RATIO*100:.1f}% of limit "
                f"({pwr_mean:.1f}W mean, limit {pwr_limit:.1f}W) (penalty {penalty:.1f})"
            )

    score = max(0.0, min(100.0, score))

    if score >= 85:
        cls = "Healthy"
    elif score >= 70:
        cls = "Monitor"
    elif score >= 50:
        cls = "Degrading"
    else:
        cls = "Decommission Candidate"

    print("\n=== HEALTH SCORE (v0) ===")
    print(f"Health Score: {score:.1f} / 100  ->  {cls}")
    if reasons:
        print("Notes:")
        for r in reasons:
            print(f" - {r}")
    else:
        print("Notes: No penalties triggered (within envelope thresholds).")

    report_path = f"{args.out_prefix}_report.md"
    with open(report_path, "w", encoding="utf-8") as rf:
        rf.write("# GPU Health Index Report (v0) — Replay\n\n")
        rf.write(f"- Phase selection mode: **{phase_mode}**\n")
        rf.write(f"- Classification: **{cls}**\n")
        rf.write(f"- Health Score: **{score:.1f} / 100**\n\n")
        rf.write("## Load Window Summary (steady-state)\n\n")
        rf.write(f"- Samples: {len(load_ss)}\n")
        rf.write(f"- Power mean: {pwr_mean:.2f} W\n")
        if pwr_limit is not None:
            rf.write(f"- Power limit: {pwr_limit:.2f} W\n")
        rf.write(f"- Temp p95: {temp_p95:.2f} C\n")
        rf.write(f"- SM clock std: {clk_std:.2f} MHz\n")
        if len(perf_w) > 0:
            rf.write(f"- Perf/W mean: {perf_w.mean():.6f}\n")
        if cov is not None:
            rf.write(f"- Perf/W CoV: {cov:.6f}\n")
        rf.write("\n## Notes\n\n")
        for r in reasons:
            rf.write(f"- {r}\n")

    print(f"Wrote {report_path}")


if __name__ == "__main__":
    main()
