# GPU Health Exporter — Design Document
# Checkpoint: Pre-Implementation

This document captures the full design state before any C code is written.
It serves as a fork point — any future conversation, branch, or contributor
starts here with full context.

---

## 1. What We Are Building

A production-grade GPU health monitoring exporter written in C, deployable at
AI factory scale (thousands of multi-GPU hosts). Supports both bare metal and
Kubernetes. Replaces the Python prototype (gpu-health-index v0).

Dual purpose:
1. Operational monitoring — detect degradation, trigger alerts, inform
   decommissioning decisions in production AI factories.
2. Financial assessment — provide auditable, reproducible health signals that
   feed secondary market valuation and collateral assessment models for GPU
   lending/leasing businesses.

The financial use case does not change the exporter internals. It changes what
is built on top of the exporter's outputs. The exporter is Phase 1. Financial
modelling, assessment reports, and attestation integration are Phase 2.

---

## 2. Core Design Decisions

### 2.1 Process Model

One process per node. One poll thread per GPU.

Rationale: supports both bare metal (one systemd unit) and Kubernetes
(one DaemonSet pod per node) without deployment model conflict. Blast radius
protection comes from per-GPU thread isolation with NVML call watchdog timeouts
— not process separation. A wedged GPU can only stale its own metrics; other
GPU poll threads are unaffected.

Each GPU has its own:
- Ring buffer (array of structs, 300 samples)
- Snapshot struct
- Mutex

### 2.2 Security Model

Privileged parent + unprivileged child via fork() at startup.

- Socketpair created before fork — no filesystem presence, no named resource
- Parent: NVML/DCGM poll threads, drops to minimum capabilities after fork,
  never calls socket/bind/listen/accept
- Child: serves HTTP /metrics, drops all capabilities, seccomp whitelist
  (read, write, accept, close, send, recv, exit only), no /dev/nvidia* access
- IPC: fixed-size gpu_snapshot_t written directly over socketpair —
  no protocol parsing, no dynamic allocation
- Lifecycle: parent monitors child via waitpid(), respawns on crash.
  Parent crash causes child EOF exit. systemd restarts the unit.
- One systemd unit, one binary
- Optional TLS via mbedTLS (WITH_TLS=1 compile flag) in child only
- Management network trusted for isolation, but process isolation does not
  rely on it

### 2.3 GPU Identity

Primary identifier: serial number (nvmlDeviceGetSerial()).

Serial number is burned into hardware at manufacturing time. It survives
infoROM reflash. UUID is stored in infoROM and can change on the same physical
board after firmware events or RMA procedures — wrong anchor for long-term
health and financial tracking.

Fallback hierarchy: serial → UUID → never index.
Identity source exposed as a metric (gpu_identity_source).
UUID retained in gpu_info gauge for cross-referencing with existing tooling.

### 2.4 Telemetry Sources

NVML: required. Loaded via dlopen at runtime — no hard link dependency.
DCGM: optional. Loaded via dlopen at runtime. Detected at startup, graceful
degradation to NVML-only if unavailable. DCGM daemon assumed to be deployed
on target fleet.

All NVML and DCGM calls go through a vtable (function pointer struct).
In production: vtable populated via dlopen/dlsym.
In tests: vtable populated with fake functions — no stub .so required.

### 2.5 Ring Buffer

Array of structs. One struct per timestep containing all signals for that
sample. Temporal consistency is structurally guaranteed — all signals for a
timestep travel together.

Only windowed signals live here — signals where statistical computation over
time (p95, std dev, delta/rate) is meaningful. Point-in-time signals live in
the current state struct (see 2.5b).

```c
typedef struct {
    /* Powered */
    double   power_w;
    double   power_limit_w;

    /* Cooled / Thermal */
    double   temp_c;
    double   hbm_temp_c;

    /* Computing */
    double   sm_clock_mhz;

    /* Memory — volatile counters (ring buffer computes delta for rate) */
    uint64_t ecc_sbe_volatile;   /* running count — delta = rate over window */
    uint64_t ecc_dbe_volatile;   /* running count — delta = active events    */

    /* Timestamp */
    uint64_t timestamp_ms;
} gpu_sample_t;

typedef struct {
    gpu_sample_t *samples;       /* heap-allocated, count = window_s / poll_interval_s */
    int           head;
    int           count;
    int           capacity;
} gpu_ring_t;
```

### 2.5b Current State Struct

Point-in-time signals that do not benefit from windowing. Checked against
thresholds on their latest value. Updated every poll cycle alongside the
ring buffer write.

```c
typedef struct {
    /* Memory — irreversible / lifetime signals */
    uint64_t ecc_sbe_aggregate;      /* lifetime total, never resets           */
    uint64_t ecc_dbe_aggregate;      /* lifetime total — feeds financial record */
    uint32_t retired_pages_sbe;      /* pages permanently retired (SBE cause)  */
    uint32_t retired_pages_dbe;      /* pages permanently retired (DBE cause)  */
    uint32_t row_remap_failures;     /* HBM row remap failures — any > 0 serious */
    int      pending_row_remap;      /* boolean — reboot needed to apply remap  */

    /* Memory — capacity */
    uint64_t mem_used_bytes;
    uint64_t mem_free_bytes;
    uint64_t mem_total_bytes;
    double   mem_bw_util_pct;        /* DCGM — context signal                  */

    /* Communicating */
    int      pcie_link_gen;          /* current — compare vs pcie_link_gen_max */
    int      pcie_link_gen_max;      /* collected at startup                   */
    int      pcie_link_width;        /* current — compare vs pcie_link_width_max */
    int      pcie_link_width_max;    /* collected at startup                   */
    uint64_t pcie_replay_count;      /* counter — rate tracked via delta       */
    uint64_t nvlink_replay_count;    /* counter — aggregate across links       */
    uint64_t nvlink_recovery_count;
    uint64_t nvlink_crc_count;
    uint64_t xid_count;              /* total XID errors — delta since last check */
    uint32_t xid_last_code;          /* most recent XID event code             */

    /* Powered — current state */
    double   board_power_w;          /* total board power including HBM, VRMs  */
    double   energy_j;               /* lifetime counter                       */
    uint64_t power_violation_us;     /* cumulative time power throttled        */
    uint64_t thermal_violation_us;   /* cumulative time thermally throttled    */

    /* Throttle reasons — individual booleans, not bitmask */
    int      throttle_sw_power_cap;
    int      throttle_hw_slowdown;
    int      throttle_hw_power_brake;
    int      throttle_sw_thermal;
    int      throttle_hw_thermal;

    /* Computing */
    double   mem_clock_mhz;
    int      util_gpu_pct;
    int      util_mem_pct;
    int      pstate;                 /* P0 = full performance */

    /* Fan */
    int      fan_speed_pct;          /* -1 if not available (SXM liquid cooled) */
} gpu_state_t;
```

### 2.6 Scoring

Edge scoring — computed inline in the exporter poll thread every cycle.
Thresholds in a config file (key=value) — threshold changes never require a
binary rollout.

Telemetry completeness gate: if fewer than minimum samples in window, or
median sample step exceeds threshold, score returns N/A rather than a
misleading Healthy result.

Score recomputed on every poll cycle (cheap arithmetic, always current).
Mutex-protected snapshot handed to child via socketpair. HTTP child never
touches the ring buffer.

Classification:
- >= 85: Healthy
- >= 70: Monitor
- >= 50: Degrading
- <  50: Decommission Candidate
- N/A:   Incomplete Telemetry

### 2.7 Metric Labels

Serial number is the only label on all hot metrics.
All metadata in gpu_info{...} 1 gauge, joined in PromQL when needed.
Avoids cardinality bloat and series discontinuity.

Throttle reasons exposed as individual boolean gauges — not a bitmask.
XID errors exposed as a counter + last XID code as a gauge.

### 2.8 Baseline

Format: structured key=value (not bare float). Self-describing.

```
serial=1321021036987
uuid=GPU-abc123
driver_version=535.104.05
perf_w_mean=0.874878
established_at=2026-03-15T14:22:00Z
workload=cublas_bf16_gemm_n8192
sample_count=270
```

Validation on load:
- serial mismatch: hard fail, emit gpu_baseline_serial_mismatch 1
- driver mismatch: soft warn, emit gpu_baseline_driver_mismatch 1
- value out of sane range: hard fail, emit gpu_baseline_valid 0
- TTL exceeded: soft warn, emit gpu_baseline_age_seconds

Storage:
- Bare metal: /etc/gpu-health/baseline/{serial}
- Kubernetes: ConfigMap (not Secret — not sensitive data), mounted read-only
  at /etc/gpu-health/baseline/, one key per GPU serial
- Same format, same path, same code path on both platforms
- inotify-based reload on change — no restart required

Provisioning: post-deployment playbook. No provisioning pipeline yet.
Interface designed to be pipeline-ready. Exporter exposes gpu_baseline_available
and gpu_baseline_age_seconds for future pipeline consumption.

### 2.9 Active Probe

Separate binary. Not part of the exporter. Language: CUDA C.
Workload: BF16 GEMM via cuBLAS. Single phase.

Rationale for BF16 GEMM:
- Exercises tensor cores — the primary AI compute path on A100/H100/H200
- cuBLAS is present on any CUDA-capable system, no additional dependency
- GPU Burn uses FP64 — wrong hardware path for AI factory validation
- HBM degradation detected by passive ECC/remap monitoring, not the probe
- Silent compute errors detected by passive ECC DBE monitoring

Probe output: state file written atomically (write to .tmp, rename).
Exporter reads state file. Probe result has a configurable TTL.
On expiry: perf/W component silently dropped from score,
gpu_probe_result_stale gauge emitted.

Probe scheduling:
- Bare metal: systemd timer
- Kubernetes: unsolved — documented limitation. K8s CronJobs compete for
  GPU resources. Bare metal systemd timer is the clean path.

### 2.10 Deployment

Bare metal:
- One systemd unit per node
- Port 9108
- Prometheus file_sd, one entry per node written by exporter at startup
- sd_notify READY=1 after first successful poll on all GPUs

Kubernetes:
- DaemonSet, one pod per node
- Port 9108
- Prometheus ServiceMonitor
- ConfigMap for baseline
- env var overrides for all config values (env takes precedence over file)
- /ready and /live HTTP endpoints for kubelet probes
- Logs always to stderr (captured by both journald and container runtime)
- sd_notify skipped — detected at runtime via NOTIFY_SOCKET env var

Build:
- Mostly-static binary (dlopen'd NVML and DCGM are the only dynamic deps)
- Makefile with WITH_TLS=1 flag for optional mbedTLS
- Target: Ampere (A100) onwards. Blackwell supported naturally (NVML additive).
- Single golden image compilation

Deployment artifacts:
- systemd unit (bare metal)
- DaemonSet + ConfigMap + ServiceMonitor + RBAC manifests (Kubernetes)
- Helm chart

### 2.11 HTTP / Metrics Server

Two threads: poll thread (parent process) and HTTP thread (child process).
HTTP thread serves last-known-good snapshot — never blocks on NVML.
SO_SNDTIMEO set on accepted sockets to bound slow-client backpressure.
Some backpressure acceptable; full thread pool not justified for this workload.
/metrics — Prometheus text format
/ready   — 200 after first successful poll, 503 before
/live    — 200 while poll loop is running, 503 if stale beyond threshold

---

## 3. Signal Inventory

### 3.1 Powered

| Signal | Source | Type |
|--------|--------|------|
| GPU power draw (W) | NVML / DCGM | Gauge |
| Total board power (W) | DCGM | Gauge |
| Power management limit (W) | NVML / DCGM | Gauge |
| Power violation time (µs) | DCGM | Counter |
| Energy consumed (J) | DCGM | Counter |
| Throttle: SW_POWER_CAP | NVML | Gauge (bool) |
| Throttle: HW_SLOWDOWN | NVML | Gauge (bool) |
| Throttle: HW_POWER_BRAKE | NVML | Gauge (bool) |
| Throttle: SW_THERMAL | NVML | Gauge (bool) |
| Throttle: HW_THERMAL | NVML | Gauge (bool) |

VRM health observed indirectly via HW_SLOWDOWN and power violation time.
Direct rail voltage not reliably accessible via NVML/DCGM.

### 3.2 Fed (Memory)

| Signal | Source | Type |
|--------|--------|------|
| Memory used (bytes) | NVML / DCGM | Gauge |
| Memory free (bytes) | NVML / DCGM | Gauge |
| Memory total (bytes) | NVML / DCGM | Gauge |
| ECC SBE volatile count | NVML / DCGM | Counter |
| ECC DBE volatile count | NVML / DCGM | Counter |
| ECC SBE aggregate count | NVML / DCGM | Counter |
| ECC DBE aggregate count | NVML / DCGM | Counter |
| Retired pages (SBE cause) | NVML | Gauge |
| Retired pages (DBE cause) | NVML | Gauge |
| Row remap failure count | NVML / DCGM | Gauge |
| Pending row remap | NVML | Gauge (bool) |
| HBM temperature (C) | NVML | Gauge |
| Memory bandwidth utilisation (%) | DCGM | Gauge |

### 3.3 Communicating

| Signal | Source | Type |
|--------|--------|------|
| PCIe current link gen | NVML | Gauge |
| PCIe current link width | NVML | Gauge |
| PCIe replay counter | NVML / DCGM | Counter |
| PCIe recovery counter | DCGM | Counter |
| NVLink replay error count | DCGM | Counter |
| NVLink recovery error count | DCGM | Counter |
| NVLink CRC flit error count | DCGM | Counter |
| XID error counter (total) | DCGM | Counter |
| Last XID error code | DCGM | Gauge |

PCIe max link gen and max link width collected once at startup → gpu_info.

### 3.4 Cooled

| Signal | Source | Type |
|--------|--------|------|
| GPU die temperature (C) | NVML / DCGM | Gauge |
| HBM temperature (C) | NVML | Gauge (shared with Fed) |
| Fan speed (%) | NVML | Gauge (skip if unavailable) |
| Thermal violation time (µs) | DCGM | Counter |

### 3.5 Computing

| Signal | Source | Type |
|--------|--------|------|
| SM clock (MHz) | NVML / DCGM | Gauge |
| Memory clock (MHz) | NVML / DCGM | Gauge |
| GPU utilisation (%) | NVML / DCGM | Gauge |
| Performance state (P-state) | NVML | Gauge |

### 3.6 Exporter Self-Health

| Signal | Type | Purpose |
|--------|------|---------|
| gpu_health_exporter_info | Gauge | Version, build info |
| gpu_identity_source | Gauge | serial or uuid_fallback |
| gpu_baseline_available | Gauge (bool) | Baseline file present and valid |
| gpu_baseline_valid | Gauge (bool) | Passed validation |
| gpu_baseline_age_seconds | Gauge | Time since baseline established |
| gpu_baseline_driver_mismatch | Gauge (bool) | Driver changed since baseline |
| gpu_probe_result_stale | Gauge (bool) | Probe result older than TTL |
| gpu_health_last_poll_timestamp | Gauge | Epoch of last successful poll |
| gpu_health_collector_errors_total | Counter | By error kind label |
| gpu_telemetry_ok | Gauge (bool) | Completeness gate passed |
| gpu_dcgm_available | Gauge (bool) | DCGM connected and responding |

---

## 4. Scoring Model

Score starts at 100. Penalties applied for envelope violations, stability
violations, and baseline-relative drift.

The scorer has two distinct input paths:

**Ring buffer path** — statistical computation over 300s window:
- power_w         → saturation fraction
- temp_c          → p95
- hbm_temp_c      → p95
- sm_clock_mhz    → standard deviation
- ecc_sbe_vol     → delta over window (rate)
- ecc_dbe_vol     → delta over window (active events)

**Current state path** — threshold check on latest value:
- retired_pages_dbe    → absolute count threshold
- retired_pages_sbe    → absolute count threshold
- row_remap_failures   → any non-zero
- pending_row_remap    → boolean
- pcie_link_gen        → vs max (from gpu_info)
- pcie_link_width      → vs max (from gpu_info)
- xid_count            → delta since last check
- throttle_reasons     → current booleans

Total score = 100 − windowed penalties − current-state penalties

ECC DBE aggregate count (lifetime, never resets) collected every poll but
feeds the lifetime degradation record (Phase 2 / financial), not the
operational score directly. It IS exposed as a metric for Prometheus alerting.

| Condition | Penalty | Path | Notes |
|-----------|---------|------|-------|
| GPU temp p95 > 80°C | -10 | Ring buffer | |
| GPU temp p95 > 90°C | -25 | Ring buffer | Replaces -10 |
| HBM temp p95 > 85°C | -10 | Ring buffer | |
| HBM temp p95 > 95°C | -25 | Ring buffer | Replaces -10 |
| SM clock std > 120 MHz | up to -15 | Ring buffer | Capped, scaled |
| Power saturation >= 98% of limit | up to -3 | Ring buffer | Fraction of window |
| ECC SBE rate > 100/hr | -5 | Ring buffer | Delta over window |
| ECC DBE any in window | -25 | Ring buffer | Active corruption event |
| Perf/W drop > 3% vs baseline | -5 | — | Probe result |
| Perf/W drop > 7% vs baseline | -15 | — | Probe result |
| Perf/W drop > 12% vs baseline | -30 | — | Probe result |
| Retired pages (DBE) >= 1 | -5 | Current state | |
| Retired pages (DBE) >= 10 | -15 | Current state | Replaces -5 |
| Row remap failures > 0 | -25 | Current state | HBM physically degraded |
| PCIe link gen degraded | -10 | Current state | vs max at startup |
| PCIe link width degraded | -15 | Current state | Lane loss |

All thresholds configurable via config file. Binary encodes penalty structure,
config encodes threshold values.

---

## 5. What Is Still To Define (Pre-Implementation)

These are decisions, not just content. Work through before writing code.

### 5.1 State File Protocol ✓ DECIDED

Two files, single writer each, both key=value, both atomic write (tmp → rename).

**{serial}.probe** — probe owns, written after each run:
```
serial=1321021036987
uuid=GPU-abc123
perf_w_mean=0.874878
probe_timestamp=2026-03-15T14:22:00Z
probe_exit_code=0
driver_version=535.104.05
workload=cublas_bf16_gemm_n8192
sample_count=270
probe_duration_s=330
```

**{serial}.state** — exporter owns, written every ~30s (not every poll):
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
reasons=Power headroom: 0.0% samples >= 98.0% of limit
```

Location: /var/run/gpu-health/ (configurable)
Rationale: local fallback for incident inspection without Prometheus,
out-of-band audit path for financial reporting tools, no coordination
complexity — single writer per file.
Kubernetes: .probe file is best-effort (probe scheduling unsolved on K8s).
Missing file treated as no probe result — graceful degradation.

### 5.2 Config File Format ✓ DECIDED

Format: key=value, one per line, # comments, env var overrides with
GPU_HEALTH_ prefix (env takes precedence over file, file over compiled
defaults). Validation at startup — hard fail on invalid values, log reason.

```
# Paths
state_dir                   = /var/run/gpu-health
baseline_dir                = /etc/gpu-health/baseline
listen_addr                 = 0.0.0.0
listen_port                 = 9108

# Polling
poll_interval_s             = 1
window_s                    = 300
state_write_interval_s      = 30

# Telemetry completeness gate
min_sample_ratio            = 0.80
max_median_step_s           = 2.5
min_samples_absolute        = 10

# Scoring — thermal
temp_p95_warn_c             = 80
temp_p95_bad_c              = 90
hbm_temp_p95_warn_c         = 85
hbm_temp_p95_bad_c          = 95

# Scoring — clocks
clk_std_warn_mhz            = 120

# Scoring — power
power_high_ratio            = 0.98
power_penalty_max           = 3.0

# Scoring — memory
ecc_sbe_rate_warn_per_hour  = 100
ecc_sbe_penalty             = 5.0
ecc_dbe_penalty             = 25.0
retired_pages_warn          = 1
retired_pages_bad           = 10
retired_pages_pen_warn      = 5.0
retired_pages_pen_bad       = 15.0
row_remap_failure_penalty   = 25.0

# Scoring — PCIe
pcie_link_degraded_penalty  = 10.0
pcie_width_degraded_penalty = 15.0

# Scoring — perf/W drift
perf_drop_warn              = 0.03
perf_drop_bad               = 0.07
perf_drop_severe            = 0.12
perf_drop_pen_warn          = 5.0
perf_drop_pen_bad           = 15.0
perf_drop_pen_severe        = 30.0

# Probe result — TTL is 1.5x probe_interval_s by design
probe_interval_s            = 86400
probe_ttl_s                 = 129600

# NVML / DCGM — refine thresholds in field
nvml_timeout_ms             = 5000
nvml_error_threshold        = 10
nvml_hard_error_threshold   = 1
nvml_retry_interval_s       = 60
dcgm_timeout_ms             = 5000
dcgm_error_threshold        = 10
dcgm_retry_interval_s       = 60

# TLS (only active with WITH_TLS=1 build)
tls_cert_path               =
tls_key_path                =
```

### 5.3 Error Backoff on NVML Failures ✓ DECIDED

Two thresholds, not one flat counter:
- nvml_error_threshold = 10: consecutive any-errors → mark GPU unavailable
- nvml_hard_error_threshold = 1: consecutive NVML_ERROR_GPU_IS_LOST or
  NVML_ERROR_DRIVER_NOT_LOADED → mark unavailable immediately
- nvml_retry_interval_s = 60: retry cadence once marked unavailable
  (no point hammering a wedged driver at 1Hz)
- Same thresholds mirrored for DCGM
- All values configurable, refine in field

### 5.4 Probe Result TTL ✓ DECIDED

- probe_ttl_s = 129600 (36 hours) — 1.5× default probe_interval_s (24h)
- On expiry: perf/W component silently dropped from score
- gpu_probe_result_stale gauge emitted on expiry
- Rationale: TTL must comfortably outlast probe interval with margin —
  one missed probe should not immediately drop the perf/W signal

### 5.5 Startup Sequence ✓ DECIDED

```
1.  Parse config → hard fail if invalid
2.  Validate directories → hard fail if missing/unwritable
3.  dlopen NVML → nvmlInit_v2() → hard fail if fails
4.  dlopen DCGM → attempt connection → mark available/unavailable (soft)
5.  nvmlDeviceGetCount() → hard fail if 0
6.  For each GPU i:
      get handle, serial, allocate ring buffer + snapshot + mutex
      read baseline file → validate → store result
      read probe state file → validate TTL → store result
      set gpu_ready[i] = 0
      spawn poll_thread(i)
7.  Create socketpair(parent_fd, child_fd)
8.  fork()
      child:
        close parent_fd
        close all NVML handles and library fd
        drop capabilities
        install seccomp filter
        bind listen_port → hard fail if port in use
        enter HTTP accept loop
        return 503 from /ready until first snapshot received
      parent:
        close child_fd
        drop capabilities to minimum
9.  Parent: wait for all gpu_ready[i] == 1
            OR (nvml_timeout_ms * 2) ms elapsed
10. Parent: log any GPUs that did not complete first poll
11. Parent: sd_notify READY=1 (only if NOTIFY_SOCKET env var is set)
12. Parent: enter steady-state monitor loop (poll threads already running)
            waitpid() for child → respawn if exits
```

Key properties:
- systemd never times out on healthy deployment (TimeoutStartSec = 25s default)
- Wedged GPU cannot block startup forever (global_startup_timeout releases READY)
- /metrics never serves unknown state (/ready returns 503 until first snapshot)
- GPU that fails first poll is in known state (gpu_ready set on failure too)
- Port binding happens before READY fires
- NVML handles explicitly closed in child after fork — not accessible from
  unprivileged HTTP process

---

## 6. Next Steps — Spec Content

All pre-implementation decisions are complete. Section 5 is fully decided.
The following are writing tasks, not design decisions. They follow directly
from everything above and get produced as the spec is written.

### 6.1 File and Directory Layout

```
gpu-health-exporter/
├── src/
│   ├── main.c          — arg parsing, startup sequence, fork, signal handling
│   ├── nvml.c/h        — vtable definition, dlopen population, per-call wrappers
│   ├── dcgm.c/h        — vtable definition, dlopen population, graceful fallback
│   ├── collector.c/h   — per-GPU poll thread, ring buffer writes, state updates
│   ├── scorer.c/h      — pure arithmetic, no I/O, two-path scoring model
│   ├── snapshot.c/h    — mutex-protected snapshot, socketpair handoff
│   ├── http.c/h        — HTTP server (child), /metrics /ready /live
│   ├── config.c/h      — key=value parser, env var override, validation
│   ├── state.c/h       — baseline read/validate, probe state read, inotify reload
│   ├── procpriv.c/h    — capability drop, seccomp filter installation
│   └── util.c/h        — logging, time helpers, safe string ops
├── tests/
│   ├── test_scorer.c   — unit tests, fake vtable, no hardware required
│   ├── test_config.c
│   ├── test_state.c
│   └── test_ring.c
├── deploy/
│   ├── gpu-health.service          — systemd unit (bare metal)
│   ├── gpu-health.conf.example     — annotated config example
│   └── k8s/
│       ├── daemonset.yaml
│       ├── configmap-baseline.yaml
│       ├── servicemonitor.yaml
│       ├── rbac.yaml
│       └── Chart.yaml              — Helm chart root
├── probe/
│   ├── gpu_health_probe.cu         — cuBLAS BF16 GEMM probe (CUDA C)
│   └── Makefile
└── Makefile                        — WITH_TLS=1, DEBUG=1, test target

```

### 6.2 Complete Metric Names

To be written in spec — naming convention: `gpu_` prefix, snake_case,
units in name (e.g. `gpu_temp_celsius`, `gpu_power_watts`), counters
suffixed `_total`. Every metric gets HELP and TYPE lines.

### 6.3 Snapshot Struct

The struct written over the socketpair. Fixed size. Contains:
- All computed score fields (score, classification, reasons bitmask)
- Latest raw values from gpu_state_t (for /metrics formatting)
- Ring buffer statistics (p95 temp, clk std, power saturation fraction)
- Timestamp of last successful poll
- Identity fields (serial, uuid, gpu_index)

### 6.4 Build System

Makefile targets: all, test, clean, install
Flags: WITH_TLS=1 (links mbedTLS), DEBUG=1 (sanitizers + debug symbols)
Static linking for everything except libdl (required for dlopen)

---

## 7. Phase 2 — Financial Layer (Future, Not Blocking Phase 1)

These do not change the exporter. They are consumers of its outputs.

- Assessment report: structured JSON + human-readable format produced by
  probe run. Contains full signal snapshot, scoring breakdown, irreversible
  degradation summary, probe methodology parameters, timestamp.
- Lifetime degradation record: accumulating irreversible signals (ECC aggregate,
  retired pages, row remap failures) tracked over GPU's monitored lifetime.
  Separate from rolling window metrics. Updated each probe run.
- NVIDIA attestation: cryptographic proof of GPU firmware genuineness for
  counterparty trust in secondary market transactions. H100/H200 via NVIDIA
  Confidential Computing.
- Financial model inputs: health score trend, lifetime ECC, retired pages,
  row remap history, perf/W retention, age in service, GPU model.
  Exporter produces all of these. Financial model is a separate system.

---

## 8. FMEA Summary

Key failure modes and mitigations designed in:

| Failure | Mitigation |
|---------|-----------|
| NVML call hang | Watchdog timeout per poll thread |
| DCGM unavailable | Runtime detection, graceful NVML-only fallback |
| GPU disappears mid-run | Per-poll error handling, gpu_present 0, no exit |
| HTTP blocks on slow client | SO_SNDTIMEO bounds wait |
| Baseline file corrupt | Validation on load, fall back to no-baseline mode |
| Baseline serial mismatch | Hard fail, explicit metric |
| Probe result stale | TTL check, gpu_probe_result_stale metric |
| State file race (probe/exporter) | Atomic rename on probe side |
| Disk full on state write | Log and continue, don't crash |
| infoROM reflash changes UUID | Serial number is primary ID, UUID is secondary |
| Silent wrong answer (units bug) | Sanity bounds on every raw value before use |
| Telemetry too sparse to score | Completeness gate, return N/A not fake Healthy |
| Kubernetes pod restart loses state | Probe state is best-effort on K8s, graceful |
| Config push with bad thresholds | Validation at startup, hard fail with reason |

---

## 9. Out of Scope for Phase 1

- Multi-GPU cluster-wide fleet normalization
- Kubernetes-native probe scheduling
- Alertmanager integration (use Prometheus alerting rules on exported metrics)
- Provisioning pipeline integration (interface designed for it, pipeline TBD)
- Financial valuation model
- Assessment report generation
- NVIDIA attestation integration
- HA or multi-instance exporter coordination
