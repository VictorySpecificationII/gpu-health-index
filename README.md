# gpu-health-exporter

A production-grade GPU health monitoring exporter written in C. Exposes 55+
Prometheus metrics per GPU via a `/metrics` endpoint on port 9108.

Designed for AI factory scale: thousands of multi-GPU hosts, bare metal and
Kubernetes, NVML + DCGM, no hard link dependencies.

---

## What it does

One process per node. One poll thread per GPU. A fork-based privilege
separation model isolates the HTTP server from NVML access.

**Parent process** — runs one NVML/DCGM poll thread per GPU. Computes a
health score every poll cycle. Pushes fixed-size snapshots to the child over
a Unix socketpair.

**Child process** — serves `/metrics`, `/ready`, and `/live` over TCP.
Holds no GPU handles. Dropped to minimum capabilities after fork. Respawned
by the parent if it exits.

**Signal sources:**
- NVML: required. Loaded via `dlopen` — no hard link dependency on
  `libnvidia-ml.so`. Captures compute, thermal, memory capacity, ECC, PCIe,
  and throttle state. NVML alone is an incomplete health picture.
- DCGM: required operationally. Loaded via `dlopen`. Adds board power, energy,
  memory bandwidth utilization, NVLink counters, XID errors, and
  power/thermal violation time — signals that have no NVML equivalent.
  The binary soft-fails if DCGM is absent (fields emit `NaN`,
  `gpu_dcgm_available 0`), but this state is an anomaly worth alerting on,
  not an acceptable degraded mode.

---

## Requirements

**Build:**
- GCC with C11 support
- `libdl`, `libpthread`, `libm` (standard on any Linux system)
- Optional: mbedTLS (`WITH_TLS=1`)

**Runtime:**
- NVML (`libnvidia-ml.so`) in the dynamic linker path. Present on any host
  with NVIDIA drivers installed.
- DCGM daemon (`nv-hostengine`) running on the host. `libdcgm.so` must be
  in the dynamic linker path.
- A writable `state_dir` (default: `/var/run/gpu-health`).

**Supported GPUs:** Ampere (A100) and later. Blackwell supported natively
(NVML is additive).

---

## Building

```sh
# Release build
make

# Debug build — AddressSanitizer + UBSan + debug symbols
make DEBUG=1

# With TLS support (requires mbedTLS)
make WITH_TLS=1

# Run all tests (no GPU required — tests use a fake vtable)
make test

# Install to /usr/local/bin
sudo make install PREFIX=/usr/local
```

Binary: `build/gpu-health-exporter`

---

## Quick start

```sh
# Create the state directory (required — hard fail at startup if missing)
sudo mkdir -p /var/run/gpu-health

# Run with compiled-in defaults
sudo ./build/gpu-health-exporter

# Or point at a config file
sudo ./build/gpu-health-exporter -c /etc/gpu-health/gpu-health.conf

# Verify
curl http://localhost:9108/metrics
curl http://localhost:9108/ready
```

Logs go to stderr. If the DCGM daemon is not running, the exporter will
start but `gpu_dcgm_available{...}` will be `0` and DCGM-sourced fields will
emit `NaN`. This is an anomaly state — alert on it.

---

## Configuration

Config file format: `key = value`, one per line. `#` comments. Unknown
keys are ignored (forward compatible).

All values can be overridden with environment variables using the
`GPU_HEALTH_<KEY>` prefix (uppercase). Environment takes precedence over
the config file, which takes precedence over compiled-in defaults.

```
# ── Paths ─────────────────────────────────────────────────────────────
state_dir                   = /var/run/gpu-health
baseline_dir                = /etc/gpu-health/baseline
listen_addr                 = 0.0.0.0
listen_port                 = 9108

# ── Polling ───────────────────────────────────────────────────────────
poll_interval_s             = 1
window_s                    = 300
state_write_interval_s      = 30

# ── Telemetry completeness gate ───────────────────────────────────────
# Score is N/A until these thresholds are met.
min_sample_ratio            = 0.80
max_median_step_s           = 2.5
min_samples_absolute        = 10

# ── Scoring — thermal ─────────────────────────────────────────────────
temp_p95_warn_c             = 80
temp_p95_bad_c              = 90
hbm_temp_p95_warn_c         = 85
hbm_temp_p95_bad_c          = 95

# ── Scoring — clocks ──────────────────────────────────────────────────
clk_std_warn_mhz            = 120

# ── Scoring — power ───────────────────────────────────────────────────
power_high_ratio            = 0.98
power_penalty_max           = 3.0

# ── Scoring — memory (ECC / retired pages / row remap) ────────────────
ecc_sbe_rate_warn_per_hour  = 100
ecc_sbe_penalty             = 5.0
ecc_dbe_penalty             = 25.0
retired_pages_warn          = 1
retired_pages_bad           = 10
retired_pages_pen_warn      = 5.0
retired_pages_pen_bad       = 15.0
row_remap_failure_penalty   = 25.0

# ── Scoring — PCIe ────────────────────────────────────────────────────
pcie_link_degraded_penalty  = 10.0
pcie_width_degraded_penalty = 15.0

# ── Scoring — perf/W drift vs baseline ───────────────────────────────
perf_drop_warn              = 0.03
perf_drop_bad               = 0.07
perf_drop_severe            = 0.12
perf_drop_pen_warn          = 5.0
perf_drop_pen_bad           = 15.0
perf_drop_pen_severe        = 30.0

# ── Probe result TTL ──────────────────────────────────────────────────
# TTL is 1.5× probe_interval_s by design — one missed probe does not
# immediately drop the perf/W signal.
probe_interval_s            = 86400
probe_ttl_s                 = 129600

# ── NVML / DCGM ───────────────────────────────────────────────────────
nvml_timeout_ms             = 5000
nvml_error_threshold        = 10
nvml_hard_error_threshold   = 1
nvml_retry_interval_s       = 60
dcgm_timeout_ms             = 5000
dcgm_error_threshold        = 10
dcgm_retry_interval_s       = 60

# ── TLS (only active with WITH_TLS=1 build) ───────────────────────────
tls_cert_path               =
tls_key_path                =
```

---

## Deployment

### Bare metal (systemd)

The unit file is at [deploy/gpu-health.service](deploy/gpu-health.service).

```sh
# Create config directory and drop in your config
sudo mkdir -p /etc/gpu-health
sudo cp deploy/gpu-health.conf.example /etc/gpu-health/gpu-health.conf

# Install binary + unit file
sudo make install PREFIX=/usr/local

# Enable and start
sudo systemctl daemon-reload
sudo systemctl enable --now gpu-health
```

Key unit properties:
- `Requires=nv-hostengine.service` — if the DCGM daemon stops, this unit stops with it
- `RuntimeDirectory=gpu-health` — systemd creates `/var/run/gpu-health` automatically
- `TimeoutStartSec=60s` — the exporter sends `READY=1` after the first successful poll on all GPUs; 60s gives margin for driver and DCGM init on large GPU counts
- `Type=notify` — `sd_notify READY=1` is sent over a raw Unix socket; no libsystemd dependency

### Kubernetes (DaemonSet)

The exporter runs as a DaemonSet, one pod per node. Baseline files are
mounted from a ConfigMap.

Key pod spec requirements:
- `hostNetwork: true` or a `Service` exposing port 9108
- `securityContext.privileged: true` or at minimum `SYS_ADMIN` capability
  (required for NVML device access)
- `/dev/nvidia*` and `/dev/nvidiactl` mounted into the pod
- `NVML_DSO_PATH` env var if `libnvidia-ml.so` is not in the standard path
- Liveness probe: `GET /live` — 200 while poll loop is running
- Readiness probe: `GET /ready` — 200 after first successful poll

Environment variable overrides for pod-level config:
```yaml
env:
  - name: GPU_HEALTH_LISTEN_ADDR
    value: "0.0.0.0"
  - name: GPU_HEALTH_STATE_DIR
    value: "/var/run/gpu-health"
  - name: GPU_HEALTH_BASELINE_DIR
    value: "/etc/gpu-health/baseline"
```

Baseline ConfigMap (one key per GPU serial):
```yaml
apiVersion: v1
kind: ConfigMap
metadata:
  name: gpu-baselines
data:
  "1321021036987": |
    serial=1321021036987
    uuid=GPU-abc123
    driver_version=535.104.05
    perf_w_mean=0.874878
    established_at=2026-03-15T14:22:00Z
    workload=cublas_bf16_gemm_n8192
    sample_count=270
```

Mount at `/etc/gpu-health/baseline/`. The exporter reloads baseline files
on change via inotify — no restart required.

---

## HTTP endpoints

| Endpoint | Response |
|---|---|
| `GET /metrics` | Prometheus text format 0.0.4 |
| `GET /ready` | 200 after first successful poll on all GPUs; 503 before |
| `GET /live` | 200 while poll loop is running; 503 if last poll age > 3× poll interval |

---

## Metrics reference

All per-GPU metrics carry a single `serial` label. Static metadata lives
in `gpu_info{...}` and is joined in PromQL when needed. This avoids
cardinality bloat and series discontinuity when metadata changes.

### Health score

| Metric | Type | Description |
|---|---|---|
| `gpu_health_score` | Gauge | 0–100. N/A class when `telemetry_ok=0` |
| `gpu_health_class` | Gauge | 0=N/A 1=Healthy 2=Monitor 3=Degrading 4=Decommission |
| `gpu_telemetry_ok` | Gauge | 1 if completeness gate passed and score is valid |

### Identity

| Metric | Type | Description |
|---|---|---|
| `gpu_info` | Gauge | Always 1. Labels: serial, uuid, model, driver, index, pcie_gen_max, pcie_width_max |
| `gpu_identity_source` | Gauge | 0=serial number, 1=UUID fallback (serial unavailable) |
| `gpu_present` | Gauge | 1 if GPU is responding; 0 if lost or error threshold exceeded |

### Windowed statistics (over scoring window)

| Metric | Type | Description |
|---|---|---|
| `gpu_temp_p95_celsius` | Gauge | GPU die temperature p95 |
| `gpu_hbm_temp_p95_celsius` | Gauge | HBM temperature p95 |
| `gpu_sm_clock_std_mhz` | Gauge | SM clock standard deviation |
| `gpu_power_saturation_ratio` | Gauge | Fraction of window where power ≥ 98% of limit |
| `gpu_ecc_sbe_rate_per_hour` | Gauge | ECC SBE rate (errors/hour) |
| `gpu_ecc_dbe_in_window` | Gauge | 1 if any DBE delta observed in window |

### Thermal

| Metric | Type | Description |
|---|---|---|
| `gpu_temp_celsius` | Gauge | Current GPU die temperature |
| `gpu_hbm_temp_celsius` | Gauge | Current HBM temperature |
| `gpu_fan_speed_ratio` | Gauge | Fan speed 0.0–1.0; **absent for liquid-cooled GPUs** |

### Power

| Metric | Type | Description |
|---|---|---|
| `gpu_power_watts` | Gauge | Current GPU power draw |
| `gpu_power_limit_watts` | Gauge | Current power management limit |
| `gpu_board_power_watts` | Gauge | Total board power (DCGM; NaN = DCGM anomaly) |
| `gpu_energy_joules_total` | Counter | Lifetime energy consumed (DCGM; NaN = DCGM anomaly) |
| `gpu_power_violation_microseconds_total` | Counter | Cumulative power throttle time |
| `gpu_thermal_violation_microseconds_total` | Counter | Cumulative thermal throttle time |
| `gpu_throttle_sw_power_cap` | Gauge | 1 if SW power cap throttle active |
| `gpu_throttle_hw_slowdown` | Gauge | 1 if HW thermal slowdown active |
| `gpu_throttle_hw_power_brake` | Gauge | 1 if HW power brake active |
| `gpu_throttle_sw_thermal` | Gauge | 1 if SW thermal throttle active |
| `gpu_throttle_hw_thermal` | Gauge | 1 if HW thermal throttle active |

### Memory

| Metric | Type | Description |
|---|---|---|
| `gpu_memory_used_bytes` | Gauge | Memory currently in use |
| `gpu_memory_free_bytes` | Gauge | Memory free |
| `gpu_memory_total_bytes` | Gauge | Total memory capacity |
| `gpu_memory_bandwidth_utilization_ratio` | Gauge | Memory BW utilization 0.0–1.0 (DCGM; NaN = DCGM anomaly) |
| `gpu_ecc_sbe_aggregate_total` | Counter | Lifetime SBE count (does not reset on driver reload) |
| `gpu_ecc_dbe_aggregate_total` | Counter | Lifetime DBE count |
| `gpu_retired_pages_sbe` | Gauge | Pages retired due to SBE |
| `gpu_retired_pages_dbe` | Gauge | Pages retired due to DBE |
| `gpu_row_remap_failures` | Gauge | Row remap failures; any non-zero = irreversible HBM damage |
| `gpu_pending_row_remap` | Gauge | 1 if remap pending (reboot required) |

### Interconnects

| Metric | Type | Description |
|---|---|---|
| `gpu_pcie_link_gen` | Gauge | Current PCIe link generation |
| `gpu_pcie_link_width` | Gauge | Current PCIe lane count |
| `gpu_pcie_replay_total` | Counter | PCIe replay counter |
| `gpu_nvlink_replay_total` | Counter | NVLink replay errors |
| `gpu_nvlink_recovery_total` | Counter | NVLink recovery errors |
| `gpu_nvlink_crc_total` | Counter | NVLink CRC flit errors |
| `gpu_xid_errors_total` | Counter | Total XID errors |
| `gpu_xid_last_code` | Gauge | Most recent XID error code (0 if none) |

### Compute

| Metric | Type | Description |
|---|---|---|
| `gpu_sm_clock_mhz` | Gauge | Current SM clock frequency |
| `gpu_mem_clock_mhz` | Gauge | Current memory clock frequency |
| `gpu_utilization_gpu_ratio` | Gauge | GPU compute utilization 0.0–1.0 |
| `gpu_utilization_memory_ratio` | Gauge | Memory controller utilization 0.0–1.0 |
| `gpu_pstate` | Gauge | Performance state (0=P0=maximum, 15=minimum) |

### Baseline and probe

| Metric | Type | Description |
|---|---|---|
| `gpu_baseline_available` | Gauge | 1 if baseline file present and parseable |
| `gpu_baseline_valid` | Gauge | 1 if baseline passed all validation checks |
| `gpu_baseline_serial_mismatch` | Gauge | 1 if baseline serial does not match device |
| `gpu_baseline_driver_mismatch` | Gauge | 1 if driver version changed since baseline |
| `gpu_baseline_age_seconds` | Gauge | Seconds since baseline was established |
| `gpu_probe_available` | Gauge | 1 if probe result file present and parseable |
| `gpu_probe_result_stale` | Gauge | 1 if probe result older than configured TTL |
| `gpu_perf_drop_ratio` | Gauge | Perf/W drop vs baseline (NaN if no baseline or probe) |

### Exporter self-health

| Metric | Type | Description |
|---|---|---|
| `gpu_health_exporter_info` | Gauge | Always 1. Label: version |
| `gpu_dcgm_available` | Gauge | 1 if DCGM is connected and responding |
| `gpu_health_last_poll_timestamp` | Gauge | Unix timestamp of last successful poll |

---

## Scoring model

Score starts at 100. Penalties are applied every poll cycle over the
300-second scoring window (configurable via `window_s`).

| Condition | Penalty | Notes |
|---|---|---|
| GPU temp p95 > 80°C | −10 | |
| GPU temp p95 > 90°C | −25 | Replaces −10 |
| HBM temp p95 > 85°C | −10 | |
| HBM temp p95 > 95°C | −25 | Replaces −10 |
| SM clock std dev > 120 MHz | up to −15 | Scaled, capped |
| Power ≥ 98% of limit (fraction of window) | up to −3 | |
| ECC SBE rate > 100/hr | −5 | |
| ECC DBE any in window | −25 | Active corruption |
| Perf/W drop > 3% vs baseline | −5 | Probe result |
| Perf/W drop > 7% vs baseline | −15 | Probe result |
| Perf/W drop > 12% vs baseline | −30 | Probe result |
| Retired pages (DBE cause) ≥ 1 | −5 | |
| Retired pages (DBE cause) ≥ 10 | −15 | Replaces −5 |
| Row remap failures > 0 | −25 | Irreversible |
| PCIe link gen degraded vs max | −10 | |
| PCIe link width degraded vs max | −15 | Lane loss |

All thresholds are configurable. The binary encodes the penalty structure;
the config file encodes the threshold values. Threshold changes take effect
on the next startup without a binary rollout.

**Classification:**

| Score | Class |
|---|---|
| ≥ 85 | Healthy |
| ≥ 70 | Monitor |
| ≥ 50 | Degrading |
| < 50 | Decommission Candidate |
| — | N/A (telemetry incomplete) |

**Telemetry completeness gate:** if fewer than `min_sample_ratio` of
expected samples are present in the window, or the median inter-sample gap
exceeds `max_median_step_s`, the score is withheld and `gpu_telemetry_ok`
is set to 0. This prevents a misleading Healthy result during startup or
after a poll outage.

---

## Baseline provisioning

A baseline captures a GPU's reference performance/Watt at the time it is
commissioned. Without a baseline, the perf/W drift component is excluded
from scoring and `gpu_perf_drop_ratio` emits `NaN`.

Baseline format (one file per GPU serial, filename = serial number):
```
serial=1321021036987
uuid=GPU-abc123
driver_version=535.104.05
perf_w_mean=0.874878
established_at=2026-03-15T14:22:00Z
workload=cublas_bf16_gemm_n8192
sample_count=270
```

**Bare metal:** place in `baseline_dir` (default `/etc/gpu-health/baseline/`).
The exporter reloads on change via inotify — no restart needed.

**Kubernetes:** mount a ConfigMap at the same path. Kubelet propagates
ConfigMap updates; inotify picks up the change.

Baselines are produced by the companion `gpu_health_probe` binary (Phase 2,
cuBLAS BF16 GEMM). The exporter exposes `gpu_baseline_available` and
`gpu_baseline_age_seconds` for pipeline consumption.

---

## State files

The exporter writes a state file for each GPU every `state_write_interval_s`
seconds (default 30s). These are local fallback — readable without Prometheus
during incident response.

Location: `state_dir/{serial}.state` (default `/var/run/gpu-health/`)

```
serial=1321021036987
score=87.3
classification=Healthy
ts_utc=2026-04-02T14:22:00Z
temp_p95=72.3
clk_std=45.2
power_mean=650.1
power_limit=700.0
telemetry_ok=1
baseline_available=1
probe_result_age_s=3600
reasons=
```

Probe results (written by the probe binary) live alongside:
`state_dir/{serial}.probe`

---

## GPU identity

The primary label on all metrics is the GPU **serial number**
(`nvmlDeviceGetSerial`). Serial numbers are burned in at manufacturing
and survive infoROM reflash. UUIDs are stored in infoROM and can change
after firmware events or RMA procedures — they are an unreliable anchor
for long-term health and financial tracking.

If `nvmlDeviceGetSerial` fails, the UUID is used as a fallback and
`gpu_identity_source` is set to 1. The UUID is always retained in
`gpu_info` labels for cross-referencing with other tooling.

---

## Architecture notes

- No hard link on NVML or DCGM. Both are loaded via `dlopen` at runtime.
- Fork at startup: HTTP child holds no GPU handles and runs with dropped
  capabilities. Parent respawns the child if it exits.
- IPC: fixed-size `gpu_snapshot_t` structs over a Unix socketpair. No
  protocol parsing, no dynamic allocation in the hot path.
- Ring buffer: array of structs, one per poll timestep, capacity =
  `window_s / poll_interval_s`. Statistical computation (p95, stddev,
  delta/rate) runs over this buffer. HTTP child never touches it.
- inotify-based baseline hot-reload. Threshold changes require a restart;
  baseline data changes do not.
- Logs always to stderr — compatible with both journald and container
  runtime log capture.
