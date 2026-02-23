#!/usr/bin/env python3
import argparse
import pandas as pd
import matplotlib.pyplot as plt


def main():
    p = argparse.ArgumentParser(description="Analyze NVML + GEMM throughput run")
    p.add_argument("--nvml", required=True, help="NVML CSV (data/..._nvml.csv)")
    p.add_argument("--gemm", required=True, help="GEMM CSV (data/..._gemm.csv)")
    p.add_argument("--out_prefix", default="data/h200_run1", help="Output prefix for plots/csv")
    args = p.parse_args()

    nvml = pd.read_csv(args.nvml)
    gemm = pd.read_csv(args.gemm)

    # Parse timestamps (second resolution; both are UTC ISO strings)
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

    # perf-per-watt proxy
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

    # Quick summary stats (useful for README snippets)
    # Define "load window" heuristically: where iters_per_sec is non-null and > 0
    load = merged[(merged["iters_per_sec"].notna()) & (merged["iters_per_sec"] > 0)]
    if len(load) > 0:
        print("\n=== LOAD WINDOW SUMMARY ===")
        print(f"Samples: {len(load)}")
        print(f"Power W: mean={load['power_w'].mean():.2f}  p95={load['power_w'].quantile(0.95):.2f}")
        print(f"Temp  C: mean={load['temp_c'].mean():.2f}  p95={load['temp_c'].quantile(0.95):.2f}")
        print(f"SM clk : mean={load['sm_clock_mhz'].mean():.1f}  std={load['sm_clock_mhz'].std():.1f}")
        print(f"Iters/s: mean={load['iters_per_sec'].mean():.3f}  std={load['iters_per_sec'].std():.3f}")
        print(f"Perf/W : mean={load['perf_per_watt'].mean():.6f}  std={load['perf_per_watt'].std():.6f}")
    else:
        print("\nNo load samples found (iters_per_sec missing). Did the GEMM run overlap with collector timestamps?")


if __name__ == "__main__":
    main()
