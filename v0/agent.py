#!/usr/bin/env python3
"""
gpu-health-index agent (v0)

Long-running agent that:
  1) Pulls DCGM metrics from Prometheus (query_range) for a rolling window
  2) Computes a passive health score (thermals/clocks/power headroom + telemetry completeness)
  3) Optionally incorporates last active-probe perf/W drift from a JSON state file
  4) Exposes Prometheus metrics on /metrics
  5) Optionally triggers an active probe (your run_experiment_prom.sh) on a schedule or when suspicious

Designed to be systemd-friendly and dependency-light.
Requires: prometheus_client (pip install prometheus-client)

Typical run:
  ./agent.py --prom http://127.0.0.1:9090 --uuid <GPU-UUID> --listen 0.0.0.0:9108

Notes:
- This agent does NOT continuously run GEMM. It can schedule/trigger your existing probe script.
- Passive scoring is meant for "always-on" signal. Perf/W drift is best captured via probes.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

from prometheus_client import Gauge, Counter, Summary, start_http_server


# ----------------------------
# Prometheus helpers
# ----------------------------

def iso_utc(ts: float) -> str:
    return dt.datetime.fromtimestamp(ts, tz=dt.timezone.utc).isoformat().replace("+00:00", "Z")


def http_get_json(url: str, timeout_s: float = 20.0) -> dict:
    with urllib.request.urlopen(url, timeout=timeout_s) as resp:
        body = resp.read()
    return json.loads(body.decode("utf-8"))


def prom_query_range(
    prom_url: str,
    query: str,
    start: float,
    end: float,
    step: float,
    timeout_s: float = 20.0,
) -> List[Tuple[float, float]]:
    """
    Returns list of (timestamp, value) from Prom query_range.
    If multiple series match, we error to keep deterministic behavior.
    """
    params = {
        "query": query,
        "start": f"{start:.3f}",
        "end": f"{end:.3f}",
        "step": f"{step:.3f}",
    }
    url = prom_url.rstrip("/") + "/api/v1/query_range?" + urllib.parse.urlencode(params)
    data = http_get_json(url, timeout_s=timeout_s)

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
            v = float(v_s)
            if v != v:  # NaN
                continue
            out.append((float(ts_s), v))
        except ValueError:
            continue
    return out


def median_dt(ts: List[float]) -> Optional[float]:
    if len(ts) < 3:
        return None
    dts = [ts[i] - ts[i - 1] for i in range(1, len(ts))]
    dts = sorted(dts)
    mid = len(dts) // 2
    if len(dts) % 2 == 1:
        return dts[mid]
    return 0.5 * (dts[mid - 1] + dts[mid])


def quantile(sorted_vals: List[float], q: float) -> float:
    if not sorted_vals:
        return float("nan")
    if q <= 0:
        return sorted_vals[0]
    if q >= 1:
        return sorted_vals[-1]
    pos = (len(sorted_vals) - 1) * q
    lo = int(pos)
    hi = min(lo + 1, len(sorted_vals) - 1)
    frac = pos - lo
    return sorted_vals[lo] * (1 - frac) + sorted_vals[hi] * frac


def mean(vals: List[float]) -> float:
    return sum(vals) / len(vals) if vals else float("nan")


def stdev(vals: List[float]) -> float:
    if len(vals) < 2:
        return 0.0
    m = mean(vals)
    var = sum((v - m) ** 2 for v in vals) / (len(vals) - 1)
    return var ** 0.5


# ----------------------------
# Health logic
# ----------------------------

@dataclass
class PassiveWindow:
    power_w: List[float]
    temp_c: List[float]
    sm_clock_mhz: List[float]
    power_limit_w: List[float]
    ts: List[float]  # timestamps (aligned by prom step, not necessarily identical across metrics)


@dataclass
class ScoreResult:
    score: Optional[float]          # None if incomplete
    classification: str
    reasons: List[str]
    telemetry_ok: bool
    samples: int
    median_step_s: Optional[float]
    temp_p95: Optional[float]
    clk_std: Optional[float]
    pwr_mean: Optional[float]
    pwr_limit: Optional[float]
    pct_high: Optional[float]
    perf_w_mean_last_probe: Optional[float]
    baseline_perf_w: Optional[float]
    perf_drop: Optional[float]


def classify(score: float) -> str:
    if score >= 85:
        return "Healthy"
    if score >= 70:
        return "Monitor"
    if score >= 50:
        return "Degrading"
    return "Decommission Candidate"


def compute_passive_score(
    win: PassiveWindow,
    *,
    expected_step_s: float,
    min_row_ratio: float,
    max_median_dt_s: float,
    power_high_ratio: float,
    power_penalty_max: float,
    temp_p95_warn: float,
    temp_p95_bad: float,
    clk_std_warn: float,
    # optional perf drift from last probe
    perf_w_mean_last_probe: Optional[float],
    baseline_perf_w: Optional[float],
    perf_drop_warn: float,
    perf_drop_bad: float,
    perf_drop_severe: float,
    perf_drop_pen_warn: float,
    perf_drop_pen_bad: float,
    perf_drop_pen_severe: float,
) -> ScoreResult:
    reasons: List[str] = []

    # Telemetry completeness
    n = min(len(win.power_w), len(win.temp_c), len(win.sm_clock_mhz))
    ts = win.ts[:n]
    med_dt = median_dt(ts)

    # expected rows
    window_seconds = (ts[-1] - ts[0]) if len(ts) >= 2 else 0.0
    expected_rows = int(window_seconds / expected_step_s) + 1 if window_seconds > 0 else n
    min_rows = int(expected_rows * min_row_ratio)

    telemetry_ok = True
    if n < 10:
        telemetry_ok = False
        reasons.append(f"Incomplete telemetry: too few samples (n={n})")
    if expected_rows > 0 and n < min_rows:
        telemetry_ok = False
        reasons.append(f"Incomplete telemetry: rows={n} < min_rows={min_rows} (expected≈{expected_rows})")
    if med_dt is not None and med_dt > max_median_dt_s:
        telemetry_ok = False
        reasons.append(f"Incomplete telemetry: median_dt={med_dt:.2f}s > {max_median_dt_s:.2f}s")

    # If incomplete, do not issue a fake Healthy score.
    if not telemetry_ok:
        return ScoreResult(
            score=None,
            classification="Incomplete Telemetry",
            reasons=reasons,
            telemetry_ok=False,
            samples=n,
            median_step_s=med_dt,
            temp_p95=None,
            clk_std=None,
            pwr_mean=None,
            pwr_limit=None,
            pct_high=None,
            perf_w_mean_last_probe=perf_w_mean_last_probe,
            baseline_perf_w=baseline_perf_w,
            perf_drop=None,
        )

    # Compute stats
    power = win.power_w[:n]
    temp = win.temp_c[:n]
    clk = win.sm_clock_mhz[:n]
    pwr_lim = [v for v in win.power_limit_w[:n] if v == v]  # drop NaN

    power_sorted = sorted(power)
    temp_sorted = sorted(temp)

    pwr_mean = mean(power)
    temp_p95 = quantile(temp_sorted, 0.95)
    clk_std = stdev(clk)

    # Power headroom (fraction near limit)
    pwr_limit = pwr_lim[0] if pwr_lim else None
    pct_high = None
    score = 100.0

    # Thermal penalty
    if temp_p95 > temp_p95_warn:
        penalty = 25.0 if temp_p95 >= temp_p95_bad else 10.0
        score -= penalty
        reasons.append(f"Thermal: p95 temp {temp_p95:.1f}C (penalty {penalty:.0f})")

    # Clock stability penalty
    if clk_std > clk_std_warn:
        penalty = min(15.0, (clk_std - clk_std_warn) / 20.0)
        score -= penalty
        reasons.append(f"Clocks: SM clock std {clk_std:.1f}MHz (penalty {penalty:.1f})")

    # Power saturation penalty
    if pwr_limit and pwr_limit > 0:
        ratios = [p / pwr_limit for p in power if pwr_limit > 0]
        pct_high = sum(1 for r in ratios if r >= power_high_ratio) / len(ratios) if ratios else 0.0
        penalty = power_penalty_max * pct_high
        score -= penalty
        reasons.append(
            f"Power headroom: {pct_high*100:.1f}% samples ≥ {power_high_ratio*100:.1f}% of limit "
            f"({pwr_mean:.1f}W mean, limit {pwr_limit:.1f}W) (penalty {penalty:.1f})"
        )
    else:
        reasons.append("Power headroom: power limit missing (no penalty)")

    # Optional: perf/W drift from last probe
    perf_drop = None
    if (perf_w_mean_last_probe is not None) and (baseline_perf_w is not None) and baseline_perf_w > 0:
        perf_drop = 1.0 - (perf_w_mean_last_probe / baseline_perf_w)
        if perf_drop > perf_drop_severe:
            penalty = perf_drop_pen_severe
        elif perf_drop > perf_drop_bad:
            penalty = perf_drop_pen_bad
        elif perf_drop > perf_drop_warn:
            penalty = perf_drop_pen_warn
        else:
            penalty = 0.0

        if penalty > 0:
            score -= penalty
            reasons.append(
                f"Degradation: last probe perf/W {perf_w_mean_last_probe:.6f} vs baseline {baseline_perf_w:.6f} "
                f"({perf_drop*100:.1f}% drop) (penalty {penalty:.0f})"
            )
        else:
            reasons.append(
                f"Baseline: last probe perf/W {perf_w_mean_last_probe:.6f} vs baseline {baseline_perf_w:.6f} "
                f"({perf_drop*100:.1f}% drop)"
            )

    score = max(0.0, min(100.0, score))
    return ScoreResult(
        score=score,
        classification=classify(score),
        reasons=reasons,
        telemetry_ok=True,
        samples=n,
        median_step_s=med_dt,
        temp_p95=float(temp_p95),
        clk_std=float(clk_std),
        pwr_mean=float(pwr_mean),
        pwr_limit=float(pwr_limit) if pwr_limit else None,
        pct_high=float(pct_high) if pct_high is not None else None,
        perf_w_mean_last_probe=perf_w_mean_last_probe,
        baseline_perf_w=baseline_perf_w,
        perf_drop=float(perf_drop) if perf_drop is not None else None,
    )


# ----------------------------
# Agent state + probe integration
# ----------------------------

def read_json_file(path: str) -> Optional[dict]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return None
    except Exception as e:
        print(f"[agent] WARN: failed reading {path}: {e}", file=sys.stderr)
        return None


def atomic_write_json(path: str, obj: dict) -> None:
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)


def maybe_run_probe(
    *,
    now: float,
    last_probe_epoch: Optional[float],
    probe_interval_s: int,
    suspicious: bool,
    suspicious_probe_min_interval_s: int,
    probe_cmd: Optional[List[str]],
    env: Dict[str, str],
    state_path: str,
) -> Tuple[Optional[int], Optional[float]]:
    """
    Returns (exit_code, new_last_probe_epoch) if probe attempted, else (None, last_probe_epoch).
    """
    if not probe_cmd:
        return (None, last_probe_epoch)

    if last_probe_epoch is None:
        due = True
    else:
        due = (now - last_probe_epoch) >= probe_interval_s

    # If suspicious, allow earlier probe but with a safety minimum interval.
    if suspicious and last_probe_epoch is not None:
        due = due or ((now - last_probe_epoch) >= suspicious_probe_min_interval_s)

    if not due:
        return (None, last_probe_epoch)

    print(f"[agent] probe: launching: {' '.join(probe_cmd)}", flush=True)
    try:
        proc = subprocess.run(
            probe_cmd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=60 * 60,  # 1h hard cap; your probe is ~11min + overhead
        )
        out = proc.stdout[-4000:] if proc.stdout else ""
        print(f"[agent] probe: exit={proc.returncode}\n{out}", flush=True)

        new_last = now
        # persist a small probe result (logs + timestamp)
        prev = read_json_file(state_path) or {}
        prev.setdefault("probe", {})
        prev["probe"]["last_probe_epoch"] = int(new_last)
        prev["probe"]["last_probe_exit"] = int(proc.returncode)
        prev["probe"]["last_probe_tail"] = out
        atomic_write_json(state_path, prev)
        return (proc.returncode, new_last)
    except subprocess.TimeoutExpired:
        print("[agent] probe: TIMEOUT", file=sys.stderr, flush=True)
        prev = read_json_file(state_path) or {}
        prev.setdefault("probe", {})
        prev["probe"]["last_probe_epoch"] = int(now)
        prev["probe"]["last_probe_exit"] = 124
        prev["probe"]["last_probe_tail"] = "timeout"
        atomic_write_json(state_path, prev)
        return (124, now)
    except Exception as e:
        print(f"[agent] probe: ERROR: {e}", file=sys.stderr, flush=True)
        return (2, last_probe_epoch)


# ----------------------------
# Prometheus exported metrics (agent)
# ----------------------------

M_SCORE = Gauge("gpu_health_score", "GPU health score (0-100). NaN if incomplete.", ["uuid"])
M_CLASS = Gauge("gpu_health_class", "One-hot class gauge", ["uuid", "class"])
M_TELEMETRY_OK = Gauge("gpu_health_telemetry_ok", "1 if telemetry is complete enough to score, else 0", ["uuid"])
M_SAMPLES = Gauge("gpu_health_samples", "Number of samples used for scoring window", ["uuid"])
M_MEDIAN_DT = Gauge("gpu_health_median_step_seconds", "Median dt between samples", ["uuid"])

M_TEMP_P95 = Gauge("gpu_health_temp_p95_c", "GPU temp p95 (C) over scoring window", ["uuid"])
M_CLK_STD = Gauge("gpu_health_sm_clock_std_mhz", "SM clock stddev (MHz) over scoring window", ["uuid"])
M_PWR_MEAN = Gauge("gpu_health_power_mean_w", "Power mean (W) over scoring window", ["uuid"])
M_PWR_LIMIT = Gauge("gpu_health_power_limit_w", "Power management limit (W)", ["uuid"])
M_PWR_PCT_HIGH = Gauge("gpu_health_power_pct_near_limit", "Fraction of samples near power limit", ["uuid"])

M_PROBE_LAST = Gauge("gpu_health_last_probe_epoch", "Last probe timestamp epoch seconds", ["uuid"])
M_PROBE_EXIT = Gauge("gpu_health_last_probe_exit_code", "Last probe exit code", ["uuid"])

M_PERF_LAST = Gauge("gpu_health_last_probe_perf_per_watt_mean", "Perf/W mean from last probe (if available)", ["uuid"])
M_PERF_BASELINE = Gauge("gpu_health_baseline_perf_per_watt_mean", "Baseline perf/W mean (if available)", ["uuid"])
M_PERF_DROP = Gauge("gpu_health_perf_per_watt_drop_ratio", "Drop ratio vs baseline (0.10 = 10% drop)", ["uuid"])

M_ERRORS = Counter("gpu_health_agent_errors_total", "Agent internal errors", ["kind"])
M_PROM_LAT = Summary("gpu_health_agent_prom_query_seconds", "Latency of Prometheus queries", ["metric"])


# ----------------------------
# Main
# ----------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description="GPU Health Index agent (DCGM+Prom always-on scorer + optional probe scheduler).")
    ap.add_argument("--prom", default="http://127.0.0.1:9090", help="Prometheus base URL")
    ap.add_argument("--uuid", required=True, help="GPU UUID label (e.g. GPU-xxxx)")
    ap.add_argument("--listen", default="127.0.0.1:9108", help="Agent metrics listen address (host:port)")
    ap.add_argument("--poll_s", type=int, default=30, help="Polling interval (seconds)")
    ap.add_argument("--window_s", type=int, default=300, help="Rolling window for passive scoring (seconds)")
    ap.add_argument("--step_s", type=float, default=1.0, help="Prom query_range step (seconds)")
    ap.add_argument("--min_row_ratio", type=float, default=0.80, help="Min rows ratio vs expected to accept telemetry")
    ap.add_argument("--max_median_dt", type=float, default=2.5, help="Max median dt (seconds) to accept telemetry")

    ap.add_argument("--baseline", default="data/baseline.json", help="Baseline JSON (optional; used only with last probe perf/W)")
    ap.add_argument("--state", default="data/agent_state.json", help="Agent state JSON (stores last probe + last perf/W mean)")

    # Probe scheduling
    ap.add_argument("--probe_cmd", default="", help="Command to run active probe (e.g. './run_experiment_prom.sh'). Leave empty to disable.")
    ap.add_argument("--probe_interval_s", type=int, default=6 * 3600, help="Periodic probe interval (seconds)")
    ap.add_argument("--suspicious_probe_min_interval_s", type=int, default=30 * 60, help="Min interval between probes when suspicious (seconds)")

    # Scoring thresholds (keep aligned with analyze.py defaults)
    ap.add_argument("--temp_p95_warn", type=float, default=80.0)
    ap.add_argument("--temp_p95_bad", type=float, default=90.0)
    ap.add_argument("--clk_std_warn", type=float, default=120.0)
    ap.add_argument("--power_high_ratio", type=float, default=0.98)
    ap.add_argument("--power_penalty_max", type=float, default=3.0)

    # Perf drop thresholds (applied only if state has last_probe perf/W mean AND baseline is readable)
    ap.add_argument("--perf_drop_warn", type=float, default=0.03)
    ap.add_argument("--perf_drop_bad", type=float, default=0.07)
    ap.add_argument("--perf_drop_severe", type=float, default=0.12)
    ap.add_argument("--perf_drop_pen_warn", type=float, default=5.0)
    ap.add_argument("--perf_drop_pen_bad", type=float, default=15.0)
    ap.add_argument("--perf_drop_pen_severe", type=float, default=30.0)

    args = ap.parse_args()

    # Start /metrics server
    host, port_s = args.listen.rsplit(":", 1)
    port = int(port_s)
    start_http_server(port, addr=host)
    print(f"[agent] listening on http://{args.listen}/metrics", flush=True)

    uuid = args.uuid

    # Preload baseline perf/W mean (optional)
    baseline_perf = None
    b = read_json_file(args.baseline)
    if b and "perf_per_watt_mean" in b:
        try:
            baseline_perf = float(b["perf_per_watt_mean"])
        except Exception:
            baseline_perf = None

    # probe command
    probe_cmd = None
    if args.probe_cmd.strip():
        # allow passing a shell-like string, but keep it simple:
        # if you need env vars, set them in systemd or wrap with bash -lc
        probe_cmd = args.probe_cmd.strip().split()

    last_probe_epoch: Optional[float] = None
    last_probe_exit: Optional[int] = None

    def set_class_onehot(cls: str) -> None:
        for c in ["Healthy", "Monitor", "Degrading", "Decommission Candidate", "Incomplete Telemetry"]:
            M_CLASS.labels(**{"uuid": uuid, "class": c}).set(1.0 if c == cls else 0.0)

    while True:
        t_end = time.time()
        t_start = t_end - float(args.window_s)

        # Load optional perf/W mean from state (written by your probe / analyzer pipeline)
        # Expected shape (you can choose): state["last_probe"]["perf_per_watt_mean"]
        st = read_json_file(args.state) or {}
        perf_last = None
        if isinstance(st.get("last_probe"), dict) and "perf_per_watt_mean" in st["last_probe"]:
            try:
                perf_last = float(st["last_probe"]["perf_per_watt_mean"])
            except Exception:
                perf_last = None

        # Also load probe metadata if present
        if isinstance(st.get("probe"), dict):
            try:
                last_probe_epoch = float(st["probe"].get("last_probe_epoch")) if st["probe"].get("last_probe_epoch") else last_probe_epoch
            except Exception:
                pass
            try:
                last_probe_exit = int(st["probe"].get("last_probe_exit")) if st["probe"].get("last_probe_exit") is not None else last_probe_exit
            except Exception:
                pass

        # Build queries by UUID to avoid gpu index churn
        # Note: you already have DCGM_FI_DEV_POWER_MGMT_LIMIT enabled.
        q_power = f'DCGM_FI_DEV_POWER_USAGE{{UUID="{uuid}"}}'
        q_temp = f'DCGM_FI_DEV_GPU_TEMP{{UUID="{uuid}"}}'
        q_clk = f'DCGM_FI_DEV_SM_CLOCK{{UUID="{uuid}"}}'
        q_pwr_limit = f'DCGM_FI_DEV_POWER_MGMT_LIMIT{{UUID="{uuid}"}}'

        try:
            with M_PROM_LAT.labels(metric="power").time():
                power = prom_query_range(args.prom, q_power, t_start, t_end, args.step_s)
            with M_PROM_LAT.labels(metric="temp").time():
                temp = prom_query_range(args.prom, q_temp, t_start, t_end, args.step_s)
            with M_PROM_LAT.labels(metric="sm_clock").time():
                clk = prom_query_range(args.prom, q_clk, t_start, t_end, args.step_s)
            with M_PROM_LAT.labels(metric="pwr_limit").time():
                pwr_limit = prom_query_range(args.prom, q_pwr_limit, t_start, t_end, args.step_s)

            # Align by timestamp intersection (strict, keeps stats honest)
            power_map = {ts: v for ts, v in power}
            temp_map = {ts: v for ts, v in temp}
            clk_map = {ts: v for ts, v in clk}
            lim_map = {ts: v for ts, v in pwr_limit}

            ts_all = sorted(set(power_map) & set(temp_map) & set(clk_map))
            pw = [power_map[ts] for ts in ts_all]
            tc = [temp_map[ts] for ts in ts_all]
            sc = [clk_map[ts] for ts in ts_all]
            pl = [lim_map.get(ts, float("nan")) for ts in ts_all]

            win = PassiveWindow(power_w=pw, temp_c=tc, sm_clock_mhz=sc, power_limit_w=pl, ts=ts_all)

            res = compute_passive_score(
                win,
                expected_step_s=float(args.step_s),
                min_row_ratio=float(args.min_row_ratio),
                max_median_dt_s=float(args.max_median_dt),
                power_high_ratio=float(args.power_high_ratio),
                power_penalty_max=float(args.power_penalty_max),
                temp_p95_warn=float(args.temp_p95_warn),
                temp_p95_bad=float(args.temp_p95_bad),
                clk_std_warn=float(args.clk_std_warn),
                perf_w_mean_last_probe=perf_last,
                baseline_perf_w=baseline_perf,
                perf_drop_warn=float(args.perf_drop_warn),
                perf_drop_bad=float(args.perf_drop_bad),
                perf_drop_severe=float(args.perf_drop_severe),
                perf_drop_pen_warn=float(args.perf_drop_pen_warn),
                perf_drop_pen_bad=float(args.perf_drop_pen_bad),
                perf_drop_pen_severe=float(args.perf_drop_pen_severe),
            )

            # Emit metrics
            M_TELEMETRY_OK.labels(uuid=uuid).set(1.0 if res.telemetry_ok else 0.0)
            M_SAMPLES.labels(uuid=uuid).set(float(res.samples))
            M_MEDIAN_DT.labels(uuid=uuid).set(float(res.median_step_s) if res.median_step_s is not None else float("nan"))

            if res.score is None:
                M_SCORE.labels(uuid=uuid).set(float("nan"))
            else:
                M_SCORE.labels(uuid=uuid).set(float(res.score))
            set_class_onehot(res.classification)

            if res.temp_p95 is not None:
                M_TEMP_P95.labels(uuid=uuid).set(res.temp_p95)
            else:
                M_TEMP_P95.labels(uuid=uuid).set(float("nan"))

            if res.clk_std is not None:
                M_CLK_STD.labels(uuid=uuid).set(res.clk_std)
            else:
                M_CLK_STD.labels(uuid=uuid).set(float("nan"))

            if res.pwr_mean is not None:
                M_PWR_MEAN.labels(uuid=uuid).set(res.pwr_mean)
            else:
                M_PWR_MEAN.labels(uuid=uuid).set(float("nan"))

            if res.pwr_limit is not None:
                M_PWR_LIMIT.labels(uuid=uuid).set(res.pwr_limit)
            else:
                M_PWR_LIMIT.labels(uuid=uuid).set(float("nan"))

            if res.pct_high is not None:
                M_PWR_PCT_HIGH.labels(uuid=uuid).set(res.pct_high)
            else:
                M_PWR_PCT_HIGH.labels(uuid=uuid).set(float("nan"))

            if last_probe_epoch is not None:
                M_PROBE_LAST.labels(uuid=uuid).set(float(last_probe_epoch))
            else:
                M_PROBE_LAST.labels(uuid=uuid).set(float("nan"))

            if last_probe_exit is not None:
                M_PROBE_EXIT.labels(uuid=uuid).set(float(last_probe_exit))
            else:
                M_PROBE_EXIT.labels(uuid=uuid).set(float("nan"))

            if perf_last is not None:
                M_PERF_LAST.labels(uuid=uuid).set(perf_last)
            else:
                M_PERF_LAST.labels(uuid=uuid).set(float("nan"))

            if baseline_perf is not None:
                M_PERF_BASELINE.labels(uuid=uuid).set(baseline_perf)
            else:
                M_PERF_BASELINE.labels(uuid=uuid).set(float("nan"))

            if res.perf_drop is not None:
                M_PERF_DROP.labels(uuid=uuid).set(res.perf_drop)
            else:
                M_PERF_DROP.labels(uuid=uuid).set(float("nan"))

            # Persist a "latest" snapshot (handy for debugging / dashboards / logs)
            latest = {
                "ts_utc": iso_utc(time.time()),
                "window": {"start_utc": iso_utc(t_start), "end_utc": iso_utc(t_end), "step_s": args.step_s},
                "uuid": uuid,
                "score": res.score,
                "classification": res.classification,
                "telemetry_ok": res.telemetry_ok,
                "samples": res.samples,
                "median_step_s": res.median_step_s,
                "temp_p95": res.temp_p95,
                "clk_std": res.clk_std,
                "power_mean": res.pwr_mean,
                "power_limit": res.pwr_limit,
                "pct_high": res.pct_high,
                "last_probe_perf_w_mean": perf_last,
                "baseline_perf_w": baseline_perf,
                "perf_drop": res.perf_drop,
                "reasons": res.reasons,
            }
            st.setdefault("latest", {})
            st["latest"] = latest
            atomic_write_json(args.state, st)

            # Suspicion heuristic for early probe:
            # - Passive says Degrading/Monitor OR high clock variance OR thermal p95 close to warn
            suspicious = (
                res.classification in ("Monitor", "Degrading", "Decommission Candidate")
                or (res.clk_std is not None and res.clk_std > float(args.clk_std_warn) * 1.5)
                or (res.temp_p95 is not None and res.temp_p95 > float(args.temp_p95_warn) - 3.0)
            )

            # Maybe run probe
            env = os.environ.copy()
            # If you want to drive OUT_PREFIX etc, wrap probe_cmd with bash -lc in systemd
            code, new_last = maybe_run_probe(
                now=time.time(),
                last_probe_epoch=last_probe_epoch,
                probe_interval_s=int(args.probe_interval_s),
                suspicious=suspicious,
                suspicious_probe_min_interval_s=int(args.suspicious_probe_min_interval_s),
                probe_cmd=probe_cmd,
                env=env,
                state_path=args.state,
            )
            if new_last is not None:
                last_probe_epoch = new_last
            if code is not None:
                last_probe_exit = code

        except Exception as e:
            M_ERRORS.labels(kind="loop").inc()
            print(f"[agent] ERROR: {e}", file=sys.stderr, flush=True)

        time.sleep(max(1, int(args.poll_s)))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
