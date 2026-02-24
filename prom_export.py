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


def prom_query_range(prom_url: str, query: str, start: float, end: float, step: float) -> List[Tuple[float, float]]:
    """
    Returns list of (timestamp, value) from Prom query_range.
    If multiple series match, we error (keep deterministic).
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


def median_dt_seconds(ts_sorted: List[float]) -> Optional[float]:
    if len(ts_sorted) < 3:
        return None
    dts = [ts_sorted[i + 1] - ts_sorted[i] for i in range(len(ts_sorted) - 1)]
    dts = [x for x in dts if x > 0]
    if not dts:
        return None
    dts.sort()
    mid = len(dts) // 2
    if len(dts) % 2 == 1:
        return dts[mid]
    return 0.5 * (dts[mid - 1] + dts[mid])


def main() -> int:
    ap = argparse.ArgumentParser(description="Export DCGM telemetry from Prometheus into nvml-like CSV.")
    ap.add_argument("--prom", default="http://127.0.0.1:9090", help="Prometheus base URL")

    # Prefer UUID in any serious setup; gpu index is fallback.
    ap.add_argument("--uuid", default=None, help='GPU UUID (e.g. "GPU-xxxx"). Preferred selector.')
    ap.add_argument("--gpu", type=int, default=0, help='GPU index label (gpu="0"). Fallback if --uuid not set.')

    ap.add_argument("--start", required=True, help="Start time (epoch seconds or ISO8601, UTC recommended)")
    ap.add_argument("--end", required=True, help="End time (epoch seconds or ISO8601)")
    ap.add_argument("--step", type=float, default=1.0, help="Step seconds (default 1.0)")
    ap.add_argument("--out", required=True, help="Output CSV path (e.g. data/run4_nvml_prom.csv)")

    # Guards
    ap.add_argument("--min_rows", type=int, default=0, help="If >0, error when fewer rows are written.")
    ap.add_argument("--max_median_dt", type=float, default=2.5, help="Error if median sample spacing exceeds this (seconds).")

    args = ap.parse_args()

    def parse_time(s: str) -> float:
        try:
            return float(s)
        except ValueError:
            pass
        try:
            if s.endswith("Z"):
                s = s[:-1] + "+00:00"
            return dt.datetime.fromisoformat(s).timestamp()
        except Exception as e:
            raise SystemExit(f"Could not parse time '{s}': {e}")

    start = parse_time(args.start)
    end = parse_time(args.end)
    step = float(args.step)

    # Selector
    if args.uuid:
        sel = f'UUID="{args.uuid}"'
        sel_desc = f"UUID={args.uuid}"
    else:
        sel = f'gpu="{args.gpu}"'
        sel_desc = f'gpu={args.gpu}'

    # Metric queries (DCGM exporter)
    q_power = f"DCGM_FI_DEV_POWER_USAGE{{{sel}}}"
    q_temp = f"DCGM_FI_DEV_GPU_TEMP{{{sel}}}"
    q_smclk = f"DCGM_FI_DEV_SM_CLOCK{{{sel}}}"
    q_pwr_limit = f"DCGM_FI_DEV_POWER_MGMT_LIMIT{{{sel}}}"

    print(
        f"[prom_export] prom={args.prom} sel=({sel_desc}) "
        f"start={iso_utc(start)} end={iso_utc(end)} step={step}s"
    )
    print(f"[prom_export] writing {args.out}")

    power = to_series_map(prom_query_range(args.prom, q_power, start, end, step))
    temp = to_series_map(prom_query_range(args.prom, q_temp, start, end, step))
    smclk = to_series_map(prom_query_range(args.prom, q_smclk, start, end, step))
    pwr_limit = to_series_map(prom_query_range(args.prom, q_pwr_limit, start, end, step))

    ts_all = sorted(set(power.keys()) | set(temp.keys()) | set(smclk.keys()) | set(pwr_limit.keys()))
    if not ts_all:
        print("[prom_export] ERROR: no samples found. Is Prom scraping dcgm-exporter?", file=sys.stderr)
        return 2

    wrote = 0
    wrote_ts: List[float] = []
    with open(args.out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["ts_utc", "power_w", "temp_c", "sm_clock_mhz", "power_limit_w"])
        for ts in ts_all:
            # Require core triplet
            if ts not in power or ts not in temp or ts not in smclk:
                continue
            w.writerow(
                [
                    iso_utc(ts),
                    f"{power[ts]:.6f}",
                    f"{temp[ts]:.6f}",
                    f"{smclk[ts]:.6f}",
                    f"{pwr_limit.get(ts, float('nan')):.6f}",
                ]
            )
            wrote += 1
            wrote_ts.append(ts)

    med = median_dt_seconds(wrote_ts)
    print(f"[prom_export] rows={wrote} median_dt={med if med is not None else 'n/a'}")

    # Guards
    if args.min_rows > 0 and wrote < args.min_rows:
        print(f"[prom_export] ERROR: wrote {wrote} rows < min_rows={args.min_rows}", file=sys.stderr)
        return 3
    if med is not None and med > float(args.max_median_dt):
        print(f"[prom_export] ERROR: median_dt={med:.3f}s > max_median_dt={args.max_median_dt}s", file=sys.stderr)
        return 4

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
