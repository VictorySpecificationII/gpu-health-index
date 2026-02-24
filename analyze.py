#!/usr/bin/env python3
import argparse
import json

import matplotlib.pyplot as plt
import pandas as pd


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
    p = argparse.ArgumentParser(description="Analyze NVML + GEMM throughput run")
    p.add_argument("--nvml", required=True, help="NVML CSV (data/..._nvml.csv)")
    p.add_argument("--gemm", required=True, help="GEMM CSV (data/..._gemm.csv)")
    p.add_argument("--out_prefix", default="data/run", help="Output prefix for plots/csv")
    p.add_argument("--phases", default=None, help="Path to phases.json (idle_s/load_s/cooldown_s/steady_trim_s)")
    p.add_argument("--baseline", default=None, help="Path to baseline.json (adds perf/W drift penalty)")
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
    # Phase slicing (deterministic if phases.json is provided)
    # ---------------------------
    idle_s = None
    load_s = None
    cooldown_s = None
    steady_trim_s = None

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

        idle = merged[(merged["ts"] >= t0) & (merged["ts"] < t_idle_end)].copy()
        load = merged[(merged["ts"] >= t_idle_end) & (merged["ts"] < t_load_end)].copy()
        cooldown = merged[(merged["ts"] >= t_load_end) & (merged["ts"] < t_cool_end)].copy()

        load_ss_start = t_idle_end + pd.Timedelta(seconds=steady_trim_s)
        load_ss = load[load["ts"] >= load_ss_start].copy()

        phase_mode = "phases.json"
        _ = idle
        _ = cooldown
    else:
        # Heuristic fallback: load where iters_per_sec exists and > 0
        load = merged[(merged["iters_per_sec"].notna()) & (merged["iters_per_sec"] > 0)].copy()
        load_ss = load.iloc[30:].copy() if len(load) > 40 else load.copy()
        phase_mode = "heuristic"

    if len(load) == 0:
        print("\nNo load samples found. Did the GEMM run overlap with collector timestamps?")
        return

    print(f"\nPhase selection mode: {phase_mode}")

    # Print both full and steady-state summaries to avoid confusion
    print_summary("LOAD WINDOW SUMMARY (full)", load)
    print_summary("LOAD WINDOW SUMMARY (steady-state)", load_ss)

    # ---------------------------
    # Health scoring (v0 + baseline-relative drift)
    # ---------------------------
    # Tunable thresholds
    TEMP_P95_WARN = 80.0    # C
    TEMP_P95_BAD = 90.0     # C

    CLK_STD_WARN = 120.0    # MHz (conservative)
    PERF_W_COV_WARN = 0.20  # coefficient of variation (std/mean)

    # Power headroom policy (penalize sustained saturation slightly)
    PWR_HIGH_RATIO = 0.98   # "near limit"
    PWR_PENALTY_MAX = 3.0   # max points deducted for sustained saturation

    # Baseline-relative degradation thresholds (perf/W drop)
    PERF_DROP_WARN = 0.03   # 3%
    PERF_DROP_BAD = 0.07    # 7%
    PERF_DROP_SEVERE = 0.12 # 12%
    PERF_DROP_PEN_WARN = 5.0
    PERF_DROP_PEN_BAD = 15.0
    PERF_DROP_PEN_SEVERE = 30.0

    # New: "effective steady-load" gating
    STEADY_PWR_RATIO = 0.90      # require >=90% of power limit to consider samples comparable
    MIN_STEADY_SAMPLES = 60      # require enough samples to compare to baseline / compute stable stats
    MIN_STEADY_FRACTION = 0.50   # if less than 50% of steady-state samples are steady-power, treat as incomplete

    score = 100.0
    reasons = []

    # Use steady-state load window for scoring
    if len(load_ss) == 0:
        load_ss = load.copy()

    # Power limit (if present)
    pwr_limit = None
    if "power_limit_w" in load_ss.columns and load_ss["power_limit_w"].notna().any():
        pwr_limit = float(load_ss["power_limit_w"].dropna().iloc[0])

    # Build effective steady-load slice (only if we know power limit)
    if pwr_limit is not None and "power_w" in load_ss.columns:
        load_eff = load_ss[load_ss["power_w"] >= (STEADY_PWR_RATIO * pwr_limit)].copy()
        eff_mode = f"power>={STEADY_PWR_RATIO:.2f}*limit"
    else:
        load_eff = load_ss.copy()
        eff_mode = "no_power_limit"

    # Guard: avoid scoring nonsense when steady-state is mostly not at steady load
    if len(load_ss) > 0:
        eff_frac = len(load_eff) / max(1, len(load_ss))
        if eff_frac < MIN_STEADY_FRACTION:
            print("\n=== HEALTH SCORE (v0) ===")
            print("Health Score: N/A  ->  Incomplete Telemetry")
            print(f"Notes: insufficient steady-load samples ({len(load_eff)} of {len(load_ss)} steady-state; mode={eff_mode}).")
            return

    # --- Telemetry completeness guard (avoid false "Healthy" on partial phase coverage) ---
    if args.phases and len(load) > 0:
        # steady-state should be most of load (e.g. 270/300 in your long runs)
        if len(load_ss) < int(0.7 * len(load)):
            print("\n=== HEALTH SCORE (v0) ===")
            print("Health Score: N/A  ->  Incomplete Telemetry")
            print(f"Notes: steady-state samples too low ({len(load_ss)} of {len(load)} load samples).")
            return

    # Temperature penalty (use effective load)
    temp_p95 = float(load_eff["temp_c"].quantile(0.95))
    if temp_p95 > TEMP_P95_WARN:
        penalty = 25.0 if temp_p95 >= TEMP_P95_BAD else 10.0
        score -= penalty
        reasons.append(f"Thermal: p95 temp {temp_p95:.1f}C (penalty {penalty:.0f})")

    # Clock stability penalty (use effective load)
    clk_std = float(load_eff["sm_clock_mhz"].std())
    if clk_std > CLK_STD_WARN:
        penalty = min(15.0, (clk_std - CLK_STD_WARN) / 20.0)  # capped
        score -= penalty
        reasons.append(f"Clocks: SM clock std {clk_std:.1f}MHz (penalty {penalty:.1f})")

    # Perf/W stability penalty (use effective load)
    perf_w = load_eff["perf_per_watt"].dropna()
    perf_w_mean = float(perf_w.mean()) if len(perf_w) > 0 else None
    cov = None
    if len(perf_w) > 20 and perf_w.mean() > 0:
        cov = float(perf_w.std() / perf_w.mean())
        if cov > PERF_W_COV_WARN:
            penalty = min(10.0, (cov - PERF_W_COV_WARN) * 25.0)
            score -= penalty
            reasons.append(f"Efficiency: perf/W CoV {cov:.3f} (penalty {penalty:.1f})")

    # Power saturation penalty (fraction-of-time near power limit) — use effective load window
    pwr_mean = float(load_eff["power_w"].mean())
    pct_high = None
    if pwr_limit is not None and "power_w" in load_eff.columns:
        ratio_series = (load_eff["power_w"] / pwr_limit).dropna()
        if len(ratio_series) > 0:
            pct_high = float((ratio_series >= PWR_HIGH_RATIO).mean())
            penalty = PWR_PENALTY_MAX * pct_high
            score -= penalty
            reasons.append(
                f"Power headroom: {pct_high*100:.1f}% samples ≥ {PWR_HIGH_RATIO*100:.1f}% of limit "
                f"({pwr_mean:.1f}W mean, limit {pwr_limit:.1f}W) (penalty {penalty:.1f})"
            )

    # Baseline-relative perf/W degradation penalty
    baseline_perf = None
    drop = None
    if args.baseline and perf_w_mean is not None:
        # Only compare to baseline if we have enough steady-load samples
        if len(load_eff) < MIN_STEADY_SAMPLES:
            reasons.append(
                f"Baseline: skipped (only {len(load_eff)} steady-load samples; need ≥ {MIN_STEADY_SAMPLES})."
            )
        else:
            with open(args.baseline, "r", encoding="utf-8") as bf:
                b = json.load(bf)
            baseline_perf = float(b.get("perf_per_watt_mean"))
            if baseline_perf and baseline_perf > 0:
                drop = 1.0 - (perf_w_mean / baseline_perf)

                if drop > PERF_DROP_SEVERE:
                    penalty = PERF_DROP_PEN_SEVERE
                elif drop > PERF_DROP_BAD:
                    penalty = PERF_DROP_PEN_BAD
                elif drop > PERF_DROP_WARN:
                    penalty = PERF_DROP_PEN_WARN
                else:
                    penalty = 0.0

                if penalty > 0:
                    score -= penalty
                    reasons.append(
                        f"Degradation: perf/W mean {perf_w_mean:.6f} vs baseline {baseline_perf:.6f} "
                        f"({drop*100:.1f}% drop) (penalty {penalty:.0f})"
                    )
                else:
                    reasons.append(
                        f"Baseline: perf/W mean {perf_w_mean:.6f} vs baseline {baseline_perf:.6f} ({drop*100:.1f}% drop)"
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

    # Write report snippet
    report_path = f"{args.out_prefix}_report.md"
    with open(report_path, "w", encoding="utf-8") as rf:
        rf.write("# GPU Health Index Report (v0)\n\n")
        rf.write(f"- Phase selection mode: **{phase_mode}**\n")
        rf.write(f"- Effective load filter: **{eff_mode}**\n")
        if args.baseline:
            rf.write(f"- Baseline: **{args.baseline}**\n")
        rf.write(f"- Classification: **{cls}**\n")
        rf.write(f"- Health Score: **{score:.1f} / 100**\n\n")

        rf.write("## Load Window Summary (steady-state)\n\n")
        rf.write(f"- Samples (steady-state): {len(load_ss)}\n")
        rf.write(f"- Samples (effective load): {len(load_eff)}\n")
        rf.write(f"- Power mean: {pwr_mean:.2f} W\n")
        if pwr_limit is not None:
            rf.write(f"- Power limit: {pwr_limit:.2f} W\n")
            rf.write(f"- Power ratio (mean): {(pwr_mean / pwr_limit) * 100:.2f}%\n")
        if pct_high is not None:
            rf.write(f"- Power saturation: {pct_high*100:.2f}% samples ≥ {PWR_HIGH_RATIO*100:.1f}% of limit\n")
        rf.write(f"- Temp p95: {temp_p95:.2f} C\n")
        rf.write(f"- SM clock std: {clk_std:.2f} MHz\n")
        if perf_w_mean is not None:
            rf.write(f"- Perf/W mean: {perf_w_mean:.6f}\n")
        if cov is not None:
            rf.write(f"- Perf/W CoV: {cov:.6f}\n")
        if baseline_perf is not None and drop is not None:
            rf.write(f"- Baseline perf/W: {baseline_perf:.6f}\n")
            rf.write(f"- Perf/W drop vs baseline: {drop*100:.2f}%\n")

        rf.write("\n## Notes\n\n")
        if reasons:
            for r in reasons:
                rf.write(f"- {r}\n")
        else:
            rf.write("- No penalties triggered (within envelope thresholds).\n")

    print(f"Wrote {report_path}")


if __name__ == "__main__":
    main()
