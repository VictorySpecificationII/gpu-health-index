#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

#include "snapshot.h"
#include "util.h"

/* -------------------------------------------------------------------------
 * snapshot_update
 *
 * Assembles gpu_snapshot_t from current ctx state + score result.
 * The poll thread is the sole writer of ctx->state, ctx->baseline,
 * ctx->probe, and ctx->ring, so we read them without their individual
 * mutexes here — the caller (poll thread) has just finished writing.
 * We only acquire snapshot_mutex to protect the HTTP child's reader.
 * ------------------------------------------------------------------------- */

void snapshot_update(gpu_ctx_t *ctx,
                     const gpu_score_result_t *score,
                     int dcgm_available)
{
    (void)dcgm_available;   /* informational — not needed for snapshot fields */

    gpu_snapshot_t s;
    memset(&s, 0, sizeof(s));

    /* --- Identity (immutable after startup) -------------------------------- */
    memcpy(s.serial,         ctx->serial,         sizeof(s.serial));
    memcpy(s.uuid,           ctx->uuid,           sizeof(s.uuid));
    memcpy(s.gpu_model,      ctx->gpu_model,      sizeof(s.gpu_model));
    memcpy(s.driver_version, ctx->driver_version, sizeof(s.driver_version));
    s.gpu_index           = ctx->gpu_index;
    s.identity_source     = ctx->identity_source;
    s.pcie_link_gen_max   = ctx->pcie_link_gen_max;
    s.pcie_link_width_max = ctx->pcie_link_width_max;

    /* --- Score -------------------------------------------------------------- */
    s.score                 = score->score;
    s.classification        = score->classification;
    s.reason_mask           = score->reason_mask;
    s.telemetry_ok          = score->telemetry_ok;

    /* --- Pre-computed ring stats ------------------------------------------- */
    s.temp_p95_c            = score->temp_p95_c;
    s.hbm_temp_p95_c        = score->hbm_temp_p95_c;
    s.clk_std_mhz           = score->clk_std_mhz;
    s.power_saturation_frac = score->power_saturation_frac;
    s.ecc_sbe_rate_per_hour = score->ecc_sbe_rate_per_hour;
    s.ecc_dbe_in_window     = score->ecc_dbe_in_window;

    /* --- Perf/W drop ------------------------------------------------------- */
    s.perf_drop_frac    = score->perf_drop_frac;
    s.probe_available   = ctx->probe.available;
    s.probe_stale       = ctx->probe.stale;

    /* --- Baseline health --------------------------------------------------- */
    s.baseline_available       = ctx->baseline.available;
    s.baseline_valid           = ctx->baseline.valid;
    s.baseline_serial_mismatch = ctx->baseline.serial_mismatch;
    s.baseline_driver_mismatch = ctx->baseline.driver_mismatch;
    if (ctx->baseline.available && ctx->baseline.established_at_s > 0) {
        uint64_t now_s = (uint64_t)time_now_s();
        s.baseline_age_s = (now_s > ctx->baseline.established_at_s)
                           ? (now_s - ctx->baseline.established_at_s)
                           : 0;
    }

    /* --- Raw current state (for /metrics) ---------------------------------- */
    const gpu_state_t *st = &ctx->state;
    s.ecc_sbe_aggregate     = st->ecc_sbe_aggregate;
    s.ecc_dbe_aggregate     = st->ecc_dbe_aggregate;
    s.retired_pages_sbe     = st->retired_pages_sbe;
    s.retired_pages_dbe     = st->retired_pages_dbe;
    s.row_remap_failures    = st->row_remap_failures;
    s.pending_row_remap     = st->pending_row_remap;
    s.mem_used_bytes        = st->mem_used_bytes;
    s.mem_free_bytes        = st->mem_free_bytes;
    s.mem_total_bytes       = st->mem_total_bytes;
    s.mem_bw_util_pct       = st->mem_bw_util_pct;
    s.pcie_link_gen         = st->pcie_link_gen;
    s.pcie_link_width       = st->pcie_link_width;
    s.pcie_replay_count     = st->pcie_replay_count;
    s.nvlink_replay_count   = st->nvlink_replay_count;
    s.nvlink_recovery_count = st->nvlink_recovery_count;
    s.nvlink_crc_count      = st->nvlink_crc_count;
    s.xid_count             = st->xid_count;
    s.xid_last_code         = st->xid_last_code;
    s.board_power_w         = st->board_power_w;
    s.power_w               = (ctx->ring.count > 0)
                              ? ctx->ring.samples[(ctx->ring.head - 1 + ctx->ring.capacity)
                                                  % ctx->ring.capacity].power_w
                              : 0.0;
    s.power_limit_w         = (ctx->ring.count > 0)
                              ? ctx->ring.samples[(ctx->ring.head - 1 + ctx->ring.capacity)
                                                  % ctx->ring.capacity].power_limit_w
                              : 0.0;
    s.energy_j              = st->energy_j;
    s.power_violation_us    = st->power_violation_us;
    s.thermal_violation_us  = st->thermal_violation_us;
    s.throttle_sw_power_cap = st->throttle_sw_power_cap;
    s.throttle_hw_slowdown  = st->throttle_hw_slowdown;
    s.throttle_hw_power_brake = st->throttle_hw_power_brake;
    s.throttle_sw_thermal   = st->throttle_sw_thermal;
    s.throttle_hw_thermal   = st->throttle_hw_thermal;
    s.sm_clock_mhz          = (ctx->ring.count > 0)
                              ? ctx->ring.samples[(ctx->ring.head - 1 + ctx->ring.capacity)
                                                  % ctx->ring.capacity].sm_clock_mhz
                              : 0.0;
    s.mem_clock_mhz         = st->mem_clock_mhz;
    s.util_gpu_pct          = st->util_gpu_pct;
    s.util_mem_pct          = st->util_mem_pct;
    s.pstate                = st->pstate;
    s.temp_c                = (ctx->ring.count > 0)
                              ? ctx->ring.samples[(ctx->ring.head - 1 + ctx->ring.capacity)
                                                  % ctx->ring.capacity].temp_c
                              : 0.0;
    s.hbm_temp_c            = (ctx->ring.count > 0)
                              ? ctx->ring.samples[(ctx->ring.head - 1 + ctx->ring.capacity)
                                                  % ctx->ring.capacity].hbm_temp_c
                              : 0.0;
    s.fan_speed_pct         = st->fan_speed_pct;

    /* --- Exporter health --------------------------------------------------- */
    s.last_poll_ms   = time_now_ms();
    s.gpu_present    = ctx->gpu_present;
    s.gpu_available  = ctx->gpu_available;

    /* Publish under snapshot_mutex */
    pthread_mutex_lock(&ctx->snapshot_mutex);
    ctx->snapshot = s;
    pthread_mutex_unlock(&ctx->snapshot_mutex);
}

/* -------------------------------------------------------------------------
 * snapshot_send
 *
 * Copies the snapshot under mutex and writes one gpu_ipc_msg_t to fd.
 * Uses a retry loop for EINTR.
 * ------------------------------------------------------------------------- */

int snapshot_send(int fd, gpu_ctx_t *ctx)
{
    gpu_ipc_msg_t msg;
    msg.gpu_index = (int32_t)ctx->gpu_index;

    pthread_mutex_lock(&ctx->snapshot_mutex);
    msg.snapshot = ctx->snapshot;
    pthread_mutex_unlock(&ctx->snapshot_mutex);

    const char *buf = (const char *)&msg;
    size_t remaining = sizeof(msg);
    while (remaining > 0) {
        ssize_t n = write(fd, buf, remaining);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            log_error("snapshot: write to child failed");
            return -1;
        }
        buf       += (size_t)n;
        remaining -= (size_t)n;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * snapshot_recv
 *
 * Reads one gpu_ipc_msg_t from fd.  Blocks until complete.
 * Returns -1 on EOF or read error.
 * ------------------------------------------------------------------------- */

int snapshot_recv(int fd, gpu_ipc_msg_t *msg)
{
    char *buf = (char *)msg;
    size_t remaining = sizeof(*msg);
    while (remaining > 0) {
        ssize_t n = read(fd, buf, remaining);
        if (n == 0) {
            /* EOF — parent closed the socket */
            return -1;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            log_error("snapshot: read from parent failed");
            return -1;
        }
        buf       += (size_t)n;
        remaining -= (size_t)n;
    }
    return 0;
}
