#!/usr/bin/env python3
import argparse
import os
import sys
import numpy as np
import pandas as pd


def linear_ramp(n: int, start: float, end: float) -> np.ndarray:
    if n <= 1:
        return np.array([end], dtype=float)
    return np.linspace(start, end, n, dtype=float)


def clamp(series: pd.Series, lo=None, hi=None) -> pd.Series:
    out = series.copy()
    if lo is not None:
        out = out.clip(lower=lo)
    if hi is not None:
        out = out.clip(upper=hi)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate replay degradation datasets from a merged run CSV.")
    ap.add_argument("--in", dest="inp", required=True, help="Input merged CSV (e.g., data/run1_merged.csv)")
    ap.add_argument("--out_dir", default="data/replay", help="Output directory (default: data/replay)")
    args = ap.parse_args()

    print(f"[make_replay] input:   {args.inp}")
    print(f"[make_replay] out_dir: {args.out_dir}")

    os.makedirs(args.out_dir, exist_ok=True)

    df = pd.read_csv(args.inp)
    print(f"[make_replay] rows: {len(df)} cols: {len(df.columns)}")

    required = {"ts_utc", "power_w", "temp_c", "sm_clock_mhz", "iters_per_sec", "power_limit_w"}
    missing = sorted([c for c in required if c not in df.columns])
    if missing:
        print(f"[make_replay] ERROR: missing required columns: {missing}", file=sys.stderr)
        print(f"[make_replay] Columns present: {list(df.columns)}", file=sys.stderr)
        return 2

    load_mask = df["iters_per_sec"].notna() & (df["iters_per_sec"] > 0)
    load_idx = np.where(load_mask.values)[0]
    print(f"[make_replay] load samples: {len(load_idx)}")

    if len(load_idx) < 50:
        print("[make_replay] ERROR: not enough load samples found in input.", file=sys.stderr)
        return 3

    i0, i1 = int(load_idx[0]), int(load_idx[-1])
    load_len = i1 - i0 + 1
    print(f"[make_replay] load index range: {i0}..{i1} (len={load_len})")

    def write_variant(name: str, out_df: pd.DataFrame):
        out_path = os.path.join(args.out_dir, f"{name}.csv")
        out_df.to_csv(out_path, index=False)
        print(f"[make_replay] wrote: {out_path}")

    # Variant A: efficiency drop 10% across load window
    df_eff = df.copy()
    ramp = linear_ramp(load_len, 1.00, 0.90)
    df_eff.loc[i0:i1, "iters_per_sec"] = df_eff.loc[i0:i1, "iters_per_sec"].values * ramp
    if "perf_per_watt" in df_eff.columns:
        df_eff["perf_per_watt"] = df_eff["iters_per_sec"] / df_eff["power_w"]
    write_variant("degrading_efficiency_10pct", df_eff)

    # Variant B: clock droop 8% + throughput droop 6%
    df_clk = df.copy()
    df_clk["sm_clock_mhz"] = df_clk["sm_clock_mhz"].astype(float)
    df_clk["iters_per_sec"] = df_clk["iters_per_sec"].astype(float)
    clk_ramp = linear_ramp(load_len, 1.00, 0.92)
    it_ramp = linear_ramp(load_len, 1.00, 0.94)
    df_clk.loc[i0:i1, "sm_clock_mhz"] = df_clk.loc[i0:i1, "sm_clock_mhz"].values * clk_ramp
    df_clk.loc[i0:i1, "iters_per_sec"] = df_clk.loc[i0:i1, "iters_per_sec"].values * it_ramp
    if "perf_per_watt" in df_clk.columns:
        df_clk["perf_per_watt"] = df_clk["iters_per_sec"] / df_clk["power_w"]
    write_variant("degrading_clocks_8pct", df_clk)

    # Variant C: thermal rise +10C + throughput droop 3%
    df_th = df.copy()
    df_th["temp_c"] = df_th["temp_c"].astype(float)
    df_th["iters_per_sec"] = df_th["iters_per_sec"].astype(float)
    t_ramp = linear_ramp(load_len, 0.0, 10.0)
    df_th.loc[i0:i1, "temp_c"] = df_th.loc[i0:i1, "temp_c"].values + t_ramp
    df_th["temp_c"] = clamp(df_th["temp_c"], lo=0.0, hi=110.0)
    it_ramp2 = linear_ramp(load_len, 1.00, 0.97)
    df_th.loc[i0:i1, "iters_per_sec"] = df_th.loc[i0:i1, "iters_per_sec"].values * it_ramp2
    if "perf_per_watt" in df_th.columns:
        df_th["perf_per_watt"] = df_th["iters_per_sec"] / df_th["power_w"]
    write_variant("degrading_thermal_plus10C", df_th)

    # Variant D: combined strong degradation
    df_combo = df.copy()
    df_combo["temp_c"] = df_combo["temp_c"].astype(float)
    df_combo["sm_clock_mhz"] = df_combo["sm_clock_mhz"].astype(float)
    df_combo["iters_per_sec"] = df_combo["iters_per_sec"].astype(float)
    df_combo.loc[i0:i1, "iters_per_sec"] = df_combo.loc[i0:i1, "iters_per_sec"].values * linear_ramp(load_len, 1.00, 0.80)
    df_combo.loc[i0:i1, "temp_c"] = clamp(df_combo.loc[i0:i1, "temp_c"] + linear_ramp(load_len, 0.0, 15.0), lo=0.0, hi=110.0)
    df_combo.loc[i0:i1, "sm_clock_mhz"] = df_combo.loc[i0:i1, "sm_clock_mhz"].values * linear_ramp(load_len, 1.00, 0.90)
    if "perf_per_watt" in df_combo.columns:
        df_combo["perf_per_watt"] = df_combo["iters_per_sec"] / df_combo["power_w"]
    write_variant("degrading_combined_strong", df_combo)

    print("[make_replay] done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
