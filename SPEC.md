# GPU Health Exporter — Implementation Spec
## Section 6: File Layout, Types, and Module Interfaces

Derived from DESIGN.md checkpoint. All design decisions are final.
This document is the C coding reference — everything a compiler needs to know
before source is written.

---

## 6.1 File and Directory Layout

```
gpu-health-exporter/
├── src/
│   ├── main.c          — arg parsing, startup sequence (DESIGN §5.5), fork, signal handling
│   ├── nvml.c/h        — nvml_vtable_t definition, dlopen population, per-call wrappers
│   ├── dcgm.c/h        — dcgm_vtable_t definition, dlopen population, graceful fallback
│   ├── collector.c/h   — per-GPU poll thread, ring buffer writes, state updates
│   ├── scorer.c/h      — pure arithmetic, no I/O, ring-buffer path + current-state path
│   ├── snapshot.c/h    — mutex-protected snapshot build, socketpair IPC send/recv
│   ├── http.c/h        — HTTP child process: /metrics /ready /live, snapshot rendering
│   ├── config.c/h      — key=value parser, env var override (GPU_HEALTH_ prefix), validation
│   ├── state.c/h       — baseline read/validate, probe state read, inotify reload
│   ├── procpriv.c/h    — capability drop, seccomp filter installation
│   └── util.c/h        — logging (stderr, level-filtered), time helpers, safe string ops
├── tests/
│   ├── test_scorer.c   — unit tests, fake vtable injection, no hardware required
│   ├── test_config.c   — config parser validation, env override, boundary values
│   ├── test_state.c    — baseline load/validate, probe state parse, TTL checks
│   └── test_ring.c     — ring buffer writes, wraparound, statistical functions
├── deploy/
│   ├── gpu-health.service          — systemd unit (bare metal)
│   ├── gpu-health.conf.example     — annotated config with all keys and defaults
│   └── k8s/
│       ├── daemonset.yaml
│       ├── configmap-baseline.yaml
│       ├── servicemonitor.yaml
│       ├── rbac.yaml
│       └── Chart.yaml
├── probe/
│   ├── gpu_health_probe.cu         — cuBLAS BF16 GEMM probe (CUDA C)
│   └── Makefile
└── Makefile
```

### Per-file role summary

| File | Role |
|------|------|
| `main.c` | Entry point only. Calls config, nvml, dcgm, collector, fork, http. No business logic. |
| `nvml.c/h` | NVML vtable. dlopen libnvidia-ml.so, dlsym all function pointers. Wraps every call with timeout check. Exposes `nvml_load`, `nvml_unload`. |
| `dcgm.c/h` | DCGM vtable. dlopen libdcgm.so, dlsym. Connects to local DCGM daemon. Exposes field subscription setup and per-poll latest-value fetch. |
| `collector.c/h` | One `poll_thread` per GPU. Reads NVML+DCGM each cycle, writes `gpu_sample_t` to ring buffer, updates `gpu_state_t`, calls scorer, calls snapshot_update. |
| `scorer.c/h` | Pure function. Takes ring buffer + current state + baseline + probe result + config. Returns `gpu_score_result_t`. No I/O, no globals. |
| `snapshot.c/h` | Assembles `gpu_snapshot_t` from score result + current state + identity. Holds per-GPU mutex. Sends/receives `gpu_ipc_msg_t` over socketpair. |
| `http.c/h` | HTTP child process. Reads `gpu_ipc_msg_t` from socketpair, maintains snapshot array. Serves /metrics, /ready, /live. |
| `config.c/h` | Parses key=value config file. Applies `GPU_HEALTH_` env var overrides. Validates all values at load time. Exposes `gpu_config_t`. |
| `state.c/h` | Reads and validates baseline files and probe state files. Implements inotify watch on baseline_dir for hot reload. |
| `procpriv.c/h` | Drops Linux capabilities. Installs seccomp-BPF filter for HTTP child. |
| `util.c/h` | `log_info/warn/error/debug`. `time_now_ms()`. `safe_strncpy`. `parse_uint64`, `parse_double` with bounds checking. |

---

## 6.2 Type Definitions

All types defined in a shared header `src/types.h` included by all modules.
No module-specific type definitions visible across module boundaries.

### 6.2.1 Enumerations

```c
/* GPU health classification */
typedef enum {
    GPU_CLASS_NA           = 0,   /* Incomplete telemetry */
    GPU_CLASS_HEALTHY      = 1,   /* score >= 85          */
    GPU_CLASS_MONITOR      = 2,   /* score >= 70          */
    GPU_CLASS_DEGRADING    = 3,   /* score >= 50          */
    GPU_CLASS_DECOMMISSION = 4,   /* score <  50          */
} gpu_class_t;

/* GPU identity source — which identifier anchors this GPU */
typedef enum {
    GPU_IDENTITY_SERIAL = 0,   /* nvmlDeviceGetSerial() succeeded */
    GPU_IDENTITY_UUID   = 1,   /* serial unavailable, UUID used   */
} gpu_identity_src_t;

/* Log levels */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3,
} log_level_t;
```

### 6.2.2 Reason Bitmask

One bit per active penalty condition. Set in `gpu_score_result_t.reason_mask`
and forwarded verbatim in `gpu_snapshot_t.reason_mask` for /metrics rendering.

```c
#define GPU_REASON_TEMP_WARN            (1u << 0)   /* temp p95 > temp_p95_warn_c          */
#define GPU_REASON_TEMP_BAD             (1u << 1)   /* temp p95 > temp_p95_bad_c           */
#define GPU_REASON_HBM_TEMP_WARN        (1u << 2)   /* hbm temp p95 > hbm_temp_p95_warn_c  */
#define GPU_REASON_HBM_TEMP_BAD         (1u << 3)   /* hbm temp p95 > hbm_temp_p95_bad_c   */
#define GPU_REASON_CLK_STD_HIGH         (1u << 4)   /* sm clock std > clk_std_warn_mhz     */
#define GPU_REASON_POWER_SATURATION     (1u << 5)   /* any samples >= power_high_ratio      */
#define GPU_REASON_ECC_SBE_HIGH         (1u << 6)   /* sbe rate > ecc_sbe_rate_warn_per_hr  */
#define GPU_REASON_ECC_DBE_ACTIVE       (1u << 7)   /* any dbe volatile delta in window     */
#define GPU_REASON_PERF_DROP_WARN       (1u << 8)   /* perf/W drop > perf_drop_warn         */
#define GPU_REASON_PERF_DROP_BAD        (1u << 9)   /* perf/W drop > perf_drop_bad          */
#define GPU_REASON_PERF_DROP_SEVERE     (1u << 10)  /* perf/W drop > perf_drop_severe       */
#define GPU_REASON_RETIRED_PAGES_WARN   (1u << 11)  /* retired_pages_dbe >= retired_pages_warn */
#define GPU_REASON_RETIRED_PAGES_BAD    (1u << 12)  /* retired_pages_dbe >= retired_pages_bad  */
#define GPU_REASON_ROW_REMAP            (1u << 13)  /* row_remap_failures > 0               */
#define GPU_REASON_PCIE_GEN             (1u << 14)  /* pcie_link_gen < pcie_link_gen_max    */
#define GPU_REASON_PCIE_WIDTH           (1u << 15)  /* pcie_link_width < pcie_link_width_max */
#define GPU_REASON_TELEMETRY_INCOMPLETE (1u << 16)  /* completeness gate failed             */
#define GPU_REASON_PROBE_STALE          (1u << 17)  /* probe result older than probe_ttl_s  */
```

### 6.2.3 Ring Buffer Types

Windowed signals only. Statistical computation (p95, std dev, delta/rate)
runs over this buffer. Point-in-time signals live in `gpu_state_t`.

```c
/* One sample: all windowed signals for a single poll timestep */
typedef struct {
    /* Powered */
    double   power_w;
    double   power_limit_w;

    /* Thermal */
    double   temp_c;
    double   hbm_temp_c;

    /* Computing */
    double   sm_clock_mhz;

    /* Memory counters — ring buffer stores running totals, scorer computes delta */
    uint64_t ecc_sbe_volatile;   /* running count */
    uint64_t ecc_dbe_volatile;   /* running count */

    uint64_t timestamp_ms;       /* epoch ms — used for completeness gate */
} gpu_sample_t;

/* Ring buffer container — one per GPU, heap-allocated at startup */
typedef struct {
    gpu_sample_t *samples;       /* heap-allocated, capacity = window_s / poll_interval_s */
    int           head;          /* index of next write position */
    int           count;         /* samples currently valid (0..capacity) */
    int           capacity;      /* total slots */
} gpu_ring_t;
```

### 6.2.4 Current State Struct

Point-in-time signals. Updated every poll cycle alongside the ring buffer write.
Not subject to windowing — checked against thresholds on latest value.

```c
typedef struct {
    /* Memory — irreversible / lifetime */
    uint64_t ecc_sbe_aggregate;
    uint64_t ecc_dbe_aggregate;
    uint32_t retired_pages_sbe;
    uint32_t retired_pages_dbe;
    uint32_t row_remap_failures;
    int      pending_row_remap;

    /* Memory — capacity */
    uint64_t mem_used_bytes;
    uint64_t mem_free_bytes;
    uint64_t mem_total_bytes;
    double   mem_bw_util_pct;    /* DCGM — NaN if DCGM unavailable */

    /* Interconnects */
    int      pcie_link_gen;
    int      pcie_link_width;
    uint64_t pcie_replay_count;
    uint64_t nvlink_replay_count;
    uint64_t nvlink_recovery_count;
    uint64_t nvlink_crc_count;
    uint64_t xid_count;
    uint32_t xid_last_code;

    /* Power — current state */
    double   board_power_w;       /* DCGM — NaN if DCGM unavailable */
    double   energy_j;            /* DCGM — NaN if DCGM unavailable */
    uint64_t power_violation_us;  /* DCGM — 0 if DCGM unavailable   */
    uint64_t thermal_violation_us;/* DCGM — 0 if DCGM unavailable   */

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
    int      pstate;              /* 0 = P0 = full performance */

    /* Fan */
    int      fan_speed_pct;       /* -1 if unavailable (liquid cooled SXM) */
} gpu_state_t;
```

### 6.2.5 Baseline and Probe Result

```c
/* Loaded and validated baseline file */
typedef struct {
    int      available;           /* file present and parsed */
    int      valid;               /* passed all validation checks */
    int      driver_mismatch;     /* soft warn: driver changed since baseline */
    char     serial[32];
    char     uuid[48];
    char     driver_version[64];
    double   perf_w_mean;
    uint64_t established_at_s;    /* Unix epoch */
    char     workload[128];
    int      sample_count;
} gpu_baseline_t;

/* Loaded probe state file ({serial}.probe) */
typedef struct {
    int      available;           /* file present and parsed */
    int      stale;               /* result age > probe_ttl_s */
    char     serial[32];
    double   perf_w_mean;
    uint64_t probe_timestamp_s;   /* Unix epoch */
    int      probe_exit_code;
    char     workload[128];
    int      sample_count;
    double   probe_duration_s;
} gpu_probe_result_t;
```

### 6.2.6 Scoring Output

Output of `scorer.c`. Pure struct, no heap allocation.

```c
typedef struct {
    double      score;
    gpu_class_t classification;
    uint32_t    reason_mask;
    int         telemetry_ok;

    /* Ring buffer statistics — also exposed directly as Prometheus metrics */
    double      temp_p95_c;
    double      hbm_temp_p95_c;
    double      clk_std_mhz;
    double      power_saturation_frac;   /* fraction of window samples at >= power_high_ratio */
    double      ecc_sbe_rate_per_hour;
    int         ecc_dbe_in_window;       /* boolean — any DBE delta in window */

    /* Perf/W delta relative to baseline — NaN if baseline or probe unavailable */
    double      perf_drop_frac;
} gpu_score_result_t;
```

### 6.2.7 Snapshot and IPC

`gpu_snapshot_t` is the fixed-size payload the HTTP child receives.
It is self-contained — the child never touches ring buffers or NVML.

`gpu_ipc_msg_t` is the atomic unit sent over the socketpair.
One send per GPU per poll cycle. Child reads one message, updates
its per-GPU snapshot array at `gpu_index`.

```c
/* Fixed-size snapshot: everything the HTTP child needs to render /metrics */
typedef struct {
    /* Identity */
    char                serial[32];
    char                uuid[48];
    char                gpu_model[64];
    char                driver_version[64];
    int                 gpu_index;
    gpu_identity_src_t  identity_source;
    int                 pcie_link_gen_max;   /* from startup — static */
    int                 pcie_link_width_max; /* from startup — static */

    /* Score */
    double              score;
    gpu_class_t         classification;
    uint32_t            reason_mask;
    int                 telemetry_ok;

    /* Ring buffer stats (pre-computed) */
    double              temp_p95_c;
    double              hbm_temp_p95_c;
    double              clk_std_mhz;
    double              power_saturation_frac;
    double              ecc_sbe_rate_per_hour;
    int                 ecc_dbe_in_window;

    /* Perf/W */
    double              perf_drop_frac;
    int                 probe_stale;
    int                 probe_available;

    /* Baseline health */
    int                 baseline_available;
    int                 baseline_valid;
    int                 baseline_driver_mismatch;
    uint64_t            baseline_age_s;

    /* Raw current state — all fields from gpu_state_t, for /metrics rendering */
    uint64_t            ecc_sbe_aggregate;
    uint64_t            ecc_dbe_aggregate;
    uint32_t            retired_pages_sbe;
    uint32_t            retired_pages_dbe;
    uint32_t            row_remap_failures;
    int                 pending_row_remap;
    uint64_t            mem_used_bytes;
    uint64_t            mem_free_bytes;
    uint64_t            mem_total_bytes;
    double              mem_bw_util_pct;
    int                 pcie_link_gen;
    int                 pcie_link_width;
    uint64_t            pcie_replay_count;
    uint64_t            nvlink_replay_count;
    uint64_t            nvlink_recovery_count;
    uint64_t            nvlink_crc_count;
    uint64_t            xid_count;
    uint32_t            xid_last_code;
    double              board_power_w;
    double              power_w;
    double              power_limit_w;
    double              energy_j;
    uint64_t            power_violation_us;
    uint64_t            thermal_violation_us;
    int                 throttle_sw_power_cap;
    int                 throttle_hw_slowdown;
    int                 throttle_hw_power_brake;
    int                 throttle_sw_thermal;
    int                 throttle_hw_thermal;
    double              sm_clock_mhz;
    double              mem_clock_mhz;
    int                 util_gpu_pct;
    int                 util_mem_pct;
    int                 pstate;
    double              temp_c;
    double              hbm_temp_c;
    int                 fan_speed_pct;

    /* Exporter state */
    uint64_t            last_poll_ms;    /* epoch ms of last successful poll */
    int                 gpu_present;     /* 0 if device disappeared mid-run */
    int                 gpu_available;   /* 0 if NVML failing above threshold */
} gpu_snapshot_t;

/* IPC message over socketpair. Fixed size — read/write with sizeof. */
typedef struct {
    int32_t        gpu_index;
    gpu_snapshot_t snapshot;
} gpu_ipc_msg_t;

/* Sent as the very first write on the socketpair, immediately after fork.
   Child reads this before entering the message loop. */
typedef struct {
    int32_t num_gpus;
} gpu_ipc_init_t;
```

### 6.2.8 Config Struct

All fields correspond 1:1 to the key=value config file defined in DESIGN §5.2.
Defaults are compiled in and overridden by file, then by `GPU_HEALTH_` env vars.

```c
typedef struct {
    /* Paths */
    char   state_dir[256];
    char   baseline_dir[256];
    char   listen_addr[64];
    int    listen_port;

    /* Polling */
    int    poll_interval_s;
    int    window_s;
    int    state_write_interval_s;

    /* Telemetry completeness gate */
    double min_sample_ratio;
    double max_median_step_s;
    int    min_samples_absolute;

    /* Scoring — thermal */
    double temp_p95_warn_c;
    double temp_p95_bad_c;
    double hbm_temp_p95_warn_c;
    double hbm_temp_p95_bad_c;

    /* Scoring — clocks */
    double clk_std_warn_mhz;

    /* Scoring — power */
    double power_high_ratio;
    double power_penalty_max;

    /* Scoring — memory */
    double ecc_sbe_rate_warn_per_hour;
    double ecc_sbe_penalty;
    double ecc_dbe_penalty;
    int    retired_pages_warn;
    int    retired_pages_bad;
    double retired_pages_pen_warn;
    double retired_pages_pen_bad;
    double row_remap_failure_penalty;

    /* Scoring — PCIe */
    double pcie_link_degraded_penalty;
    double pcie_width_degraded_penalty;

    /* Scoring — perf/W drift */
    double perf_drop_warn;
    double perf_drop_bad;
    double perf_drop_severe;
    double perf_drop_pen_warn;
    double perf_drop_pen_bad;
    double perf_drop_pen_severe;

    /* Probe */
    int    probe_interval_s;
    int    probe_ttl_s;

    /* NVML */
    int    nvml_timeout_ms;
    int    nvml_error_threshold;
    int    nvml_hard_error_threshold;
    int    nvml_retry_interval_s;

    /* DCGM */
    int    dcgm_timeout_ms;
    int    dcgm_error_threshold;
    int    dcgm_retry_interval_s;

    /* TLS (child only, WITH_TLS=1 build) */
    char   tls_cert_path[256];
    char   tls_key_path[256];
} gpu_config_t;
```

Compiled-in defaults:

```c
#define CFG_DEFAULT_STATE_DIR              "/var/run/gpu-health"
#define CFG_DEFAULT_BASELINE_DIR           "/etc/gpu-health/baseline"
#define CFG_DEFAULT_LISTEN_ADDR            "0.0.0.0"
#define CFG_DEFAULT_LISTEN_PORT            9108
#define CFG_DEFAULT_POLL_INTERVAL_S        1
#define CFG_DEFAULT_WINDOW_S               300
#define CFG_DEFAULT_STATE_WRITE_INTERVAL_S 30
#define CFG_DEFAULT_MIN_SAMPLE_RATIO       0.80
#define CFG_DEFAULT_MAX_MEDIAN_STEP_S      2.5
#define CFG_DEFAULT_MIN_SAMPLES_ABSOLUTE   10
#define CFG_DEFAULT_TEMP_P95_WARN_C        80.0
#define CFG_DEFAULT_TEMP_P95_BAD_C         90.0
#define CFG_DEFAULT_HBM_TEMP_P95_WARN_C    85.0
#define CFG_DEFAULT_HBM_TEMP_P95_BAD_C     95.0
#define CFG_DEFAULT_CLK_STD_WARN_MHZ       120.0
#define CFG_DEFAULT_POWER_HIGH_RATIO       0.98
#define CFG_DEFAULT_POWER_PENALTY_MAX      3.0
#define CFG_DEFAULT_ECC_SBE_RATE_WARN      100.0
#define CFG_DEFAULT_ECC_SBE_PENALTY        5.0
#define CFG_DEFAULT_ECC_DBE_PENALTY        25.0
#define CFG_DEFAULT_RETIRED_PAGES_WARN     1
#define CFG_DEFAULT_RETIRED_PAGES_BAD      10
#define CFG_DEFAULT_RETIRED_PEN_WARN       5.0
#define CFG_DEFAULT_RETIRED_PEN_BAD        15.0
#define CFG_DEFAULT_ROW_REMAP_PENALTY      25.0
#define CFG_DEFAULT_PCIE_GEN_PENALTY       10.0
#define CFG_DEFAULT_PCIE_WIDTH_PENALTY     15.0
#define CFG_DEFAULT_PERF_DROP_WARN         0.03
#define CFG_DEFAULT_PERF_DROP_BAD          0.07
#define CFG_DEFAULT_PERF_DROP_SEVERE       0.12
#define CFG_DEFAULT_PERF_DROP_PEN_WARN     5.0
#define CFG_DEFAULT_PERF_DROP_PEN_BAD      15.0
#define CFG_DEFAULT_PERF_DROP_PEN_SEVERE   30.0
#define CFG_DEFAULT_PROBE_INTERVAL_S       86400
#define CFG_DEFAULT_PROBE_TTL_S            129600
#define CFG_DEFAULT_NVML_TIMEOUT_MS        5000
#define CFG_DEFAULT_NVML_ERROR_THRESHOLD   10
#define CFG_DEFAULT_NVML_HARD_THRESHOLD    1
#define CFG_DEFAULT_NVML_RETRY_INTERVAL_S  60
#define CFG_DEFAULT_DCGM_TIMEOUT_MS        5000
#define CFG_DEFAULT_DCGM_ERROR_THRESHOLD   10
#define CFG_DEFAULT_DCGM_RETRY_INTERVAL_S  60
```

### 6.2.9 Per-GPU Context

One per GPU, allocated as an array at startup. Lives in the parent process.
The HTTP child never has access to this struct.

```c
typedef struct {
    /* Identity — set at startup, immutable thereafter */
    char                serial[32];
    char                uuid[48];
    char                gpu_model[64];
    char                driver_version[64];
    int                 gpu_index;
    gpu_identity_src_t  identity_source;
    int                 pcie_link_gen_max;
    int                 pcie_link_width_max;
    void               *nvml_handle;    /* nvmlDevice_t, opaque in this header */

    /* Ring buffer — written by poll thread, held under ring_mutex */
    gpu_ring_t          ring;
    pthread_mutex_t     ring_mutex;

    /* Current state — written by poll thread, held under state_mutex */
    gpu_state_t         state;
    pthread_mutex_t     state_mutex;

    /* Latest snapshot — written by poll thread after scoring,
       held under snapshot_mutex, read by snapshot_send */
    gpu_snapshot_t      snapshot;
    pthread_mutex_t     snapshot_mutex;

    /* Baseline and probe — loaded at startup, reloaded via inotify */
    gpu_baseline_t      baseline;
    gpu_probe_result_t  probe;
    pthread_mutex_t     files_mutex;     /* protects baseline + probe together */

    /* Poll thread lifecycle */
    pthread_t           thread;
    volatile int        ready;           /* 1 after first successful poll */
    volatile int        stop;            /* signal thread to exit */

    /* Error tracking */
    volatile int        gpu_present;     /* 0 if device disappeared */
    volatile int        gpu_available;   /* 0 if NVML failing above threshold */
    int                 consecutive_errors;
    int                 consecutive_hard_errors;
    uint64_t            last_retry_ms;

    /* Exporter self-health counters — written by poll thread */
    uint64_t            collector_errors_total;
} gpu_ctx_t;
```

### 6.2.10 Top-Level Exporter Context

One instance, stack-allocated in `main`. Passed by pointer to all modules.

```c
typedef struct {
    gpu_config_t     cfg;
    nvml_vtable_t    nvml;
    dcgm_vtable_t    dcgm;
    int              dcgm_available;
    int              num_gpus;
    gpu_ctx_t       *gpus;           /* array of num_gpus, heap-allocated */
    int              parent_fd;      /* socketpair: parent writes here */
    int              child_fd;       /* socketpair: child reads from here */
    pid_t            child_pid;
    volatile int     running;        /* cleared on SIGTERM/SIGINT */
} exporter_t;
```

---

## 6.3 Vtable Definitions

All NVML and DCGM calls go through function pointer structs.
In production: populated via `dlopen`/`dlsym`.
In tests: populated with fake functions — no hardware, no stub .so.

### 6.3.1 NVML Vtable

NVML function names are `nvmlDeviceGet*` etc. The vtable strips the `nvml` prefix
for brevity internally. `nvml_load()` populates by dlsym-ing the canonical names.

```c
/* Forward-declare NVML opaque types used in vtable */
typedef void *nvml_device_t;   /* nvmlDevice_t, treated as opaque void* */

typedef struct {
    /* Library lifecycle */
    int  (*Init)(void);          /* nvmlInit_v2 */
    void (*Shutdown)(void);      /* nvmlShutdown */
    const char *(*ErrorString)(int result); /* nvmlErrorString */

    /* Device enumeration */
    int (*DeviceGetCount)(unsigned int *count);
    int (*DeviceGetHandleByIndex)(unsigned int index, nvml_device_t *device);

    /* Identity — called once at startup */
    int (*DeviceGetSerial)(nvml_device_t dev, char *serial, unsigned int len);
    int (*DeviceGetUUID)(nvml_device_t dev, char *uuid, unsigned int len);
    int (*DeviceGetName)(nvml_device_t dev, char *name, unsigned int len);
    int (*SystemGetDriverVersion)(char *version, unsigned int len);

    /* PCIe limits — called once at startup, stored in gpu_ctx_t */
    int (*DeviceGetMaxPcieLinkGeneration)(nvml_device_t dev, unsigned int *gen);
    int (*DeviceGetMaxPcieLinkWidth)(nvml_device_t dev, unsigned int *width);

    /* Per-poll thermal */
    int (*DeviceGetTemperature)(nvml_device_t dev, int sensor, unsigned int *temp);
    /*   sensor: 0 = NVML_TEMPERATURE_GPU, 1 = NVML_TEMPERATURE_MEM (HBM) */

    /* Per-poll power */
    int (*DeviceGetPowerUsage)(nvml_device_t dev, unsigned int *milliwatts);
    int (*DeviceGetEnforcedPowerLimit)(nvml_device_t dev, unsigned int *milliwatts);

    /* Per-poll ECC */
    int (*DeviceGetTotalEccErrors)(nvml_device_t dev, int error_type,
                                   int counter_type, unsigned long long *count);
    /*   error_type: 0=corrected(SBE), 1=uncorrected(DBE)                    */
    /*   counter_type: 0=volatile, 1=aggregate                                */

    /* Per-poll memory lifetime counters */
    int (*DeviceGetRetiredPages)(nvml_device_t dev, int cause,
                                 unsigned int *count, unsigned long long *addrs);
    /*   cause: 1=SBE (MULTIPLE_SINGLE_BIT_ECC), 2=DBE (DOUBLE_BIT_ECC)     */
    int (*DeviceGetRemappedRows)(nvml_device_t dev,
                                 unsigned int *corr_rows, unsigned int *uncorr_rows,
                                 unsigned int *is_pending, unsigned int *failure);

    /* Per-poll memory capacity */
    int (*DeviceGetMemoryInfo)(nvml_device_t dev,
                               unsigned long long *used,
                               unsigned long long *free,
                               unsigned long long *total);

    /* Per-poll clocks */
    int (*DeviceGetClockInfo)(nvml_device_t dev, int type, unsigned int *mhz);
    /*   type: 0=graphics, 1=SM, 2=mem, 3=video — use 1 for SM, 2 for mem   */

    /* Per-poll utilization */
    int (*DeviceGetUtilizationRates)(nvml_device_t dev,
                                     unsigned int *gpu_pct, unsigned int *mem_pct);

    /* Per-poll throttle reasons */
    int (*DeviceGetCurrentClocksThrottleReasons)(nvml_device_t dev,
                                                  unsigned long long *reasons);
    /*   Bitmask — individual bits checked in collector against named constants */

    /* Per-poll performance state */
    int (*DeviceGetPerformanceState)(nvml_device_t dev, int *pstate);

    /* Per-poll fan */
    int (*DeviceGetFanSpeed)(nvml_device_t dev, unsigned int *speed_pct);

    /* Per-poll PCIe current link state */
    int (*DeviceGetCurrPcieLinkGeneration)(nvml_device_t dev, unsigned int *gen);
    int (*DeviceGetCurrPcieLinkWidth)(nvml_device_t dev, unsigned int *width);

    /* Per-poll PCIe replay counter */
    int (*DeviceGetPcieReplayCounter)(nvml_device_t dev, unsigned int *value);

    /* Per-poll violation times — NVML fallback if DCGM unavailable */
    int (*DeviceGetViolationStatus)(nvml_device_t dev, int policy,
                                    unsigned long long *ref_time_us,
                                    unsigned long long *violation_time_us);
    /*   policy: 0=power, 4=thermal — nvmlPerfPolicyType_t                   */
} nvml_vtable_t;
```

### 6.3.2 DCGM Field Output Struct

DCGM is field-subscription based. After setup, the collector fetches latest
values per GPU per poll cycle. Results land in this struct.
Fields unavailable (DCGM not connected, or not supported on this GPU) are
signalled by a sentinel value: `-1.0` for doubles, `UINT64_MAX` for uint64.

```c
typedef struct {
    double   power_w;                /* DCGM_FI_DEV_POWER_USAGE              */
    double   board_power_w;          /* DCGM_FI_DEV_TOTAL_ENERGY_CONSUMPTION — actually power, not energy; correct field is FI_DEV_POWER_USAGE for board */
    double   energy_j;               /* DCGM_FI_DEV_ENERGY_CONSUMPTION (mJ → J) */
    double   mem_bw_util_pct;        /* DCGM_FI_DEV_MEM_COPY_UTIL            */
    uint64_t power_violation_us;     /* DCGM_FI_DEV_POWER_VIOLATION          */
    uint64_t thermal_violation_us;   /* DCGM_FI_DEV_THERMAL_VIOLATION        */
    uint64_t nvlink_replay;          /* DCGM_FI_DEV_NVLINK_REPLAY_ERROR_COUNT_TOTAL */
    uint64_t nvlink_recovery;        /* DCGM_FI_DEV_NVLINK_RECOVERY_ERROR_COUNT_TOTAL */
    uint64_t nvlink_crc;             /* DCGM_FI_DEV_NVLINK_CRC_FLIT_ERROR_COUNT_TOTAL */
    uint64_t xid_count;              /* DCGM_FI_DEV_XID_ERRORS               */
    uint32_t xid_last_code;          /* DCGM_FI_DEV_LAST_XID                 */
    uint64_t pcie_replay;            /* DCGM_FI_DEV_PCIE_REPLAY_COUNTER      */
    uint32_t row_remap_failures;     /* DCGM_FI_DEV_ROW_REMAP_FAILURE        */
} dcgm_fields_t;

/* Sentinel values for unavailable DCGM fields */
#define DCGM_FIELD_UNAVAILABLE_DBL   (-1.0)
#define DCGM_FIELD_UNAVAILABLE_U64   (UINT64_MAX)
#define DCGM_FIELD_UNAVAILABLE_U32   (UINT32_MAX)
```

### 6.3.3 DCGM Vtable

DCGM handle and group IDs are opaque longs in the actual API.
Treated as `long` here to avoid the dcgm.h dependency in this header.

```c
typedef struct {
    /* Library lifecycle */
    int  (*Init)(void);                   /* dcgmInit */
    void (*Shutdown)(void);               /* dcgmShutdown */

    /* Daemon connection */
    int  (*Connect)(const char *addr, long *handle);   /* dcgmConnect_v2 (embedded mode: NULL addr) */
    void (*Disconnect)(long handle);

    /* Startup: group + field group setup */
    int  (*GroupCreate)(long handle, int type, const char *name, long *group_id);
    int  (*GroupAddDevice)(long handle, long group_id, unsigned int gpu_id);
    int  (*FieldGroupCreate)(long handle, unsigned short *field_ids, int count,
                             const char *name, long *field_group_id);
    int  (*WatchFields)(long handle, long group_id, long field_group_id,
                        long update_freq_us, double max_keep_age_s,
                        int max_keep_samples);

    /* Per-poll — fetches latest value for all watched fields for one GPU */
    int  (*GetLatestValues)(long handle, int gpu_id,
                            unsigned short *fields, int count,
                            void /* dcgmFieldValue_v1 */ *values);

    const char *(*ErrorString)(int result);
} dcgm_vtable_t;
```

---

## 6.4 Public Module Interfaces

These are the external symbols. Internal helpers are static.

### nvml.h

```c
/* Load libnvidia-ml.so and populate vtable. Returns 0 on success. */
int  nvml_load(nvml_vtable_t *vt, void **dl_handle);

/* Unload library handle obtained from nvml_load. */
void nvml_unload(void *dl_handle);
```

### dcgm.h

```c
/* Load libdcgm.so and populate vtable. Returns 0 on success. */
int  dcgm_load(dcgm_vtable_t *vt, void **dl_handle);
void dcgm_unload(void *dl_handle);

/* Connect to DCGM daemon, set up group and field watches for all GPUs.
   gpu_ids: array of DCGM GPU IDs (0..num_gpus-1 typically).
   Returns 0 on success; on failure, caller marks dcgm_available=0. */
int  dcgm_setup(dcgm_vtable_t *vt, long *handle,
                unsigned int *gpu_ids, int num_gpus);

/* Fetch latest field values for gpu_id into out.
   Returns 0 on success. Sets unavailable sentinels on field errors. */
int  dcgm_poll(dcgm_vtable_t *vt, long handle, int gpu_id, dcgm_fields_t *out);

/* Disconnect and clean up. */
void dcgm_teardown(dcgm_vtable_t *vt, long handle);
```

### collector.h

```c
/* Spawn the poll thread for gpu ctx[i]. Returns 0 on success. */
int  collector_start(gpu_ctx_t *ctx, exporter_t *exp);

/* Signal the poll thread to stop and join it. */
void collector_stop(gpu_ctx_t *ctx);
```

### scorer.h

```c
/* Compute score from ring buffer + current state + baseline + probe.
   Pure function: no I/O, no globals, no mutex. Caller holds ring_mutex
   and state_mutex before calling.
   Returns 0 on success (out populated). Returns -1 if ring is empty. */
int score_gpu(const gpu_ring_t        *ring,
              const gpu_state_t       *state,
              const gpu_baseline_t    *baseline,
              const gpu_probe_result_t *probe,
              const gpu_config_t      *cfg,
              gpu_score_result_t      *out);

/* Completeness gate check only. Returns 1 if gate passes, 0 if not.
   Used to set gpu_telemetry_ok metric independently of full scoring. */
int telemetry_ok(const gpu_ring_t *ring, const gpu_config_t *cfg);

/* Ring buffer statistics — exposed so tests can validate each stat independently */
double ring_p95(const gpu_ring_t *ring, size_t field_offset);
double ring_stddev(const gpu_ring_t *ring, size_t field_offset);
double ring_mean(const gpu_ring_t *ring, size_t field_offset);
```

### snapshot.h

```c
/* Build gpu_snapshot_t from score + raw state + ctx identity fields.
   Acquires ctx->snapshot_mutex internally. */
void snapshot_update(gpu_ctx_t *ctx,
                     const gpu_score_result_t *score,
                     int dcgm_available);

/* Write one gpu_ipc_msg_t to fd. Returns 0 on success, -1 on error.
   Acquires ctx->snapshot_mutex internally for the copy. */
int snapshot_send(int fd, const gpu_ctx_t *ctx);

/* Read one gpu_ipc_msg_t from fd into msg. Returns 0 on success, -1 on error/EOF. */
int snapshot_recv(int fd, gpu_ipc_msg_t *msg);
```

### http.h

```c
/* HTTP child entry point. Called after fork in the child process.
   sock_fd: child side of socketpair. num_gpus: from gpu_ipc_init_t.
   cfg: passed by value (copy, since fork). Runs until sock_fd EOF. */
void http_serve(int sock_fd, int num_gpus, const gpu_config_t *cfg);
```

### config.h

```c
/* Parse config file at path and populate cfg with compiled-in defaults,
   then file values, then GPU_HEALTH_* env var overrides.
   path may be NULL — defaults + env only.
   Returns 0 on success. Hard fail on invalid values (logs reason, returns -1). */
int config_load(const char *path, gpu_config_t *cfg);

/* Validate a fully-populated cfg. Returns 0 if valid, -1 with log on failure. */
int config_validate(const gpu_config_t *cfg);
```

### state.h

```c
/* Read and validate baseline file for given serial.
   baseline_dir: from cfg. Returns 0 on success.
   On failure: out->available=0, out->valid=0, reason logged. */
int baseline_load(const char *baseline_dir, const char *serial,
                  gpu_baseline_t *out);

/* Read and validate probe state file for given serial.
   state_dir: from cfg. probe_ttl_s: from cfg.
   Returns 0 on success. On failure: out->available=0. */
int probe_load(const char *state_dir, const char *serial,
               int probe_ttl_s, gpu_probe_result_t *out);

/* Install inotify watch on baseline_dir. Returns inotify fd, or -1 on error.
   Caller calls baseline_reload_check() on each poll cycle. */
int baseline_inotify_init(const char *baseline_dir);

/* Check if inotify fd has events. If so, reload baseline for all GPUs.
   Non-blocking: returns immediately if no events pending.
   gpus: array, num_gpus: count. */
void baseline_inotify_check(int inotify_fd, const char *baseline_dir,
                             gpu_ctx_t *gpus, int num_gpus);
```

### procpriv.h

```c
/* Drop all Linux capabilities except those in keep_caps bitmask.
   Returns 0 on success. Call in child process after fork. */
int priv_drop_caps(unsigned long keep_caps);

/* Install seccomp-BPF whitelist for HTTP child:
   read, write, accept4, close, send, recv, exit_group, sigreturn only.
   Returns 0 on success. Call after bind/listen, before accept loop. */
int priv_seccomp_http(void);
```

### util.h

```c
/* Logging — all output to stderr, prefixed with level and timestamp */
void log_msg(log_level_t level, const char *fmt, ...);

#define log_debug(...)  log_msg(LOG_DEBUG, __VA_ARGS__)
#define log_info(...)   log_msg(LOG_INFO,  __VA_ARGS__)
#define log_warn(...)   log_msg(LOG_WARN,  __VA_ARGS__)
#define log_error(...)  log_msg(LOG_ERROR, __VA_ARGS__)

/* Monotonic timestamp in milliseconds */
uint64_t time_now_ms(void);

/* Wall clock timestamp in seconds (for TTL comparisons) */
uint64_t time_now_s(void);

/* ISO 8601 UTC timestamp into buf (for state file output).
   buf must be at least 21 bytes. */
void time_iso8601(uint64_t epoch_s, char *buf, size_t len);

/* Bounds-checked string copy. Always NUL-terminates. Returns 0. */
int safe_strncpy(char *dst, const char *src, size_t size);

/* Parse uint64 from string. Returns 0 on success, -1 on parse error or
   value outside [min, max]. */
int parse_uint64(const char *s, uint64_t min, uint64_t max, uint64_t *out);

/* Parse double from string. Returns 0 on success, -1 on parse error or
   value outside [min, max]. */
int parse_double(const char *s, double min, double max, double *out);
```

---

## 6.5 Complete Metric Names

Naming convention:
- `gpu_` prefix on all GPU metrics
- Snake case, units in name (`_celsius`, `_watts`, `_bytes`, `_seconds`, `_percent`, `_mhz`)
- Counters suffixed `_total`
- Every metric has HELP and TYPE lines
- Serial number is the only label on hot metrics — all metadata in `gpu_info`
- `gpu_info` carries uuid, model, driver_version, gpu_index, pcie_gen_max, pcie_width_max
- Fan speed not emitted when unavailable (liquid-cooled SXM) — metric absent, not -1
- DCGM-sourced metrics not emitted when DCGM unavailable — metric absent, not NaN

### Exporter self-health (no GPU label)

```
# HELP gpu_health_exporter_info Static information about the exporter build
# TYPE gpu_health_exporter_info gauge
gpu_health_exporter_info{version="",build_commit="",build_date=""} 1

# HELP gpu_dcgm_available 1 if DCGM daemon is connected and responding
# TYPE gpu_dcgm_available gauge
gpu_dcgm_available 1
```

### Per-GPU identity and metadata

```
# HELP gpu_info Static GPU identity and capability information
# TYPE gpu_info gauge
gpu_info{serial="",uuid="",model="",driver_version="",gpu_index="0",pcie_gen_max="4",pcie_width_max="16"} 1

# HELP gpu_identity_source Identity anchor used: 0=serial 1=uuid_fallback
# TYPE gpu_identity_source gauge
gpu_identity_source{serial=""} 0
```

### Health score

```
# HELP gpu_health_score Composite GPU health score 0-100
# TYPE gpu_health_score gauge
gpu_health_score{serial=""} 97.5

# HELP gpu_health_classification Health classification: 1=Healthy 2=Monitor 3=Degrading 4=Decommission 0=NA
# TYPE gpu_health_classification gauge
gpu_health_classification{serial=""} 1

# HELP gpu_health_reason_mask Bitmask of active score penalties (see reason bit definitions)
# TYPE gpu_health_reason_mask gauge
gpu_health_reason_mask{serial=""} 0

# HELP gpu_telemetry_ok 1 if telemetry completeness gate passed
# TYPE gpu_telemetry_ok gauge
gpu_telemetry_ok{serial=""} 1

# HELP gpu_health_last_poll_timestamp_seconds Unix timestamp of last successful poll cycle
# TYPE gpu_health_last_poll_timestamp_seconds gauge
gpu_health_last_poll_timestamp_seconds{serial=""} 1743600000

# HELP gpu_present 1 if GPU device is present and accessible
# TYPE gpu_present gauge
gpu_present{serial=""} 1

# HELP gpu_health_collector_errors_total Total collector errors by kind
# TYPE gpu_health_collector_errors_total counter
gpu_health_collector_errors_total{serial="",kind="nvml_call"} 0
```

### Thermal

```
# HELP gpu_temp_celsius GPU die temperature in Celsius
# TYPE gpu_temp_celsius gauge
gpu_temp_celsius{serial=""} 72.0

# HELP gpu_hbm_temp_celsius HBM memory temperature in Celsius
# TYPE gpu_hbm_temp_celsius gauge
gpu_hbm_temp_celsius{serial=""} 68.0

# HELP gpu_fan_speed_percent Fan speed as percent of maximum (absent if liquid cooled)
# TYPE gpu_fan_speed_percent gauge
gpu_fan_speed_percent{serial=""} 55

# HELP gpu_thermal_violation_microseconds_total Cumulative time GPU was thermally throttled
# TYPE gpu_thermal_violation_microseconds_total counter
gpu_thermal_violation_microseconds_total{serial=""} 0
```

### Power

```
# HELP gpu_power_watts Current GPU power draw in Watts
# TYPE gpu_power_watts gauge
gpu_power_watts{serial=""} 320.5

# HELP gpu_power_limit_watts Enforced power management limit in Watts
# TYPE gpu_power_limit_watts gauge
gpu_power_limit_watts{serial=""} 400.0

# HELP gpu_board_power_watts Total board power including HBM and VRMs (DCGM)
# TYPE gpu_board_power_watts gauge
gpu_board_power_watts{serial=""} 350.0

# HELP gpu_energy_joules_total Lifetime energy consumed in Joules (DCGM)
# TYPE gpu_energy_joules_total counter
gpu_energy_joules_total{serial=""} 1234567.8

# HELP gpu_power_violation_microseconds_total Cumulative time power was throttled
# TYPE gpu_power_violation_microseconds_total counter
gpu_power_violation_microseconds_total{serial=""} 0
```

### Throttle reasons

```
# HELP gpu_throttle_sw_power_cap 1 if software power cap is throttling clocks
# TYPE gpu_throttle_sw_power_cap gauge
gpu_throttle_sw_power_cap{serial=""} 0

# HELP gpu_throttle_hw_slowdown 1 if hardware slowdown is active
# TYPE gpu_throttle_hw_slowdown gauge
gpu_throttle_hw_slowdown{serial=""} 0

# HELP gpu_throttle_hw_power_brake 1 if hardware power brake is active
# TYPE gpu_throttle_hw_power_brake gauge
gpu_throttle_hw_power_brake{serial=""} 0

# HELP gpu_throttle_sw_thermal 1 if software thermal throttling is active
# TYPE gpu_throttle_sw_thermal gauge
gpu_throttle_sw_thermal{serial=""} 0

# HELP gpu_throttle_hw_thermal 1 if hardware thermal throttling is active
# TYPE gpu_throttle_hw_thermal gauge
gpu_throttle_hw_thermal{serial=""} 0
```

### Memory — ECC

```
# HELP gpu_ecc_sbe_volatile_total Volatile single-bit ECC error count (resets on driver reload)
# TYPE gpu_ecc_sbe_volatile_total counter
gpu_ecc_sbe_volatile_total{serial=""} 0

# HELP gpu_ecc_dbe_volatile_total Volatile double-bit ECC error count (resets on driver reload)
# TYPE gpu_ecc_dbe_volatile_total counter
gpu_ecc_dbe_volatile_total{serial=""} 0

# HELP gpu_ecc_sbe_aggregate_total Lifetime single-bit ECC error count (never resets)
# TYPE gpu_ecc_sbe_aggregate_total counter
gpu_ecc_sbe_aggregate_total{serial=""} 0

# HELP gpu_ecc_dbe_aggregate_total Lifetime double-bit ECC error count (never resets)
# TYPE gpu_ecc_dbe_aggregate_total counter
gpu_ecc_dbe_aggregate_total{serial=""} 0

# HELP gpu_retired_pages_sbe Pages retired due to single-bit ECC errors
# TYPE gpu_retired_pages_sbe gauge
gpu_retired_pages_sbe{serial=""} 0

# HELP gpu_retired_pages_dbe Pages retired due to double-bit ECC errors
# TYPE gpu_retired_pages_dbe gauge
gpu_retired_pages_dbe{serial=""} 0

# HELP gpu_row_remap_failures HBM row remap failure count (any value > 0 is serious)
# TYPE gpu_row_remap_failures gauge
gpu_row_remap_failures{serial=""} 0

# HELP gpu_pending_row_remap 1 if a row remap is pending and reboot is required
# TYPE gpu_pending_row_remap gauge
gpu_pending_row_remap{serial=""} 0
```

### Memory — capacity and bandwidth

```
# HELP gpu_memory_used_bytes GPU memory currently in use
# TYPE gpu_memory_used_bytes gauge
gpu_memory_used_bytes{serial=""} 0

# HELP gpu_memory_free_bytes GPU memory currently free
# TYPE gpu_memory_free_bytes gauge
gpu_memory_free_bytes{serial=""} 80530636800

# HELP gpu_memory_total_bytes Total GPU memory capacity
# TYPE gpu_memory_total_bytes gauge
gpu_memory_total_bytes{serial=""} 80530636800

# HELP gpu_memory_bandwidth_utilization_percent HBM bandwidth utilization percent (DCGM)
# TYPE gpu_memory_bandwidth_utilization_percent gauge
gpu_memory_bandwidth_utilization_percent{serial=""} 12.5
```

### Interconnects

```
# HELP gpu_pcie_link_gen Current PCIe link generation
# TYPE gpu_pcie_link_gen gauge
gpu_pcie_link_gen{serial=""} 4

# HELP gpu_pcie_link_width Current PCIe link width in lanes
# TYPE gpu_pcie_link_width gauge
gpu_pcie_link_width{serial=""} 16

# HELP gpu_pcie_replay_total PCIe replay counter total
# TYPE gpu_pcie_replay_total counter
gpu_pcie_replay_total{serial=""} 0

# HELP gpu_nvlink_replay_total NVLink replay error count across all links
# TYPE gpu_nvlink_replay_total counter
gpu_nvlink_replay_total{serial=""} 0

# HELP gpu_nvlink_recovery_total NVLink recovery error count across all links
# TYPE gpu_nvlink_recovery_total counter
gpu_nvlink_recovery_total{serial=""} 0

# HELP gpu_nvlink_crc_total NVLink CRC flit error count across all links
# TYPE gpu_nvlink_crc_total counter
gpu_nvlink_crc_total{serial=""} 0

# HELP gpu_xid_errors_total Total XID error events
# TYPE gpu_xid_errors_total counter
gpu_xid_errors_total{serial=""} 0

# HELP gpu_xid_last_code Most recent XID error code (0 if no errors)
# TYPE gpu_xid_last_code gauge
gpu_xid_last_code{serial=""} 0
```

### Compute

```
# HELP gpu_sm_clock_mhz Current SM clock frequency in MHz
# TYPE gpu_sm_clock_mhz gauge
gpu_sm_clock_mhz{serial=""} 1410

# HELP gpu_memory_clock_mhz Current memory clock frequency in MHz
# TYPE gpu_memory_clock_mhz gauge
gpu_memory_clock_mhz{serial=""} 1215

# HELP gpu_utilization_gpu_percent GPU compute utilization percent
# TYPE gpu_utilization_gpu_percent gauge
gpu_utilization_gpu_percent{serial=""} 0

# HELP gpu_utilization_memory_percent GPU memory controller utilization percent
# TYPE gpu_utilization_memory_percent gauge
gpu_utilization_memory_percent{serial=""} 0

# HELP gpu_performance_state Current P-state (0=P0=full performance)
# TYPE gpu_performance_state gauge
gpu_performance_state{serial=""} 8
```

### Ring buffer statistics (pre-computed, also used in scoring)

```
# HELP gpu_temp_p95_celsius 95th percentile GPU temperature over scoring window
# TYPE gpu_temp_p95_celsius gauge
gpu_temp_p95_celsius{serial=""} 74.0

# HELP gpu_hbm_temp_p95_celsius 95th percentile HBM temperature over scoring window
# TYPE gpu_hbm_temp_p95_celsius gauge
gpu_hbm_temp_p95_celsius{serial=""} 69.0

# HELP gpu_sm_clock_stddev_mhz Standard deviation of SM clock over scoring window
# TYPE gpu_sm_clock_stddev_mhz gauge
gpu_sm_clock_stddev_mhz{serial=""} 12.3

# HELP gpu_power_saturation_fraction Fraction of window samples at or above power_high_ratio of limit
# TYPE gpu_power_saturation_fraction gauge
gpu_power_saturation_fraction{serial=""} 0.0

# HELP gpu_ecc_sbe_rate_per_hour ECC SBE event rate over scoring window in events/hour
# TYPE gpu_ecc_sbe_rate_per_hour gauge
gpu_ecc_sbe_rate_per_hour{serial=""} 0.0
```

### Baseline and probe health

```
# HELP gpu_baseline_available 1 if baseline file is present and valid
# TYPE gpu_baseline_available gauge
gpu_baseline_available{serial=""} 1

# HELP gpu_baseline_valid 1 if baseline file passed all validation checks
# TYPE gpu_baseline_valid gauge
gpu_baseline_valid{serial=""} 1

# HELP gpu_baseline_age_seconds Seconds since baseline was established
# TYPE gpu_baseline_age_seconds gauge
gpu_baseline_age_seconds{serial=""} 86400

# HELP gpu_baseline_serial_mismatch 1 if baseline serial does not match device serial
# TYPE gpu_baseline_serial_mismatch gauge
gpu_baseline_serial_mismatch{serial=""} 0

# HELP gpu_baseline_driver_mismatch 1 if driver version changed since baseline was established
# TYPE gpu_baseline_driver_mismatch gauge
gpu_baseline_driver_mismatch{serial=""} 0

# HELP gpu_probe_result_stale 1 if probe result is older than probe_ttl_s
# TYPE gpu_probe_result_stale gauge
gpu_probe_result_stale{serial=""} 0

# HELP gpu_probe_perf_drop_fraction Perf/W degradation relative to baseline (0.03 = 3% drop; absent if no baseline/probe)
# TYPE gpu_probe_perf_drop_fraction gauge
gpu_probe_perf_drop_fraction{serial=""} 0.01
```

---

## 6.6 Build System

### Makefile targets

```makefile
all:     builds gpu-health-exporter binary
test:    builds and runs all tests under tests/ (no hardware required)
clean:   removes build artifacts
install: installs binary to $(PREFIX)/bin, service file to /etc/systemd/system/
```

### Makefile flags

| Flag | Effect |
|------|--------|
| `WITH_TLS=1` | Links mbedTLS, enables TLS in HTTP child |
| `DEBUG=1` | Enables `-fsanitize=address,undefined`, `-g`, `-O0` |
| `PREFIX=/usr` | Install prefix (default: `/usr/local`) |

### Link model

- Statically link everything except `libdl` (required for `dlopen`)
- `libdl` is the only dynamic dependency in the final binary
- `libnvidia-ml.so` and `libdcgm.so` loaded at runtime via `dlopen`
- `mbedTLS` statically linked when `WITH_TLS=1`
- Target architectures: x86_64. Single compilation, runs on Ampere through Blackwell.

### Compiler requirements

- C11 (`-std=c11`)
- `-Wall -Wextra -Wpedantic -Werror`
- `_GNU_SOURCE` defined (for `pthread`, `seccomp`, `inotify`)
- Minimum GCC 9 or Clang 10

### Test build

Tests link against a test harness that provides a fake `nvml_vtable_t` and
`dcgm_vtable_t` populated with deterministic stub functions. No NVML or DCGM
installation required. Tests can run in CI without GPU hardware.

---

## Implementation Order

Write modules in this order. Each step is independently testable.

1. `util.c/h` — logging, time, string helpers. No deps.
2. `config.c/h` + `test_config.c` — parser, validation, env override.
3. `types.h` — all structs. Included by all subsequent modules.
4. `scorer.c/h` + `test_scorer.c` — pure arithmetic. Fake ring buffer input.
5. `state.c/h` + `test_state.c` — file parsing, validation, TTL checks.
6. Ring buffer functions in `collector.c` + `test_ring.c` — writes, wraparound, p95/stddev.
7. `nvml.c/h` — vtable, dlopen, wrappers.
8. `dcgm.c/h` — vtable, dlopen, field setup, graceful fallback.
9. `collector.c/h` — poll thread, ties ring + state + scorer + snapshot.
10. `snapshot.c/h` — IPC send/recv.
11. `procpriv.c/h` — cap drop, seccomp.
12. `http.c/h` — child HTTP server, /metrics rendering.
13. `main.c` — startup sequence, fork, signal handling, watchpid loop.
