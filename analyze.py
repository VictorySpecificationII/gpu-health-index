#!/usr/bin/env python3
import argparse
import pandas as pd
import matplotlib.pyplot as plt
import json


def main():
    p = argparse.ArgumentParser(description="Analyze NVML + GEMM throughput run")
    p.add_argument("--nvml", required=True, help="NVML CSV (data/..._nvml.csv)")
    p.add_argument("--gemm", required=True, help="GEMM CSV (data/..._gemm.csv)")
    p.add_argument("--out_prefix", default="data/h200_run1", help="Output prefix for plots/csv")
    p.add_argument("--phases", default=None, help="Path to phases.json (idle_s/load_s/cooldown_s)")
    args = p.parse_args()

    nvml = pd.read_csv(args.nvml)
    gemm = pd.read_csv(args.gemm)

    # Parse timestamps (both are UTC ISO strings)
    nvml["ts"] = pd.to_datetime(nvml["ts_utc"], utc=True, errors="coerce")
    gemm["ts"] = pd.to_datetime(gemm["ts_utc"], utc=True, errors="coerce")

    nvml = nvml.dropna(subset=["ts"]).sort_values("ts")
    gemm = gemm.dropna(subset=["ts"]).sort_values("ts")

    # Merge nearest within 1 second
    merged = pd.merge_asof(
        nvml,
        gemm[["ts", "iters_per_sec"]],
        on="ts",
        direction="nearest",
        tolerance=pd.Timedelta(seconds=1),
    )

    # perf-per-watt proxy (iters/sec/W)
    merged["perf_per_watt"] = merged["iters_per_sec"] / merged["power_w"]

    out_csv = f"{args.out_prefix}_merged.csv"
    merged.to_csv(out_csv, index=False)
    print(f"Wrote {out_csv}")

    # Plot helper
    def plot_series(ycol, ylabel, fname):
        plt.figure()
        plt.plot(merged["ts"], merged[ycol])
        plt.xlabel("Time (UTC)")
        plt.ylabel(ylabel)
        plt.tight_layout()
        out = f"{args.out_prefix}_{fname}.png"
        plt.savefig(out, dpi=150)
        print(f"Wrote {out}")

    plot_series("power_w", "Power (W)", "power")
    plot_series("temp_c", "GPU Temp (C)", "temp")
    plot_series("sm_clock_mhz", "SM Clock (MHz)", "sm_clock")
    plot_series("iters_per_sec", "GEMM iters/sec", "iters_per_sec")
    plot_series("perf_per_watt", "iters/sec/W", "perf_per_watt")

    # Load window: where iters_per_sec is present and > 0
    load = merged[(merged["iters_per_sec"].notna()) & (merged["iters_per_sec"] > 0)]

    if len(load) == 0:
        print("\nNo load samples found (iters_per_sec missing). Did the GEMM run overlap with collector timestamps?")
        return

    print("\n=== LOAD WINDOW SUMMARY ===")
    print(f"Samples: {len(load)}")
    print(f"Power W: mean={load['power_w'].mean():.2f}  p95={load['power_w'].quantile(0.95):.2f}")
    print(f"Temp  C: mean={load['temp_c'].mean():.2f}  p95={load['temp_c'].quantile(0.95):.2f}")
    print(f"SM clk : mean={load['sm_clock_mhz'].mean():.1f}  std={load['sm_clock_mhz'].std():.1f}")
    print(f"Iters/s: mean={load['iters_per_sec'].mean():.3f}  std={load['iters_per_sec'].std():.3f}")
    print(f"Perf/W : mean={load['perf_per_watt'].mean():.6f}  std={load['perf_per_watt'].std():.6f}")

    # --- Health scoring (v0, envelope-based) ---
    # Use steady-state load window: drop first 30 samples to reduce ramp/boost noise
    load_ss = load.iloc[30:].copy() if len(load) > 40 else load.copy()

    # Tunable thresholds (explicit & explainable)
    TEMP_P95_WARN = 80.0   # C
    TEMP_P95_BAD = 90.0    # C

    CLK_STD_WARN = 120.0   # MHz (conservative)
    PERF_W_COV_WARN = 0.20 # Coefficient of variation (std/mean)

    score = 100.0
    reasons = []

    # Temperature penalty based on steady-state p95
    temp_p95 = float(load_ss["temp_c"].quantile(0.95))
    if temp_p95 > TEMP_P95_WARN:
        penalty = 25.0 if temp_p95 >= TEMP_P95_BAD else 10.0
        score -= penalty
        reasons.append(f"Thermal: p95 temp {temp_p95:.1f}C (penalty {penalty:.0f})")

    # Clock stability penalty based on SM clock stddev
    clk_std = float(load_ss["sm_clock_mhz"].std())
    if clk_std > CLK_STD_WARN:
        penalty = min(15.0, (clk_std - CLK_STD_WARN) / 20.0)  # capped
        score -= penalty
        reasons.append(f"Clocks: SM clock std {clk_std:.1f}MHz (penalty {penalty:.1f})")

    # Efficiency stability penalty based on perf/W coefficient of variation
    perf_w = load_ss["perf_per_watt"].dropna()
    cov = None
    if len(perf_w) > 20 and perf_w.mean() > 0:
        cov = float(perf_w.std() / perf_w.mean())
        if cov > PERF_W_COV_WARN:
            penalty = min(10.0, (cov - PERF_W_COV_WARN) * 25.0)
            score -= penalty
            reasons.append(f"Efficiency: perf/W CoV {cov:.3f} (penalty {penalty:.1f})")

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

    # Write a small report snippet for reuse
    report_path = f"{args.out_prefix}_report.md"
    pwr_mean = float(load_ss["power_w"].mean())
    pwr_limit = (
        float(load_ss["power_limit_w"].dropna().iloc[0])
        if "power_limit_w" in load_ss.columns and load_ss["power_limit_w"].notna().any()
        else None
    )

    with open(report_path, "w", encoding="utf-8") as rf:
        rf.write("# GPU Health Index Report (v0)\n\n")
        rf.write(f"- Classification: **{cls}**\n")
        rf.write(f"- Health Score: **{score:.1f} / 100**\n\n")

        rf.write("## Load Window Summary (steady-state)\n\n")
        rf.write(f"- Samples: {len(load_ss)}\n")
        rf.write(f"- Power mean: {pwr_mean:.2f} W\n")
        if pwr_limit is not None:
            rf.write(f"- Power limit: {pwr_limit:.2f} W\n")
            rf.write(f"- Power ratio: {(pwr_mean / pwr_limit) * 100:.2f}%\n")
        rf.write(f"- Temp p95: {temp_p95:.2f} C\n")
        rf.write(f"- SM clock std: {clk_std:.2f} MHz\n")
        if len(perf_w) > 0:
            rf.write(f"- Perf/W mean: {perf_w.mean():.6f}\n")
        if cov is not None:
            rf.write(f"- Perf/W CoV: {cov:.6f}\n")

        rf.write("\n## Notes\n\n")
        if reasons:
            for r in reasons:
                rf.write(f"- {r}\n")
        else:
            rf.write("- No penalties triggered (within envelope thresholds).\n")

    print(f"Wrote {report_path}")


if __name__ == "__main__":
    main()
