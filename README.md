GPU Health Index (v0)

A deterministic, explainable methodology for assessing GPU health using telemetry, stress testing, and baseline-relative degradation analysis.

This project demonstrates how to:

Collect GPU telemetry using NVML

Run controlled stress workloads (PyTorch GEMM)

Define steady-state load windows deterministically

Build a reproducible performance baseline

Detect degradation using baseline-relative drift

Classify GPUs into operational health categories

This is not a monitoring dashboard.
This is a reliability engineering experiment.

1. Motivation

Large GPU fleets degrade.

Not catastrophically at first — but gradually:

Clock instability

Thermal envelope expansion

Efficiency decay

Sustained power saturation

The goal is to detect measurable degradation before failure, using:

Controlled workload conditions

Deterministic slicing of telemetry windows

Explicit scoring logic

No black-box ML

The output is an explainable health score and classification.

2. Methodology Overview

Each experiment consists of three deterministic phases:

Idle      →  Load      →  Cooldown
180s         300s         180s

Telemetry is collected at 1-second resolution:

Power (W)

Temperature (°C)

SM clock (MHz)

Throughput (iters/sec)

Derived: perf_per_watt

A steady-state trim removes ramp noise from the load window.

All scoring is performed on the steady-state window.

3. Health Score Model (v0)

Score starts at 100.

Penalties are explicitly defined:

Thermal envelope

p95 > 80°C → -10

p95 ≥ 90°C → -25

Clock instability

SM clock stddev > 120 MHz → penalty up to -15

Efficiency instability

perf/W coefficient of variation > 0.20 → penalty up to -10

Power headroom

Fraction of samples ≥ 98% of power limit

Max penalty: -3

Baseline-relative degradation (perf/W drop)

Compared to baseline steady-state mean:

3% drop → -5

7% drop → -15

12% drop → -30

Classification:

≥ 85 → Healthy

≥ 70 → Monitor

≥ 50 → Degrading

< 50 → Decommission Candidate

All penalties are explainable and visible in output.

4. Running a Real Experiment
4.1 Run controlled test

./run_experiment.sh

Or specify output prefix:

OUT_PREFIX=data/run2 ./run_experiment.sh

Artifacts produced:

data/runX_nvml.csv
data/runX_gemm.csv
data/runX_merged.csv
data/runX_report.md
data/runX_*.png

4.2 Build Baseline

After 3 stable runs:

./make_baseline.py \
  --merged data/run1_merged.csv data/run2_merged.csv data/run3_merged.csv \
  --phases data/run1_phases.json \
  --out data/baseline.json

Example baseline:

perf_per_watt_mean = 0.874878

This becomes the reference envelope for degradation detection.

5. Replay Degradation (Deterministic Simulation)

Replay datasets simulate realistic degradation patterns:

Efficiency decay (-10%)

Clock frequency drop (-8%)

Thermal envelope expansion (+10°C)

Combined strong degradation

Combined severe degradation

Generate replay data:

python3 make_replay.py --in data/run1_merged.csv --out_dir data/replay

Run full demo:

./run_replay_demo.sh

6. Example Classification Results

Using a baseline of:

perf/W mean = 0.874878

Healthy (real runs)

Health Score: 97.0 / 100 → Healthy
Notes:
 - Power headroom penalty (3)

Monitor (combined strong replay)

Perf/W drop: 10.8%
Health Score: 82.0 / 100 → Monitor
Notes:
 - Power headroom penalty (3)
 - Degradation penalty (15)

Degrading (combined severe replay)

Perf/W drop: 16.2%
Thermal p95: 82.8°C

Health Score: 57.0 / 100 → Degrading
Notes:
 - Thermal penalty (10)
 - Power headroom penalty (3)
 - Degradation penalty (30)

The system responds predictably to measured degradation.

7. Design Principles

Deterministic phase slicing (no heuristics if phases.json provided)

No hidden ML

Explicit envelope thresholds

Baseline-relative drift detection

Reproducible experiments

Clear, inspectable artifacts

This mirrors how reliability engineers operate:

Measure → Define envelope → Detect drift → Escalate

8. Repository Structure

collector.py              # NVML telemetry collection
stress_torch_gemm.py      # Controlled GPU workload
run_experiment.sh         # End-to-end experiment runner
analyze.py                # Merge + score live run
make_baseline.py          # Build baseline from stable runs
make_replay.py            # Generate degradation simulations
analyze_merged.py         # Score replay datasets
run_replay_demo.sh        # Reproduce full demo

9. Future Extensions

DCGM integration

Prometheus exporter

Fleet-level aggregation

Secondary market evaluation model

Longitudinal degradation tracking

10. Why This Matters

This repository demonstrates:

Hands-on GPU telemetry analysis

Stress testing methodology

Time-series degradation modeling

Deterministic classification design

Production-oriented reliability thinking

It is not a monitoring dashboard.

It is a structured health assessment framework for GPU infrastructure.


