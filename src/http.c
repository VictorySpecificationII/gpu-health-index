/*
 * http.c — HTTP child process: /metrics /ready /live.
 *
 * Runs as the unprivileged child after fork().  Never touches NVML, ring
 * buffers, or any parent-process state.  All GPU data arrives via the
 * socketpair as gpu_ipc_msg_t messages.
 *
 * Threading model:
 *   Receiver thread  — drains IPC messages, updates g_slots[] under g_lock.
 *   Main thread      — HTTP accept loop; renders metrics from a locked copy.
 *
 * Known gaps (to be closed when types.h is extended):
 *   - gpu_health_collector_errors_total: not in gpu_snapshot_t.
 *   - ECC volatile counters: not forwarded in snapshot (aggregate + rate are).
 *   - gpu_dcgm_available: inferred from NaN check on mem_bw_util_pct.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "http.h"
#include "snapshot.h"
#include "util.h"

/* --------------------------------------------------------------------------
 * Build constants
 * -------------------------------------------------------------------------- */

#define GPU_HEALTH_VERSION  "0.1.0"
#define METRICS_BUF_SIZE    (256u * 1024u)  /* max Prometheus response body   */
#define REQ_BUF_SIZE        4096u           /* request read buffer            */
#define SEND_TIMEOUT_S      5               /* SO_SNDTIMEO / SO_RCVTIMEO      */
#define LISTEN_BACKLOG      16

/* --------------------------------------------------------------------------
 * Process-local state
 * -------------------------------------------------------------------------- */

typedef struct {
    gpu_snapshot_t snap;
    int            received;    /* non-zero after first IPC message for slot */
} http_slot_t;

static http_slot_t    *g_slots;
static int             g_num_gpus;
static volatile int    g_all_ready;     /* 1 when every slot has been populated  */
static volatile int    g_running   = 1; /* cleared by SIGTERM handler or IPC EOF */
static pthread_mutex_t g_lock      = PTHREAD_MUTEX_INITIALIZER;
static int             g_stale_ms;      /* /live threshold: 3 × poll_interval_s  */

/* --------------------------------------------------------------------------
 * Signal handling
 * -------------------------------------------------------------------------- */

static void handle_sigterm(int sig) { (void)sig; g_running = 0; }

/* --------------------------------------------------------------------------
 * Buffer helper — bounds-checked vsnprintf accumulator.
 * Writes into buf[*pos .. cap-1]; always keeps buf NUL-terminated.
 * -------------------------------------------------------------------------- */

static void bprintf(char *buf, size_t *pos, size_t cap,
                    const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void bprintf(char *buf, size_t *pos, size_t cap,
                    const char *fmt, ...) {
    if (*pos >= cap - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t written = (size_t)n;
        if (written > cap - *pos - 1)
            written = cap - *pos - 1;
        *pos += written;
    }
}

/* --------------------------------------------------------------------------
 * IPC receiver thread
 * Drains gpu_ipc_msg_t from the socketpair; updates g_slots[] under g_lock.
 * On IPC EOF (parent died), sets g_running = 0 so the accept loop exits.
 * -------------------------------------------------------------------------- */

static void *ipc_receiver_thread(void *arg) {
    int fd = *(int *)arg;
    free(arg);

    gpu_ipc_msg_t msg;
    while (g_running) {
        if (snapshot_recv(fd, &msg) < 0) {
            /* EOF: parent closed the socketpair — time to exit */
            g_running = 0;
            break;
        }
        if (msg.gpu_index < 0 || msg.gpu_index >= g_num_gpus) {
            log_warn("http: IPC message has out-of-range gpu_index %d (num_gpus=%d)",
                     msg.gpu_index, g_num_gpus);
            continue;
        }
        pthread_mutex_lock(&g_lock);
        g_slots[msg.gpu_index].snap     = msg.snapshot;
        g_slots[msg.gpu_index].received = 1;
        if (!g_all_ready) {
            int all = 1;
            for (int i = 0; i < g_num_gpus; i++) {
                if (!g_slots[i].received) { all = 0; break; }
            }
            if (all) {
                g_all_ready = 1;
                log_info("http: all %d GPU(s) have reported — /ready now returns 200",
                         g_num_gpus);
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Snapshot copy helper
 * Lock, copy all slots, unlock.  Caller owns the returned allocation.
 * Returns the number of GPUs on success, -1 on allocation failure.
 * -------------------------------------------------------------------------- */

static int copy_slots(http_slot_t **out) {
    pthread_mutex_lock(&g_lock);
    size_t sz = (size_t)g_num_gpus * sizeof(http_slot_t);
    http_slot_t *copy = malloc(sz);
    if (!copy) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    memcpy(copy, g_slots, sz);
    pthread_mutex_unlock(&g_lock);
    *out = copy;
    return g_num_gpus;
}

/* --------------------------------------------------------------------------
 * Prometheus metrics rendering
 *
 * Format: Prometheus text format 0.0.4.
 * Each metric family gets exactly one # HELP and one # TYPE line, followed
 * by one sample line per GPU that has reported at least one snapshot.
 *
 * Label convention: serial= is the only label on per-GPU hot metrics.
 * Static metadata lives in gpu_info{...} = 1, joined in PromQL when needed.
 *
 * NaN handling: DCGM-only fields are NaN when DCGM is unavailable.
 *   Prometheus text format accepts NaN as a valid float value.  This is
 *   preferable to omitting the series entirely, because it preserves the
 *   metric name in the target's label set.
 *
 * Fan speed: omitted entirely for liquid-cooled GPUs (fan_speed_pct == -1).
 * -------------------------------------------------------------------------- */

/* Emit HELP + TYPE header for a metric family. */
#define M_HEADER(name, type_str, help_str)                          \
    bprintf(buf, &pos, cap,                                         \
        "# HELP " name " " help_str "\n"                            \
        "# TYPE " name " " type_str "\n")

/* Emit one sample line: metric_name{serial="..."} <double> */
#define M_GAUGE_D(name, serial, val)                                \
    bprintf(buf, &pos, cap, name "{serial=\"%s\"} %.6g\n",          \
            (serial), (double)(val))

/* Emit one sample line with an explicit format string. */
#define M_GAUGE_FMT(name, serial, fmt, val)                         \
    bprintf(buf, &pos, cap, name "{serial=\"%s\"} " fmt "\n",       \
            (serial), (val))

/* Emit a uint64 counter sample. */
#define M_COUNTER_U64(name, serial, val)                            \
    bprintf(buf, &pos, cap,                                         \
            name "{serial=\"%s\"} %" PRIu64 "\n", (serial), (uint64_t)(val))

/* Emit an int gauge. */
#define M_GAUGE_I(name, serial, val)                                \
    bprintf(buf, &pos, cap, name "{serial=\"%s\"} %d\n",            \
            (serial), (int)(val))

/* Emit a NaN-aware double gauge (emits NaN if isnan). */
#define M_GAUGE_NAN(name, serial, val) do {                         \
    if (isnan((double)(val)))                                       \
        bprintf(buf, &pos, cap, name "{serial=\"%s\"} NaN\n",       \
                (serial));                                          \
    else                                                            \
        bprintf(buf, &pos, cap, name "{serial=\"%s\"} %.6g\n",      \
                (serial), (double)(val));                           \
} while (0)

static size_t render_metrics(char *buf, size_t cap) {
    size_t pos = 0;

    http_slot_t *slots;
    int n = copy_slots(&slots);
    if (n < 0) {
        bprintf(buf, &pos, cap, "# ERROR: allocation failed\n");
        return pos;
    }

    /* ------------------------------------------------------------------ */
    /* 1. Exporter global                                                  */
    /* ------------------------------------------------------------------ */
    M_HEADER("gpu_health_exporter_info", "gauge",
             "GPU health exporter build information");
    bprintf(buf, &pos, cap,
            "gpu_health_exporter_info{version=\"" GPU_HEALTH_VERSION "\"} 1\n");

    /* ------------------------------------------------------------------ */
    /* 2. Identity and static properties                                   */
    /* ------------------------------------------------------------------ */
    M_HEADER("gpu_info", "gauge",
             "GPU identity and static properties; join on serial label for metadata");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        const gpu_snapshot_t *s = &slots[i].snap;
        bprintf(buf, &pos, cap,
                "gpu_info{serial=\"%s\",uuid=\"%s\",model=\"%s\","
                "driver=\"%s\",index=\"%d\","
                "pcie_gen_max=\"%d\",pcie_width_max=\"%d\"} 1\n",
                s->serial, s->uuid, s->gpu_model, s->driver_version,
                s->gpu_index, s->pcie_link_gen_max, s->pcie_link_width_max);
    }

    M_HEADER("gpu_identity_source", "gauge",
             "Identity source used for metric labels (0=serial, 1=uuid_fallback)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_identity_source", slots[i].snap.serial,
                  (int)slots[i].snap.identity_source);
    }

    /* ------------------------------------------------------------------ */
    /* 3. Health score                                                     */
    /* ------------------------------------------------------------------ */
    M_HEADER("gpu_health_score", "gauge",
             "GPU health score (0=worst, 100=fully healthy); N/A class when telemetry_ok=0");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_health_score", slots[i].snap.serial,
                    "%.2f", slots[i].snap.score);
    }

    M_HEADER("gpu_health_class", "gauge",
             "Health classification: 0=N/A 1=Healthy 2=Monitor 3=Degrading 4=Decommission");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_health_class", slots[i].snap.serial,
                  (int)slots[i].snap.classification);
    }

    M_HEADER("gpu_telemetry_ok", "gauge",
             "1 if telemetry completeness gate passed and score is valid");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_telemetry_ok", slots[i].snap.serial,
                  slots[i].snap.telemetry_ok);
    }

    /* ------------------------------------------------------------------ */
    /* 4. Ring buffer statistics (pre-computed by scorer)                  */
    /* ------------------------------------------------------------------ */
    M_HEADER("gpu_temp_p95_celsius", "gauge",
             "GPU die temperature p95 over scoring window (Celsius)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_temp_p95_celsius", slots[i].snap.serial,
                    "%.2f", slots[i].snap.temp_p95_c);
    }

    M_HEADER("gpu_hbm_temp_p95_celsius", "gauge",
             "HBM temperature p95 over scoring window (Celsius)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_hbm_temp_p95_celsius", slots[i].snap.serial,
                    "%.2f", slots[i].snap.hbm_temp_p95_c);
    }

    M_HEADER("gpu_sm_clock_std_mhz", "gauge",
             "SM clock standard deviation over scoring window (MHz)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_sm_clock_std_mhz", slots[i].snap.serial,
                    "%.2f", slots[i].snap.clk_std_mhz);
    }

    M_HEADER("gpu_power_saturation_ratio", "gauge",
             "Fraction of scoring window where power >= high_ratio * limit");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_power_saturation_ratio", slots[i].snap.serial,
                    "%.4f", slots[i].snap.power_saturation_frac);
    }

    M_HEADER("gpu_ecc_sbe_rate_per_hour", "gauge",
             "ECC single-bit error rate over scoring window (errors/hour)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_ecc_sbe_rate_per_hour", slots[i].snap.serial,
                    "%.2f", slots[i].snap.ecc_sbe_rate_per_hour);
    }

    M_HEADER("gpu_ecc_dbe_in_window", "gauge",
             "1 if any ECC double-bit error delta was observed in the scoring window");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_ecc_dbe_in_window", slots[i].snap.serial,
                  slots[i].snap.ecc_dbe_in_window);
    }

    /* ------------------------------------------------------------------ */
    /* 5. Thermal — current values                                         */
    /* ------------------------------------------------------------------ */
    M_HEADER("gpu_temp_celsius", "gauge",
             "GPU die temperature current value (Celsius)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_temp_celsius", slots[i].snap.serial,
                    "%.1f", slots[i].snap.temp_c);
    }

    M_HEADER("gpu_hbm_temp_celsius", "gauge",
             "HBM temperature current value (Celsius)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_hbm_temp_celsius", slots[i].snap.serial,
                    "%.1f", slots[i].snap.hbm_temp_c);
    }

    /* Fan: omit entirely for liquid-cooled GPUs (fan_speed_pct == -1). */
    M_HEADER("gpu_fan_speed_ratio", "gauge",
             "Fan speed as a fraction (0.0-1.0); absent for liquid-cooled GPUs");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        if (slots[i].snap.fan_speed_pct < 0) continue;
        M_GAUGE_FMT("gpu_fan_speed_ratio", slots[i].snap.serial,
                    "%.4f", slots[i].snap.fan_speed_pct / 100.0);
    }

    /* ------------------------------------------------------------------ */
    /* 6. Power                                                            */
    /* ------------------------------------------------------------------ */
    M_HEADER("gpu_power_watts", "gauge",
             "Current GPU power draw (Watts)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_power_watts", slots[i].snap.serial,
                    "%.2f", slots[i].snap.power_w);
    }

    M_HEADER("gpu_power_limit_watts", "gauge",
             "Current power management limit (Watts)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_power_limit_watts", slots[i].snap.serial,
                    "%.2f", slots[i].snap.power_limit_w);
    }

    /* board_power_w: NaN when DCGM unavailable */
    M_HEADER("gpu_board_power_watts", "gauge",
             "Total board power draw (Watts); NaN if DCGM unavailable");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_NAN("gpu_board_power_watts", slots[i].snap.serial,
                    slots[i].snap.board_power_w);
    }

    /* energy_j: NaN when DCGM unavailable */
    M_HEADER("gpu_energy_joules_total", "counter",
             "Total energy consumed (Joules); NaN if DCGM unavailable");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_NAN("gpu_energy_joules_total", slots[i].snap.serial,
                    slots[i].snap.energy_j);
    }

    M_HEADER("gpu_power_violation_microseconds_total", "counter",
             "Cumulative power violation time (microseconds); 0 if DCGM unavailable");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_COUNTER_U64("gpu_power_violation_microseconds_total",
                      slots[i].snap.serial, slots[i].snap.power_violation_us);
    }

    M_HEADER("gpu_thermal_violation_microseconds_total", "counter",
             "Cumulative thermal violation time (microseconds); 0 if DCGM unavailable");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_COUNTER_U64("gpu_thermal_violation_microseconds_total",
                      slots[i].snap.serial, slots[i].snap.thermal_violation_us);
    }

    M_HEADER("gpu_throttle_sw_power_cap", "gauge",
             "1 if software power cap throttle is active");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_throttle_sw_power_cap", slots[i].snap.serial,
                  slots[i].snap.throttle_sw_power_cap);
    }

    M_HEADER("gpu_throttle_hw_slowdown", "gauge",
             "1 if hardware thermal slowdown throttle is active");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_throttle_hw_slowdown", slots[i].snap.serial,
                  slots[i].snap.throttle_hw_slowdown);
    }

    M_HEADER("gpu_throttle_hw_power_brake", "gauge",
             "1 if hardware power brake throttle is active");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_throttle_hw_power_brake", slots[i].snap.serial,
                  slots[i].snap.throttle_hw_power_brake);
    }

    M_HEADER("gpu_throttle_sw_thermal", "gauge",
             "1 if software thermal throttle is active");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_throttle_sw_thermal", slots[i].snap.serial,
                  slots[i].snap.throttle_sw_thermal);
    }

    M_HEADER("gpu_throttle_hw_thermal", "gauge",
             "1 if hardware thermal throttle is active");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_throttle_hw_thermal", slots[i].snap.serial,
                  slots[i].snap.throttle_hw_thermal);
    }

    /* ------------------------------------------------------------------ */
    /* 7. Memory                                                           */
    /* ------------------------------------------------------------------ */
    M_HEADER("gpu_memory_used_bytes", "gauge",
             "GPU memory currently in use (bytes)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_COUNTER_U64("gpu_memory_used_bytes",
                      slots[i].snap.serial, slots[i].snap.mem_used_bytes);
    }

    M_HEADER("gpu_memory_free_bytes", "gauge",
             "GPU memory free (bytes)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_COUNTER_U64("gpu_memory_free_bytes",
                      slots[i].snap.serial, slots[i].snap.mem_free_bytes);
    }

    M_HEADER("gpu_memory_total_bytes", "gauge",
             "Total GPU memory capacity (bytes)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_COUNTER_U64("gpu_memory_total_bytes",
                      slots[i].snap.serial, slots[i].snap.mem_total_bytes);
    }

    /* mem_bw_util_pct: NaN when DCGM unavailable; emit as ratio */
    M_HEADER("gpu_memory_bandwidth_utilization_ratio", "gauge",
             "Memory bandwidth utilization (0.0-1.0); NaN if DCGM unavailable");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        const gpu_snapshot_t *s = &slots[i].snap;
        if (isnan(s->mem_bw_util_pct))
            bprintf(buf, &pos, cap,
                    "gpu_memory_bandwidth_utilization_ratio{serial=\"%s\"} NaN\n",
                    s->serial);
        else
            M_GAUGE_FMT("gpu_memory_bandwidth_utilization_ratio", s->serial,
                        "%.4f", s->mem_bw_util_pct / 100.0);
    }

    M_HEADER("gpu_ecc_sbe_aggregate_total", "counter",
             "ECC single-bit errors (aggregate lifetime counter; does not reset on driver reload)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_COUNTER_U64("gpu_ecc_sbe_aggregate_total",
                      slots[i].snap.serial, slots[i].snap.ecc_sbe_aggregate);
    }

    M_HEADER("gpu_ecc_dbe_aggregate_total", "counter",
             "ECC double-bit errors (aggregate lifetime counter; does not reset on driver reload)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_COUNTER_U64("gpu_ecc_dbe_aggregate_total",
                      slots[i].snap.serial, slots[i].snap.ecc_dbe_aggregate);
    }

    M_HEADER("gpu_retired_pages_sbe", "gauge",
             "GPU memory pages retired due to single-bit errors");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_retired_pages_sbe", slots[i].snap.serial,
                  (int)slots[i].snap.retired_pages_sbe);
    }

    M_HEADER("gpu_retired_pages_dbe", "gauge",
             "GPU memory pages retired due to double-bit errors");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_retired_pages_dbe", slots[i].snap.serial,
                  (int)slots[i].snap.retired_pages_dbe);
    }

    M_HEADER("gpu_row_remap_failures", "gauge",
             "Count of row remapping failures; any non-zero value indicates irreversible damage");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_row_remap_failures", slots[i].snap.serial,
                  (int)slots[i].snap.row_remap_failures);
    }

    M_HEADER("gpu_pending_row_remap", "gauge",
             "1 if a row remapping operation is pending (requires reboot to take effect)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_pending_row_remap", slots[i].snap.serial,
                  slots[i].snap.pending_row_remap);
    }

    /* ------------------------------------------------------------------ */
    /* 8. Interconnects                                                    */
    /* ------------------------------------------------------------------ */
    M_HEADER("gpu_pcie_link_gen", "gauge",
             "Current PCIe link generation");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_pcie_link_gen", slots[i].snap.serial,
                  slots[i].snap.pcie_link_gen);
    }

    M_HEADER("gpu_pcie_link_width", "gauge",
             "Current PCIe link width (number of lanes)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_pcie_link_width", slots[i].snap.serial,
                  slots[i].snap.pcie_link_width);
    }

    M_HEADER("gpu_pcie_replay_total", "counter",
             "PCIe replay counter (total)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_COUNTER_U64("gpu_pcie_replay_total",
                      slots[i].snap.serial, slots[i].snap.pcie_replay_count);
    }

    M_HEADER("gpu_nvlink_replay_total", "counter",
             "NVLink replay error counter (total)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_COUNTER_U64("gpu_nvlink_replay_total",
                      slots[i].snap.serial, slots[i].snap.nvlink_replay_count);
    }

    M_HEADER("gpu_nvlink_recovery_total", "counter",
             "NVLink recovery error counter (total)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_COUNTER_U64("gpu_nvlink_recovery_total",
                      slots[i].snap.serial, slots[i].snap.nvlink_recovery_count);
    }

    M_HEADER("gpu_nvlink_crc_total", "counter",
             "NVLink CRC flit error counter (total)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_COUNTER_U64("gpu_nvlink_crc_total",
                      slots[i].snap.serial, slots[i].snap.nvlink_crc_count);
    }

    M_HEADER("gpu_xid_errors_total", "counter",
             "XID error counter (total)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_COUNTER_U64("gpu_xid_errors_total",
                      slots[i].snap.serial, slots[i].snap.xid_count);
    }

    M_HEADER("gpu_xid_last_code", "gauge",
             "Last XID error code observed (0 if none)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_xid_last_code", slots[i].snap.serial,
                  (int)slots[i].snap.xid_last_code);
    }

    /* ------------------------------------------------------------------ */
    /* 9. Compute                                                          */
    /* ------------------------------------------------------------------ */
    M_HEADER("gpu_sm_clock_mhz", "gauge",
             "Current SM (shader) clock frequency (MHz)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_sm_clock_mhz", slots[i].snap.serial,
                    "%.1f", slots[i].snap.sm_clock_mhz);
    }

    M_HEADER("gpu_mem_clock_mhz", "gauge",
             "Current memory clock frequency (MHz)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_mem_clock_mhz", slots[i].snap.serial,
                    "%.1f", slots[i].snap.mem_clock_mhz);
    }

    M_HEADER("gpu_utilization_gpu_ratio", "gauge",
             "GPU compute utilization (0.0-1.0)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_utilization_gpu_ratio", slots[i].snap.serial,
                    "%.4f", slots[i].snap.util_gpu_pct / 100.0);
    }

    M_HEADER("gpu_utilization_memory_ratio", "gauge",
             "GPU memory controller utilization (0.0-1.0)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_utilization_memory_ratio", slots[i].snap.serial,
                    "%.4f", slots[i].snap.util_mem_pct / 100.0);
    }

    M_HEADER("gpu_pstate", "gauge",
             "Current GPU performance state (0=P0=maximum performance, 15=minimum)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_pstate", slots[i].snap.serial,
                  slots[i].snap.pstate);
    }

    /* ------------------------------------------------------------------ */
    /* 10. Baseline and probe                                              */
    /* ------------------------------------------------------------------ */
    M_HEADER("gpu_baseline_available", "gauge",
             "1 if a baseline file is present and parseable");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_baseline_available", slots[i].snap.serial,
                  slots[i].snap.baseline_available);
    }

    M_HEADER("gpu_baseline_valid", "gauge",
             "1 if the baseline passed all validation checks");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_baseline_valid", slots[i].snap.serial,
                  slots[i].snap.baseline_valid);
    }

    M_HEADER("gpu_baseline_serial_mismatch", "gauge",
             "1 if the baseline file serial does not match the device serial (hard fault)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_baseline_serial_mismatch", slots[i].snap.serial,
                  slots[i].snap.baseline_serial_mismatch);
    }

    M_HEADER("gpu_baseline_driver_mismatch", "gauge",
             "1 if the driver version changed since the baseline was established (soft warning)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_baseline_driver_mismatch", slots[i].snap.serial,
                  slots[i].snap.baseline_driver_mismatch);
    }

    M_HEADER("gpu_baseline_age_seconds", "gauge",
             "Seconds since the baseline was established (0 if baseline unavailable)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_COUNTER_U64("gpu_baseline_age_seconds",
                      slots[i].snap.serial, slots[i].snap.baseline_age_s);
    }

    M_HEADER("gpu_probe_available", "gauge",
             "1 if a probe result file is present and parseable");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_probe_available", slots[i].snap.serial,
                  slots[i].snap.probe_available);
    }

    M_HEADER("gpu_probe_result_stale", "gauge",
             "1 if the probe result is older than the configured TTL");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_probe_result_stale", slots[i].snap.serial,
                  slots[i].snap.probe_stale);
    }

    /* perf_drop_frac: NaN when no baseline or probe is available */
    M_HEADER("gpu_perf_drop_ratio", "gauge",
             "Performance/Watt drop relative to baseline (0.0=no drop); NaN if baseline or probe unavailable");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_NAN("gpu_perf_drop_ratio", slots[i].snap.serial,
                    slots[i].snap.perf_drop_frac);
    }

    /* ------------------------------------------------------------------ */
    /* 11. Exporter operational state                                      */
    /* ------------------------------------------------------------------ */

    /*
     * gpu_dcgm_available: ground truth lives in exporter_t.dcgm_available
     * (parent only).  Inferred here from mem_bw_util_pct being NaN.
     * Requires all GPUs to agree (DCGM connects once for all GPUs).
     */
    M_HEADER("gpu_dcgm_available", "gauge",
             "1 if DCGM is connected and responding (inferred from DCGM-only fields)");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_dcgm_available", slots[i].snap.serial,
                  !isnan(slots[i].snap.mem_bw_util_pct));
    }

    M_HEADER("gpu_health_last_poll_timestamp", "gauge",
             "Unix timestamp (seconds) of the last successful poll cycle for this GPU");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_FMT("gpu_health_last_poll_timestamp", slots[i].snap.serial,
                    "%.3f", slots[i].snap.last_poll_ms / 1000.0);
    }

    M_HEADER("gpu_present", "gauge",
             "1 if the GPU device is still visible; 0 if it disappeared mid-run");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_present", slots[i].snap.serial,
                  slots[i].snap.gpu_present);
    }

    M_HEADER("gpu_available", "gauge",
             "1 if NVML is responding for this GPU; 0 if above error threshold");
    for (int i = 0; i < n; i++) {
        if (!slots[i].received) continue;
        M_GAUGE_I("gpu_available", slots[i].snap.serial,
                  slots[i].snap.gpu_available);
    }

    free(slots);
    return pos;
}

#undef M_HEADER
#undef M_GAUGE_D
#undef M_GAUGE_FMT
#undef M_GAUGE_I
#undef M_COUNTER_U64
#undef M_GAUGE_NAN

/* --------------------------------------------------------------------------
 * /live staleness check
 * Returns 1 (alive) or 0 (stale).
 * -------------------------------------------------------------------------- */

static int check_liveness(void) {
    uint64_t now_ms = time_now_ms();
    pthread_mutex_lock(&g_lock);
    int ok = 1;
    for (int i = 0; i < g_num_gpus; i++) {
        if (!g_slots[i].received) continue;
        /* last_poll_ms is written with time_now_ms() — same CLOCK_MONOTONIC */
        uint64_t age = now_ms - g_slots[i].snap.last_poll_ms;
        if (age > (uint64_t)g_stale_ms) {
            ok = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/* --------------------------------------------------------------------------
 * HTTP helpers
 * -------------------------------------------------------------------------- */

static int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_response(int fd, int status,
                           const char *body, size_t body_len,
                           const char *content_type) {
    const char *status_text;
    switch (status) {
    case 200: status_text = "OK";                    break;
    case 404: status_text = "Not Found";             break;
    case 405: status_text = "Method Not Allowed";    break;
    case 500: status_text = "Internal Server Error"; break;
    case 503: status_text = "Service Unavailable";   break;
    default:  status_text = "Unknown";               break;
    }

    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        status, status_text, content_type, body_len);
    if (hlen < 0 || (size_t)hlen >= sizeof(header))
        return -1;

    if (write_all(fd, header, (size_t)hlen) < 0) return -1;
    if (body_len > 0 && write_all(fd, body, body_len) < 0) return -1;
    return 0;
}

/*
 * Read incoming HTTP request and extract the first line's method and path.
 * Reads in chunks until the first newline is found or buf is full.
 * method_out and path_out point into buf on success.
 * Returns 0 on success, -1 on error or malformed request.
 */
static int read_request_line(int fd, char *buf, size_t cap,
                              char **method_out, char **path_out) {
    size_t pos = 0;
    while (pos < cap - 1) {
        ssize_t n = read(fd, buf + pos, cap - pos - 1);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        pos += (size_t)n;
        if (memchr(buf, '\n', pos)) break;
    }
    buf[pos] = '\0';

    char *lf = memchr(buf, '\n', pos);
    if (!lf) return -1;
    *lf = '\0';
    if (lf > buf && *(lf - 1) == '\r')
        *(lf - 1) = '\0';

    /* Parse: "GET /path HTTP/1.1" */
    char *sp1 = strchr(buf, ' ');
    if (!sp1) return -1;
    *sp1 = '\0';
    *method_out = buf;
    *path_out   = sp1 + 1;

    char *sp2 = strchr(*path_out, ' ');
    if (sp2) *sp2 = '\0';

    return 0;
}

/* --------------------------------------------------------------------------
 * Request dispatch
 * -------------------------------------------------------------------------- */

static void handle_request(int fd, const char *method, const char *path) {
    if (strcmp(method, "GET") != 0) {
        const char body[] = "Method Not Allowed\n";
        write_response(fd, 405, body, sizeof(body) - 1, "text/plain");
        return;
    }

    if (strcmp(path, "/metrics") == 0) {
        char *buf = malloc(METRICS_BUF_SIZE);
        if (!buf) {
            const char body[] = "Internal Server Error\n";
            write_response(fd, 500, body, sizeof(body) - 1, "text/plain");
            return;
        }
        size_t len = render_metrics(buf, METRICS_BUF_SIZE);
        write_response(fd, 200, buf, len,
                       "text/plain; version=0.0.4; charset=utf-8");
        free(buf);

    } else if (strcmp(path, "/ready") == 0) {
        if (g_all_ready) {
            const char body[] = "ready\n";
            write_response(fd, 200, body, sizeof(body) - 1, "text/plain");
        } else {
            const char body[] = "not ready\n";
            write_response(fd, 503, body, sizeof(body) - 1, "text/plain");
        }

    } else if (strcmp(path, "/live") == 0) {
        if (check_liveness()) {
            const char body[] = "alive\n";
            write_response(fd, 200, body, sizeof(body) - 1, "text/plain");
        } else {
            const char body[] = "stale\n";
            write_response(fd, 503, body, sizeof(body) - 1, "text/plain");
        }

    } else {
        const char body[] = "Not Found\n";
        write_response(fd, 404, body, sizeof(body) - 1, "text/plain");
    }
}

/* --------------------------------------------------------------------------
 * HTTP accept loop
 * Uses select() with a 1-second timeout to allow g_running checks.
 * -------------------------------------------------------------------------- */

static void http_accept_loop(int listen_fd) {
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int r = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            log_error("http: select: %s", strerror(errno));
            break;
        }
        if (r == 0) continue; /* timeout — loop to check g_running */

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            log_error("http: accept: %s", strerror(errno));
            continue;
        }

        /* Bound slow-client backpressure: SO_SNDTIMEO and SO_RCVTIMEO. */
        struct timeval timeout = { .tv_sec = SEND_TIMEOUT_S, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout));
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout));

        char req_buf[REQ_BUF_SIZE];
        char *method, *path;
        if (read_request_line(client_fd, req_buf, sizeof(req_buf),
                              &method, &path) == 0) {
            handle_request(client_fd, method, path);
        }
        close(client_fd);
    }
}

/* --------------------------------------------------------------------------
 * Listen socket setup
 * -------------------------------------------------------------------------- */

static int bind_listen(const char *addr, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_error("http: socket: %s", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);

    if (inet_aton(addr, &sa.sin_addr) == 0) {
        log_error("http: invalid listen address '%s'", addr);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        log_error("http: bind %s:%d: %s", addr, port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, LISTEN_BACKLOG) < 0) {
        log_error("http: listen: %s", strerror(errno));
        close(fd);
        return -1;
    }

    log_info("http: listening on %s:%d", addr, port);
    return fd;
}

/* --------------------------------------------------------------------------
 * http_child_run — entry point
 * -------------------------------------------------------------------------- */

void http_child_run(int ipc_fd, const gpu_config_t *cfg) {
    /* 1. Read gpu_ipc_init_t — learn num_gpus before doing anything else. */
    gpu_ipc_init_t init;
    {
        char *p = (char *)&init;
        size_t rem = sizeof(init);
        while (rem > 0) {
            ssize_t n = read(ipc_fd, p, rem);
            if (n <= 0) {
                log_error("http: failed to read IPC init message: %s",
                          n == 0 ? "EOF" : strerror(errno));
                exit(1);
            }
            p += n;
            rem -= (size_t)n;
        }
    }

    g_num_gpus = (int)init.num_gpus;
    if (g_num_gpus <= 0 || g_num_gpus > 256) {
        log_error("http: invalid num_gpus %d in IPC init message", g_num_gpus);
        exit(1);
    }

    /* 2. Allocate snapshot slot array. */
    g_slots = calloc((size_t)g_num_gpus, sizeof(http_slot_t));
    if (!g_slots) {
        log_error("http: calloc for slot array failed");
        exit(1);
    }

    /* 3. Staleness threshold for /live: 3 × poll_interval_s, min 5 s. */
    g_stale_ms = cfg->poll_interval_s * 3 * 1000;
    if (g_stale_ms < 5000)
        g_stale_ms = 5000;

    /* 4. Signal handlers. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    /* Ignore SIGPIPE — write() errors from closed client connections
       are handled by checking the return value. */
    signal(SIGPIPE, SIG_IGN);

    /* 5. Spawn IPC receiver thread. */
    int *fd_copy = malloc(sizeof(int));
    if (!fd_copy) {
        log_error("http: malloc failed");
        exit(1);
    }
    *fd_copy = ipc_fd;

    pthread_t recv_thread;
    if (pthread_create(&recv_thread, NULL, ipc_receiver_thread, fd_copy) != 0) {
        log_error("http: failed to create IPC receiver thread: %s",
                  strerror(errno));
        exit(1);
    }
    pthread_detach(recv_thread);

    /* 6. Bind and listen.  Hard fail if port is in use (checked before READY). */
    int listen_fd = bind_listen(cfg->listen_addr, cfg->listen_port);
    if (listen_fd < 0) {
        log_error("http: could not bind listen port — exiting");
        exit(1);
    }

    log_info("http: child process started, serving %d GPU(s)", g_num_gpus);

    /* 7. HTTP accept loop — blocks until SIGTERM or IPC EOF. */
    http_accept_loop(listen_fd);

    close(listen_fd);
    free(g_slots);
    log_info("http: child process exiting");
    exit(0);
}
