# GPU Health Index — Methodology

## 1. Research Objective

To detect GPU degradation and instability using a deterministic, reproducible, and hardware-normalized scoring methodology based on:

- Envelope compliance (thermal/power)
- Stability metrics (clock / efficiency variance)
- Baseline-relative performance-per-watt drift

The system is designed to:

- Avoid false positives from incomplete telemetry
- Enable reproducible comparison across time
- Support lifecycle decision-making

---

# 2. Experimental Design

## 2.1 Hypothesis

A GPU exhibiting degradation or instability will show measurable deviation in:

- Performance-per-watt under controlled load
- Thermal envelope behavior
- Clock stability
- Power saturation characteristics

relative to its own historical baseline.

---

# 3. Variables

## 3.1 Independent Variables (Controlled Inputs)

These are deliberately controlled by the experiment:

- Workload type: Sustained PyTorch GEMM
- Load duration
- Phase timing (idle / load / cooldown)
- Sampling resolution (1 Hz)
- Power limit (if enforced)

The workload protocol is deterministic and defined via `phases.json`.

---

## 3.2 Dependent Variables (Measured Outputs)

Measured telemetry signals:

- Power draw (`power_w`)
- Power limit (`power_limit_w`)
- GPU temperature (`temp_c`)
- SM clock (`sm_clock_mhz`)
- Memory clock
- Utilization
- Throughput (`iters_per_sec`)

Derived signal:

```bash
perf_per_watt = iters_per_sec / power_w
```


This is the primary health indicator.

---

## 3.3 Controlled Variables

To ensure comparability:

- GPU identified by UUID (not index)
- Fixed sampling resolution (1 Hz)
- Deterministic phase slicing
- Steady-state trimming
- Effective-load filtering

---

# 4. Data Collection

## 4.1 Telemetry Acquisition

Two supported paths:

- NVML → CSV (`collector.py`)
- DCGM via Prometheus → CSV (`prom_export.py`)

Sampling rate: 1 Hz

Telemetry completeness is validated using:

- Minimum row threshold
- Median scrape interval (`median_dt`)
- Single-series enforcement per UUID

If conditions fail → scoring returns **N/A (Incomplete Telemetry)**.

---

# 5. Signal Processing

## 5.1 Time Alignment

Throughput samples and telemetry are aligned via nearest-neighbor time merge (`merge_asof`) with ±1s tolerance.

This produces a single merged dataset per run.

---

## 5.2 Phase Isolation

Scoring is restricted to steady-state load:

- Load window defined by `phases.json`
- First `steady_trim_s` seconds removed
- Fallback heuristic available but deterministic mode preferred

Optional effective-load filter:

```bash
power_w >= 0.9 * power_limit
```


If fewer than 50% of steady-state samples meet criteria → scoring aborted.

This enforces comparability under effective saturation.

---

# 6. Health Signal Construction

## 6.1 Envelope Metrics

- p95 temperature
- Fraction of time near power limit

Threshold examples (v0):

- Temp warn: 80°C
- Temp critical: 90°C

---

## 6.2 Stability Metrics

- SM clock standard deviation
- Coefficient of variation of perf/W

These detect oscillation and throttling behavior.

---

## 6.3 Baseline Construction

Baseline built from one or more historical steady-state runs:

```bash
baseline_perf_w = mean(perf_per_watt_steady_state)
```


Baseline is defined per GPU / environment.

---

## 6.4 Degradation Detection

Current steady-state perf/W is compared against baseline.

Penalty tiers:

- >3% drop
- >7% drop
- >12% drop

This isolates efficiency degradation independent of absolute performance.

---

# 7. Scoring Model

Score initialized at 100.

Penalties applied for:

- Envelope violations
- Stability violations
- Baseline-relative drift

Classification:

| Score | Interpretation |
|-------|----------------|
| ≥85   | Healthy |
| 70–85 | Monitor |
| 50–70 | Degrading |
| <50   | Decommission Candidate |

Each score includes structured explanatory reasons.

---

# 8. Validation Strategy

Because real failure labels are limited, validation is performed using synthetic replay datasets.

`make_replay.py` injects controlled degradations into real merged datasets:

- Efficiency droop
- Clock droop
- Thermal increase
- Combined severe degradation

Expected classifier response is verified using `analyze_merged.py`.

This provides deterministic regression testing of scoring logic.

---

# 9. Statistical Safeguards

To prevent invalid conclusions:

- Telemetry completeness gating
- Steady-state coverage requirement (e.g., ≥70%)
- Effective-load requirement
- Single-series UUID enforcement
- Explicit N/A classification for insufficient data

Lifecycle decisions are never made on incomplete datasets.

---

# 10. Limitations (v0)

- ECC error rates not incorporated
- Baseline normalization across heterogeneous fleets not implemented
- Rule-based scoring (no supervised ML)
- Environment-sensitive baselines required

---

# 11. Method Summary

The system implements a two-layer health model:

1. Passive telemetry scoring (always-on envelope + stability checks)
2. Active controlled-load probe measuring baseline-relative perf/W drift

All signals are deterministically derived, gated for comparability, and reproducible.
