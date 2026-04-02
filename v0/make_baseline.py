#!/usr/bin/env python3
import argparse
import json
import pandas as pd


def load_steady_perf_w(merged_csv: str, phases_json: str) -> float:
    df = pd.read_csv(merged_csv)
    df["ts"] = pd.to_datetime(df["ts_utc"], utc=True, errors="coerce")
    df = df.dropna(subset=["ts"]).sort_values("ts")

    if "perf_per_watt" not in df.columns:
        df["perf_per_watt"] = df["iters_per_sec"] / df["power_w"]

    with open(phases_json, "r", encoding="utf-8") as f:
        ph = json.load(f)

    idle_s = int(ph.get("idle_s", 0))
    load_s = int(ph.get("load_s", 0))
    steady_trim_s = int(ph.get("steady_trim_s", 0))

    t0 = df["ts"].min()
    t_idle_end = t0 + pd.Timedelta(seconds=idle_s)
    t_load_end = t_idle_end + pd.Timedelta(seconds=load_s)
    load = df[(df["ts"] >= t_idle_end) & (df["ts"] < t_load_end)].copy()
    load_ss_start = t_idle_end + pd.Timedelta(seconds=steady_trim_s)
    load_ss = load[load["ts"] >= load_ss_start].copy()

    return float(load_ss["perf_per_watt"].dropna().mean())


def main():
    ap = argparse.ArgumentParser(description="Create a baseline.json from one or more merged CSV runs.")
    ap.add_argument("--merged", nargs="+", required=True, help="Merged CSV(s), e.g. data/run1_merged.csv data/run2_merged.csv")
    ap.add_argument("--phases", required=True, help="Phases JSON, e.g. data/run1_phases.json")
    ap.add_argument("--out", default="data/baseline.json", help="Output baseline json path")
    args = ap.parse_args()

    vals = [load_steady_perf_w(p, args.phases) for p in args.merged]
    baseline = {
        "perf_per_watt_mean": float(sum(vals) / len(vals)),
        "perf_per_watt_per_run": vals,
        "source_runs": args.merged,
        "phases": args.phases,
    }

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(baseline, f, indent=2)

    print(f"Wrote {args.out}")
    print(f"perf_per_watt_mean={baseline['perf_per_watt_mean']:.6f}")
    print("per_run:", ", ".join(f"{v:.6f}" for v in vals))


if __name__ == "__main__":
    main()
