#!/usr/bin/env python3
import argparse
import csv
import datetime as dt
import json
import sys
import urllib.parse
import urllib.request
from typing import Dict, List, Tuple, Optional


def iso_utc(ts: float) -> str:
    return dt.datetime.fromtimestamp(ts, tz=dt.timezone.utc).isoformat().replace("+00:00", "Z")


def http_get_json(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=30) as resp:
        body = resp.read()
    return json.loads(body.decode("utf-8"))


def prom_query_range(
    prom_url: str, query: str, start: float, end: float, step: float
) -> List[Tuple[float, float]]:
    """
    Returns list of (timestamp, value) from Prom query_range.
    If multiple series match, we error (keep deterministic for v0).
    """
    params = {
        "query": query,
        "start": f"{start:.3f}",
        "end": f"{end:.3f}",
        "step": f"{step:.3f}",
    }
    url = prom_url.rstrip("/") + "/api/v1/query_range?" + urllib.parse.urlencode(params)
    data = http_get_json(url)

    if data.get("status") != "success":
        raise RuntimeError(f"Prometheus query failed: {data}")

    result = data["data"]["result"]
    if len(result) == 0:
        return []
    if len(result) > 1:
        # You can later aggregate by gpu uuid/index, but for v0 keep it strict.
        raise RuntimeError(f"Query matched multiple series (ambiguous): {query}")

    values = result[0]["values"]  # [[ts, "val"], ...]
    out: List[Tuple[float, float]] = []
    for ts_s, v_s in values:
        try:
            out.append((float(ts_s), float(v_s)))
        except ValueError:
            # Prom uses "NaN" sometimes
            continue
    return out


def to_series_map(pairs: List[Tuple[float, float]]) -> Dict[float, float]:
    return {ts: v for ts, v in pairs}


def main() -> int:
    ap = argparse.ArgumentParser(description="Export DCGM telemetry from Prometheus into nvml-like CSV.")
    ap.add_argument("--prom", default="http://127.0.0.1:9090", help="Prometheus base URL")
    ap.add_argument("--gpu", type=int, default=0, help="GPU index (exporter label gpu=)")
    ap.add_argument("--start", required=True, help="Start time (epoch seconds or ISO8601, UTC recommended)")
    ap.add_argument("--end", required=True, help="End time (epoch seconds or ISO8601)")
    ap.add_argument("--step", type=float, default=1.0, help="Step seconds (default 1.0)")
    ap.add_argument("--out", required=True, help="Output CSV path (e.g. data/run4_nvml_prom.csv)")
    args = ap.parse_args()

    def parse_time(s: str) -> float:
        # epoch
        try:
            return float(s)
        except ValueError:
            pass
        # ISO8601
        try:
            # allow "Z"
            if s.endswith("Z"):
                s = s[:-1] + "+00:00"
            return dt.datetime.fromisoformat(s).timestamp()
        except Exception as e:
            raise SystemExit(f"Could not parse time '{s}': {e}")

    start = parse_time(args.start)
    end = parse_time(args.end)
    step = float(args.step)

    # DCGM exporter metric names commonly used:
    # - DCGM_FI_DEV_POWER_USAGE (W)
    # - DCGM_FI_DEV_GPU_TEMP (C)
    # - DCGM_FI_DEV_SM_CLOCK (MHz)
    # - DCGM_FI_DEV_POWER_MGMT_LIMIT (W)
    #
    # Exporter labels typically include: gpu="0", UUID=..., device=...
    # We'll select by gpu index.
    g = args.gpu
    q_power = f'DCGM_FI_DEV_POWER_USAGE{{gpu="{g}"}}'
    q_temp = f'DCGM_FI_DEV_GPU_TEMP{{gpu="{g}"}}'
    q_smclk = f'DCGM_FI_DEV_SM_CLOCK{{gpu="{g}"}}'
    q_pwr_limit = f'DCGM_FI_DEV_POWER_MGMT_LIMIT{{gpu="{g}"}}'

    print(f"[prom_export] prom={args.prom} gpu={g} start={iso_utc(start)} end={iso_utc(end)} step={step}s")
    print(f"[prom_export] writing {args.out}")

    power = to_series_map(prom_query_range(args.prom, q_power, start, end, step))
    temp = to_series_map(prom_query_range(args.prom, q_temp, start, end, step))
    smclk = to_series_map(prom_query_range(args.prom, q_smclk, start, end, step))
    pwr_limit = to_series_map(prom_query_range(args.prom, q_pwr_limit, start, end, step))

    # Build union of timestamps; keep rows where we have at least power+temp+smclk
    ts_all = sorted(set(power.keys()) | set(temp.keys()) | set(smclk.keys()) | set(pwr_limit.keys()))
    if not ts_all:
        print("[prom_export] ERROR: no samples found. Is Prom scraping dcgm-exporter?", file=sys.stderr)
        return 2

    wrote = 0
    with open(args.out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["ts_utc", "power_w", "temp_c", "sm_clock_mhz", "power_limit_w"])
        for ts in ts_all:
            if ts not in power or ts not in temp or ts not in smclk:
                continue
            w.writerow([
                iso_utc(ts),
                f"{power[ts]:.6f}",
                f"{temp[ts]:.6f}",
                f"{smclk[ts]:.6f}",
                f"{pwr_limit.get(ts, float('nan')):.6f}",
            ])
            wrote += 1

    print(f"[prom_export] rows={wrote}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
