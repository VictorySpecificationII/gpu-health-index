#ifndef GPU_HEALTH_TYPES_H
#define GPU_HEALTH_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * Enumerations
 * ========================================================================= */

typedef enum {
    GPU_CLASS_NA           = 0,   /* Incomplete telemetry — score not valid */
    GPU_CLASS_HEALTHY      = 1,   /* score >= 85                            */
    GPU_CLASS_MONITOR      = 2,   /* score >= 70                            */
    GPU_CLASS_DEGRADING    = 3,   /* score >= 50                            */
    GPU_CLASS_DECOMMISSION = 4,   /* score <  50                            */
} gpu_class_t;

typedef enum {
    GPU_IDENTITY_SERIAL = 0,   /* nvmlDeviceGetSerial() succeeded */
    GPU_IDENTITY_UUID   = 1,   /* serial unavailable, UUID used   */
} gpu_identity_src_t;

/* =========================================================================
 * Reason bitmask
 *
 * One bit per active penalty condition. Set in gpu_score_result_t.reason_mask
 * and forwarded verbatim in gpu_snapshot_t.reason_mask for /metrics rendering.
 * ========================================================================= */

#define GPU_REASON_TEMP_WARN             (1u << 0)
#define GPU_REASON_TEMP_BAD              (1u << 1)
#define GPU_REASON_HBM_TEMP_WARN         (1u << 2)
#define GPU_REASON_HBM_TEMP_BAD          (1u << 3)
#define GPU_REASON_CLK_STD_HIGH          (1u << 4)
#define GPU_REASON_POWER_SATURATION      (1u << 5)
#define GPU_REASON_ECC_SBE_HIGH          (1u << 6)
#define GPU_REASON_ECC_DBE_ACTIVE        (1u << 7)
#define GPU_REASON_PERF_DROP_WARN        (1u << 8)
#define GPU_REASON_PERF_DROP_BAD         (1u << 9)
#define GPU_REASON_PERF_DROP_SEVERE      (1u << 10)
#define GPU_REASON_RETIRED_PAGES_WARN    (1u << 11)
#define GPU_REASON_RETIRED_PAGES_BAD     (1u << 12)
#define GPU_REASON_ROW_REMAP             (1u << 13)
#define GPU_REASON_PCIE_GEN              (1u << 14)
#define GPU_REASON_PCIE_WIDTH            (1u << 15)
#define GPU_REASON_TELEMETRY_INCOMPLETE  (1u << 16)
#define GPU_REASON_PROBE_STALE           (1u << 17)

/* =========================================================================
 * Ring buffer
 *
 * Windowed signals only. All signals for one poll timestep travel together.
 * Statistical computation (p95, std dev, delta/rate) runs over this buffer.
 * Point-in-time signals live in gpu_state_t.
 * ========================================================================= */

typedef struct {
    /* Powered */
    double   power_w;
    double   power_limit_w;

    /* Thermal */
    double   temp_c;
    double   hbm_temp_c;

    /* Computing */
    double   sm_clock_mhz;

    /* Memory — running totals; scorer computes delta for rate */
    uint64_t ecc_sbe_volatile;
    uint64_t ecc_dbe_volatile;

    uint64_t timestamp_ms;       /* wall-clock epoch ms — used for completeness gate */
} gpu_sample_t;

typedef struct {
    gpu_sample_t *samples;       /* heap-allocated; capacity = window_s / poll_interval_s */
    int           head;          /* next write index */
    int           count;         /* valid entries (0..capacity) */
    int           capacity;
} gpu_ring_t;

/* =========================================================================
 * Current state
 *
 * Point-in-time signals, updated every poll cycle alongside ring buffer.
 * Not subject to windowing — threshold-checked on latest value.
 * ========================================================================= */

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
    double   mem_bw_util_pct;        /* DCGM; NaN if DCGM unavailable */

    /* Interconnects */
    int      pcie_link_gen;
    int      pcie_link_width;
    uint64_t pcie_replay_count;
    uint64_t nvlink_replay_count;
    uint64_t nvlink_recovery_count;
    uint64_t nvlink_crc_count;
    uint64_t xid_count;
    uint32_t xid_last_code;

    /* Power */
    double   board_power_w;          /* DCGM; NaN if DCGM unavailable   */
    double   energy_j;               /* DCGM; NaN if DCGM unavailable   */
    uint64_t power_violation_us;     /* DCGM; 0 if DCGM unavailable     */
    uint64_t thermal_violation_us;   /* DCGM; 0 if DCGM unavailable     */

    /* Throttle reasons — individual booleans, not a bitmask */
    int      throttle_sw_power_cap;
    int      throttle_hw_slowdown;
    int      throttle_hw_power_brake;
    int      throttle_sw_thermal;
    int      throttle_hw_thermal;

    /* Computing */
    double   mem_clock_mhz;
    int      util_gpu_pct;
    int      util_mem_pct;
    int      pstate;                 /* 0 = P0 = full performance */

    /* Fan */
    int      fan_speed_pct;          /* -1 if unavailable (liquid-cooled SXM) */
} gpu_state_t;

/* =========================================================================
 * Baseline and probe result
 * ========================================================================= */

typedef struct {
    int      available;          /* file present and parsed               */
    int      valid;              /* passed all validation checks           */
    int      serial_mismatch;    /* hard fail: file serial != device serial */
    int      driver_mismatch;    /* soft warn: driver changed since baseline */
    char     serial[32];
    char     uuid[48];
    char     driver_version[64];
    double   perf_w_mean;
    uint64_t established_at_s;   /* Unix epoch */
    char     workload[128];
    int      sample_count;
} gpu_baseline_t;

typedef struct {
    int      available;          /* file present and parsed   */
    int      stale;              /* age > probe_ttl_s         */
    char     serial[32];
    double   perf_w_mean;
    uint64_t probe_timestamp_s;  /* Unix epoch */
    int      probe_exit_code;
    char     workload[128];
    int      sample_count;
    double   probe_duration_s;
} gpu_probe_result_t;

/* =========================================================================
 * Scoring output
 *
 * Pure struct; output of scorer.c. No heap allocation.
 * ========================================================================= */

typedef struct {
    double      score;
    gpu_class_t classification;
    uint32_t    reason_mask;
    int         telemetry_ok;

    /* Pre-computed ring buffer statistics — also exposed as Prometheus metrics */
    double      temp_p95_c;
    double      hbm_temp_p95_c;
    double      clk_std_mhz;
    double      power_saturation_frac;  /* fraction of window at >= power_high_ratio of limit */
    double      ecc_sbe_rate_per_hour;
    int         ecc_dbe_in_window;      /* boolean — any DBE delta in window */

    /* Perf/W drop relative to baseline; NaN if baseline or probe unavailable */
    double      perf_drop_frac;
} gpu_score_result_t;

/* =========================================================================
 * Snapshot and IPC
 *
 * gpu_snapshot_t: fixed-size payload the HTTP child receives over socketpair.
 *   Self-contained — the child never touches ring buffers or NVML.
 *
 * gpu_ipc_msg_t: one message per GPU per poll cycle.
 *   Read/written with a single recv/send of exactly sizeof(gpu_ipc_msg_t).
 *
 * gpu_ipc_init_t: first write on the socketpair immediately after fork.
 *   Child reads this before entering the message loop.
 * ========================================================================= */

typedef struct {
    /* Identity */
    char                serial[32];
    char                uuid[48];
    char                gpu_model[64];
    char                driver_version[64];
    int                 gpu_index;
    gpu_identity_src_t  identity_source;
    int                 pcie_link_gen_max;
    int                 pcie_link_width_max;

    /* Score */
    double              score;
    gpu_class_t         classification;
    uint32_t            reason_mask;
    int                 telemetry_ok;

    /* Pre-computed ring buffer stats */
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
    int                 baseline_serial_mismatch;
    int                 baseline_driver_mismatch;
    uint64_t            baseline_age_s;

    /* Raw current state — all gpu_state_t fields, for /metrics rendering */
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
    uint64_t            last_poll_ms;    /* wall-clock epoch ms of last successful poll */
    int                 gpu_present;     /* 0 if device disappeared mid-run */
    int                 gpu_available;   /* 0 if NVML failing above threshold */
} gpu_snapshot_t;

typedef struct {
    int32_t        gpu_index;
    gpu_snapshot_t snapshot;
} gpu_ipc_msg_t;

typedef struct {
    int32_t num_gpus;
} gpu_ipc_init_t;

/* =========================================================================
 * Config
 *
 * All fields correspond 1:1 to the key=value config file.
 * Populated by config_load(): compiled-in defaults, then file, then env.
 * ========================================================================= */

/* Compiled-in defaults — used as the starting point in config_load() */
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

#endif /* GPU_HEALTH_TYPES_H */
