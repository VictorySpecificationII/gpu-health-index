# GPU Health Index (v0)

Validated on a single NVIDIA H200 cloud instance using sustained GEMM workloads to verify telemetry collection, baseline modeling, and degradation classification behavior on current-generation GPUs.

Deterministic GPU health scoring using:

- NVIDIA DCGM Exporter

- NVIDIA NVML

- Prometheus (1s scrape)

- Optional active GEMM probe

- Baseline-relative performance drift detection

- Optional long-running health agent

This repository provides a reproducible reference implementation for evaluating GPU thermal, power, clock, and performance health.


## Table of Contents

1. Overview

2. Architecture

3. Requirements

4. Setup (DCGM + Prometheus)

5. Running a Full Health Experiment

6. Creating a Baseline

7. Replay Validation

8. Always-On Agent

9. Health Model (v0)

10. Notes & Scope


## 1. Overview

The GPU Health Index combines:

### Passive telemetry

- Power usage

- GPU temperature

- SM clock stability

- Power headroom

### Active probe (optional)

- Sustained GEMM workload

- Iterations/sec

- Perf-per-watt comparison against baseline

### Scoring

Score starts at 100 and deducts based on:

- Thermal envelope violations

- Clock instability

- Sustained power saturation

- Performance degradation vs baseline

## 2. Architecture

```bash
+-------------------+
|  DCGM Exporter    |
|  (port 9400)      |
+-------------------+
           |
           v
+-------------------+
|   Prometheus      |
|   (port 9090)     |
+-------------------+
           |
           v
+-------------------+
|  prom_export.py   |
+-------------------+
           |
           v
+-------------------+
|   analyze.py      |
+-------------------+
           |
           v
+-------------------+
| Health Score v0   |
+-------------------+

Optional:

agent.py --> exports Prometheus metrics (:9108)

```

## 3. Requirements

- NVIDIA driver installed

- Docker + Docker Compose

- Python 3.9+

- pip packages:

  - pandas

  - matplotlib

  - prometheus-client (for agent)

Install Python dependencies:

```bash
pip install -r requirements.txt
pip install prometheus-client
```

## 4. Setup: DCGM + Prometheus


Start telemetry stack:

```bash
docker compose -f docker-compose.dcgm-prom.yml up -d
```

Verify exporter:

```bash
curl -s http://127.0.0.1:9400/metrics | head
```

Verify Prometheus:

```bash
curl -s http://127.0.0.1:9090/-/ready
```

Expected:

```bash
Prometheus Server is Ready.
```

## 5. Run a Full Health Experiment

This performs:

- Idle phase

- Sustained GEMM load

- Cooldown

- Prom telemetry export

- Deterministic phase slicing

- Health scoring

- Plots + report generation

### Get GPU index

```bash
nvidia-smi
```

GPU 0 is typically index 0.

### Run experiment

```bash
OUT_PREFIX=data/run1 \
GPU_INDEX=0 \
INTERVAL_S=1 \
IDLE_S=180 \
LOAD_S=300 \
COOLDOWN_S=180 \
STEADY_TRIM_S=30 \
./run_experiment_prom.sh
```

### Artifacts Generated

```bash
data/run1_nvml_prom.csv
data/run1_gemm.csv
data/run1_merged.csv
data/run1_report.md

data/run1_power.png
data/run1_temp.png
data/run1_sm_clock.png
data/run1_iters_per_sec.png
data/run1_perf_per_watt.png
```

## 6. Create a Baseline

After a known-good run:

```bash
python make_baseline.py \
  --merged data/run1_merged.csv \
  --out data/baseline.json
```

Example baseline:

```bash
{
  "perf_per_watt_mean": 0.874878
}
```

## 7. Replay Validation

Validate scoring logic against synthetic degraded dataset:

```bash
./analyze_merged.py \
  --merged data/replay/degrading_combined_severe.csv \
  --phases data/run1_phases.json \
  --baseline data/baseline.json \
  --out_prefix /tmp/replay_test
```

Expected classification:

```bash
Classification: Degrading
Health Score: ~57
```

## 8. Prometheus Telemetry Smoke Test

Get GPU UUID:

```bash
nvidia-smi --query-gpu=uuid --format=csv,noheader
```

Export last 2 minutes of telemetry:

```bash
./prom_export.py \
  --prom http://127.0.0.1:9090 \
  --uuid GPU-xxxxxxxx \
  --start $(date -u -d "2 minutes ago" +%s) \
  --end $(date -u +%s) \
  --step 1 \
  --out /tmp/prom_smoke.csv
```

## 9. Always-On Health Agent (Optional)

The agent continuously:

- Pulls rolling Prometheus window

- Computes passive health score

- Exposes Prometheus metrics


### Start Agent

```bash
./agent.py \
  --prom http://127.0.0.1:9090 \
  --uuid GPU-xxxxxxxx \
  --listen 0.0.0.0:9108 \
  --poll_s 30 \
  --window_s 300 \
  --step_s 1
```

Verify Agent Metrics

```bash
curl -s http://127.0.0.1:9108/metrics | grep gpu_health_score
```

Example:

```bash
gpu_health_score{uuid="GPU-..."} 97.0
```

### Metrics Exported

```bash
gpu_health_score

gpu_health_class

gpu_health_temp_p95_c

gpu_health_sm_clock_std_mhz

gpu_health_power_pct_near_limit

gpu_health_telemetry_ok
```

## 10. Health Model (v0)

Score starts at 100.
## Penalties

| Condition                   | Penalty        | Rationale |
|----------------------------|---------------|-----------|
| Temp p95 > 80°C            | -10           | Sustained operation above optimal thermal envelope reduces long-term silicon reliability. |
| Temp p95 > 90°C            | -25           | Critical thermal stress region; high likelihood of clock throttling and accelerated degradation. |
| SM clock instability       | up to -15     | High standard deviation indicates throttling, voltage instability, or thermal oscillation. |
| Sustained power saturation | up to -3      | Operating near power limit reduces performance headroom and may indicate cooling or workload imbalance. |
| Perf/W drop > 3%           | -5            | Early indicator of efficiency drift versus baseline. |
| Perf/W drop > 7%           | -15           | Material performance degradation under comparable workload. |
| Perf/W drop > 12%          | -30           | Severe efficiency regression; possible silicon degradation or sustained throttling. |

## Classification

| Score | Classification           | Interpretation |
|-------|--------------------------|----------------|
| ≥ 85  | Healthy                  | Operating within expected thermal, power, and efficiency envelopes. |
| ≥ 70  | Monitor                  | Minor drift detected; observe trends and validate cooling/workload behavior. |
| ≥ 50  | Degrading                | Sustained instability or measurable efficiency loss; intervention recommended. |
| < 50  | Decommission Candidate   | Significant degradation; hardware inspection or retirement advised. |

## 11. Scope & Status

This is GPU Health Index v0.

It is:

- Deterministic

- UUID-based (safe against index changes)

- Replay-validated

- Baseline-relative

- Agent-capable

It is not yet:

- Multi-GPU cluster-wide

- Kubernetes-native

- Alertmanager-integrated

- Production-hardened for HA

It is intended as a clean reference foundation for further development.
