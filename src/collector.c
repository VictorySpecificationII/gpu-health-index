#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "collector.h"
#include "snapshot.h"
#include "scorer.h"
#include "state.h"
#include "util.h"

/* =========================================================================
 * Ring buffer
 * ========================================================================= */

void ring_push(gpu_ring_t *ring, const gpu_sample_t *s)
{
    ring->samples[ring->head] = *s;
    ring->head = (ring->head + 1) % ring->capacity;
    if (ring->count < ring->capacity)
        ring->count++;
}

/* =========================================================================
 * Poll thread internals
 * ========================================================================= */

typedef struct {
    gpu_ctx_t  *ctx;
    exporter_t *exp;
} poll_args_t;

/*
 * Interruptible sleep: sleeps in 100 ms chunks so the thread can react to
 * ctx->stop within 100 ms rather than waiting out the full poll interval.
 */
static void poll_sleep(int seconds, volatile int *stop)
{
    struct timespec ts = {0, 100000000L};   /* 100 ms */
    int ticks = seconds * 10;
    for (int i = 0; i < ticks && !*stop; i++)
        nanosleep(&ts, NULL);
}

/*
 * Make all NVML calls for one poll cycle.
 * Populates sample (ring fields) and state (point-in-time fields).
 * Returns NVML_SUCCESS (0) on success, the first NVML error code on failure.
 */
static int do_nvml_poll(const nvml_vtable_t *vt, void *dev,
                        gpu_sample_t *sample, gpu_state_t *state)
{
    int ret;
    unsigned int u = 0;
    unsigned long long ull = 0;

    /* Timestamp first so it reflects the start of this cycle */
    sample->timestamp_ms = time_now_ms();

    /* --- Thermal ----------------------------------------------------------- */
    if ((ret = vt->DeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &u)) != 0)
        return ret;
    sample->temp_c = (double)u;

    u = 0;
    /* HBM temp: optional — not available on all SKUs; ignore failure */
    if (vt->DeviceGetTemperature(dev, NVML_TEMPERATURE_MEM, &u) == 0)
        sample->hbm_temp_c = (double)u;

    /* --- Power ------------------------------------------------------------- */
    if ((ret = vt->DeviceGetPowerUsage(dev, &u)) != 0)
        return ret;
    sample->power_w = (double)u / 1000.0;   /* mW → W */

    if ((ret = vt->DeviceGetEnforcedPowerLimit(dev, &u)) != 0)
        return ret;
    sample->power_limit_w = (double)u / 1000.0;   /* mW → W */

    /* --- SM clock ---------------------------------------------------------- */
    if ((ret = vt->DeviceGetClockInfo(dev, NVML_CLOCK_SM, &u)) != 0)
        return ret;
    sample->sm_clock_mhz = (double)u;

    /* --- ECC volatile (ring buffer: rate computation needs deltas) --------- */
    if ((ret = vt->DeviceGetTotalEccErrors(
                dev, NVML_MEMORY_ERROR_TYPE_CORRECTED,
                NVML_VOLATILE_ECC, &ull)) != 0)
        return ret;
    sample->ecc_sbe_volatile = ull;

    if ((ret = vt->DeviceGetTotalEccErrors(
                dev, NVML_MEMORY_ERROR_TYPE_UNCORRECTED,
                NVML_VOLATILE_ECC, &ull)) != 0)
        return ret;
    sample->ecc_dbe_volatile = ull;

    /* --- State: ECC aggregate ---------------------------------------------- */
    if ((ret = vt->DeviceGetTotalEccErrors(
                dev, NVML_MEMORY_ERROR_TYPE_CORRECTED,
                NVML_AGGREGATE_ECC, &ull)) != 0)
        return ret;
    state->ecc_sbe_aggregate = ull;

    if ((ret = vt->DeviceGetTotalEccErrors(
                dev, NVML_MEMORY_ERROR_TYPE_UNCORRECTED,
                NVML_AGGREGATE_ECC, &ull)) != 0)
        return ret;
    state->ecc_dbe_aggregate = ull;

    /* --- State: retired pages (optional) ----------------------------------- */
    if (vt->DeviceGetRetiredPages) {
        unsigned int page_count;
        page_count = UINT32_MAX;
        ret = vt->DeviceGetRetiredPages(
                dev, NVML_PAGE_RETIREMENT_CAUSE_MULTIPLE_SINGLE_BIT_ECC_ERRORS,
                &page_count, NULL);
        if (ret == 0)
            state->retired_pages_sbe = page_count;

        page_count = UINT32_MAX;
        ret = vt->DeviceGetRetiredPages(
                dev, NVML_PAGE_RETIREMENT_CAUSE_DOUBLE_BIT_ECC_ERRORS,
                &page_count, NULL);
        if (ret == 0)
            state->retired_pages_dbe = page_count;
    }

    /* --- State: row remap (Ampere+, optional) ------------------------------ */
    if (vt->DeviceGetRemappedRows) {
        unsigned int corr, uncorr, pending, failed;
        if (vt->DeviceGetRemappedRows(dev, &corr, &uncorr,
                                      &pending, &failed) == 0) {
            state->row_remap_failures = uncorr;
            state->pending_row_remap  = (int)pending;
        }
    }

    /* --- State: memory capacity -------------------------------------------- */
    {
        nvml_memory_t mem;
        if ((ret = vt->DeviceGetMemoryInfo(dev, &mem)) != 0)
            return ret;
        state->mem_used_bytes  = mem.used;
        state->mem_free_bytes  = mem.free;
        state->mem_total_bytes = mem.total;
    }

    /* --- State: clocks ----------------------------------------------------- */
    if ((ret = vt->DeviceGetClockInfo(dev, NVML_CLOCK_MEM, &u)) != 0)
        return ret;
    state->mem_clock_mhz = (double)u;

    /* --- State: utilization ------------------------------------------------ */
    {
        nvml_utilization_t util;
        if ((ret = vt->DeviceGetUtilizationRates(dev, &util)) != 0)
            return ret;
        state->util_gpu_pct = (int)util.gpu;
        state->util_mem_pct = (int)util.memory;
    }

    /* --- State: throttle reasons ------------------------------------------- */
    {
        unsigned long long reasons = 0;
        if ((ret = vt->DeviceGetCurrentClocksThrottleReasons(
                    dev, &reasons)) != 0)
            return ret;
        state->throttle_sw_power_cap   = !!(reasons & NVML_THROTTLE_REASON_SW_POWER_CAP);
        state->throttle_hw_slowdown    = !!(reasons & NVML_THROTTLE_REASON_HW_SLOWDOWN);
        state->throttle_hw_power_brake = !!(reasons & NVML_THROTTLE_REASON_HW_POWER_BRAKE);
        state->throttle_sw_thermal     = !!(reasons & NVML_THROTTLE_REASON_SW_THERMAL);
        state->throttle_hw_thermal     = !!(reasons & NVML_THROTTLE_REASON_HW_THERMAL);
    }

    /* --- State: performance state ------------------------------------------ */
    {
        int pstate = 0;
        if ((ret = vt->DeviceGetPerformanceState(dev, &pstate)) != 0)
            return ret;
        state->pstate = pstate;
    }

    /* --- State: PCIe current link ------------------------------------------ */
    if ((ret = vt->DeviceGetCurrPcieLinkGeneration(dev, &u)) != 0)
        return ret;
    state->pcie_link_gen = (int)u;

    if ((ret = vt->DeviceGetCurrPcieLinkWidth(dev, &u)) != 0)
        return ret;
    state->pcie_link_width = (int)u;

    /* --- State: PCIe replay (optional) ------------------------------------- */
    if (vt->DeviceGetPcieReplayCounter) {
        if (vt->DeviceGetPcieReplayCounter(dev, &u) == 0)
            state->pcie_replay_count = u;
    }

    /* --- State: fan speed (optional — absent on liquid-cooled SXM) --------- */
    state->fan_speed_pct = -1;   /* default: unavailable */
    if (vt->DeviceGetFanSpeed) {
        if (vt->DeviceGetFanSpeed(dev, &u) == 0)
            state->fan_speed_pct = (int)u;
    }

    return 0;   /* NVML_SUCCESS */
}

/*
 * Update gpu_state_t fields from a completed dcgm_fields_t poll result.
 * Only writes fields that DCGM reported as available (non-sentinel).
 */
static void apply_dcgm_state(gpu_state_t *state, const dcgm_fields_t *d)
{
    if (d->power_w != DCGM_FIELD_UNAVAILABLE_DBL)
        state->board_power_w = d->power_w;
    if (d->energy_j != DCGM_FIELD_UNAVAILABLE_DBL)
        state->energy_j = d->energy_j;
    if (d->mem_bw_util_pct != DCGM_FIELD_UNAVAILABLE_DBL)
        state->mem_bw_util_pct = d->mem_bw_util_pct;
    if (d->power_violation_us != DCGM_FIELD_UNAVAILABLE_U64)
        state->power_violation_us = d->power_violation_us;
    if (d->thermal_violation_us != DCGM_FIELD_UNAVAILABLE_U64)
        state->thermal_violation_us = d->thermal_violation_us;
    if (d->nvlink_replay != DCGM_FIELD_UNAVAILABLE_U64)
        state->nvlink_replay_count = d->nvlink_replay;
    if (d->nvlink_recovery != DCGM_FIELD_UNAVAILABLE_U64)
        state->nvlink_recovery_count = d->nvlink_recovery;
    if (d->nvlink_crc != DCGM_FIELD_UNAVAILABLE_U64)
        state->nvlink_crc_count = d->nvlink_crc;
    if (d->xid_count != DCGM_FIELD_UNAVAILABLE_U64)
        state->xid_count = d->xid_count;
    if (d->xid_last_code != DCGM_FIELD_UNAVAILABLE_U32)
        state->xid_last_code = d->xid_last_code;
    if (d->pcie_replay != DCGM_FIELD_UNAVAILABLE_U64)
        state->pcie_replay_count = d->pcie_replay;
    if (d->row_remap_failures != DCGM_FIELD_UNAVAILABLE_U32)
        state->row_remap_failures = d->row_remap_failures;
}

/* -------------------------------------------------------------------------
 * poll_thread — main loop
 * ------------------------------------------------------------------------- */

static void *poll_thread(void *arg)
{
    poll_args_t *args = (poll_args_t *)arg;
    gpu_ctx_t   *ctx  = args->ctx;
    exporter_t  *exp  = args->exp;
    free(args);

    const gpu_config_t *cfg = &exp->cfg;

    log_debug("collector[%d]: poll thread started", ctx->gpu_index);

    while (!ctx->stop) {

        /* Skip polling if GPU has been lost permanently */
        if (!ctx->gpu_present) {
            poll_sleep(cfg->poll_interval_s, &ctx->stop);
            continue;
        }

        /* Back-off: if above soft error threshold, wait for retry interval */
        if (!ctx->gpu_available) {
            uint64_t now = time_now_ms();
            uint64_t retry_ms = (uint64_t)cfg->nvml_retry_interval_s * 1000ULL;
            if (now - ctx->last_retry_ms < retry_ms) {
                poll_sleep(cfg->poll_interval_s, &ctx->stop);
                continue;
            }
            ctx->last_retry_ms = now;
            log_info("collector[%d]: retrying after error back-off", ctx->gpu_index);
        }

        /* --- NVML poll ---------------------------------------------------- */
        gpu_sample_t sample;
        gpu_state_t  state;
        memset(&sample, 0, sizeof(sample));
        memset(&state,  0, sizeof(state));

        /* Copy static pcie_link_gen/width_max from identity into state */
        state.pcie_link_gen_max   = ctx->pcie_link_gen_max;
        state.pcie_link_width_max = ctx->pcie_link_width_max;

        /* Initialise DCGM-sourced fields to NaN/sentinel before NVML poll
           so they are defined even when DCGM is unavailable */
        state.board_power_w      = 0.0;
        state.energy_j           = 0.0;
        state.mem_bw_util_pct    = 0.0;

        int nvml_ret = do_nvml_poll(&exp->nvml, ctx->nvml_handle,
                                    &sample, &state);

        if (nvml_ret != 0) {
            ctx->consecutive_errors++;
            ctx->collector_errors_total++;

            if (NVML_IS_HARD_ERROR(nvml_ret)) {
                ctx->consecutive_hard_errors++;
                log_error("collector[%d]: hard NVML error %d (streak=%d)",
                          ctx->gpu_index, nvml_ret,
                          ctx->consecutive_hard_errors);
                if (ctx->consecutive_hard_errors
                        >= cfg->nvml_hard_error_threshold) {
                    ctx->gpu_present = 0;
                    log_error("collector[%d]: GPU declared lost — "
                              "stopping poll", ctx->gpu_index);
                    ctx->ready = 1;   /* unblock startup waiter */
                    break;
                }
            } else {
                log_error("collector[%d]: NVML error %d (streak=%d)",
                          ctx->gpu_index, nvml_ret,
                          ctx->consecutive_errors);
            }

            if (ctx->consecutive_errors >= cfg->nvml_error_threshold) {
                ctx->gpu_available = 0;
                log_error("collector[%d]: marking unavailable after %d "
                          "consecutive errors",
                          ctx->gpu_index, ctx->consecutive_errors);
            }

            ctx->ready = 1;
            poll_sleep(cfg->poll_interval_s, &ctx->stop);
            continue;
        }

        /* NVML poll succeeded — reset error counters */
        ctx->consecutive_errors      = 0;
        ctx->consecutive_hard_errors = 0;
        ctx->gpu_available           = 1;

        /* --- DCGM poll (optional) ----------------------------------------- */
        if (exp->dcgm_available) {
            dcgm_fields_t dcgm_out;
            if (dcgm_poll(&exp->dcgm, exp->dcgm_handle,
                          ctx->gpu_index, &dcgm_out) == 0) {
                apply_dcgm_state(&state, &dcgm_out);
            } else {
                log_debug("collector[%d]: dcgm_poll failed this cycle",
                          ctx->gpu_index);
            }
        }

        /* --- Baseline hot-reload (inotify) --------------------------------- */
        if (exp->baseline_inotify_fd >= 0 &&
                baseline_inotify_check(exp->baseline_inotify_fd) == 1) {
            pthread_mutex_lock(&ctx->files_mutex);
            gpu_baseline_t new_baseline;
            if (baseline_load(cfg->baseline_dir, ctx->serial,
                              &new_baseline) == 0) {
                /* Detect driver version change */
                new_baseline.driver_mismatch =
                    (strncmp(new_baseline.driver_version,
                             ctx->driver_version,
                             sizeof(ctx->driver_version)) != 0) ? 1 : 0;
                ctx->baseline = new_baseline;
                log_info("collector[%d]: baseline reloaded", ctx->gpu_index);
            }
            pthread_mutex_unlock(&ctx->files_mutex);
        }

        /* --- Write ring sample (under ring_mutex) -------------------------- */
        pthread_mutex_lock(&ctx->ring_mutex);
        ring_push(&ctx->ring, &sample);
        pthread_mutex_unlock(&ctx->ring_mutex);

        /* --- Update point-in-time state (under state_mutex) ---------------- */
        pthread_mutex_lock(&ctx->state_mutex);
        ctx->state = state;
        pthread_mutex_unlock(&ctx->state_mutex);

        /* --- Score --------------------------------------------------------- */
        gpu_score_result_t score;
        pthread_mutex_lock(&ctx->ring_mutex);
        pthread_mutex_lock(&ctx->state_mutex);
        pthread_mutex_lock(&ctx->files_mutex);
        score_gpu(&ctx->ring, &ctx->state,
                  &ctx->baseline, &ctx->probe, cfg, &score);
        pthread_mutex_unlock(&ctx->files_mutex);
        pthread_mutex_unlock(&ctx->state_mutex);
        pthread_mutex_unlock(&ctx->ring_mutex);

        /* --- Snapshot + IPC ----------------------------------------------- */
        snapshot_update(ctx, &score, exp->dcgm_available);
        if (snapshot_send(exp->parent_fd, ctx) != 0) {
            log_error("collector[%d]: snapshot_send failed",
                      ctx->gpu_index);
        }

        /* Signal startup sequence that we are operational */
        ctx->ready = 1;

        poll_sleep(cfg->poll_interval_s, &ctx->stop);
    }

    log_debug("collector[%d]: poll thread exiting", ctx->gpu_index);
    return NULL;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int collector_start(gpu_ctx_t *ctx, exporter_t *exp)
{
    const gpu_config_t *cfg = &exp->cfg;

    /* Allocate ring buffer */
    int capacity = cfg->window_s / cfg->poll_interval_s;
    if (capacity < 1)
        capacity = 1;

    ctx->ring.samples = calloc((size_t)capacity, sizeof(gpu_sample_t));
    if (!ctx->ring.samples) {
        log_error("collector[%d]: ring buffer allocation failed", ctx->gpu_index);
        return -1;
    }
    ctx->ring.capacity = capacity;
    ctx->ring.head     = 0;
    ctx->ring.count    = 0;

    /* Initialise all mutexes */
    pthread_mutex_init(&ctx->ring_mutex,     NULL);
    pthread_mutex_init(&ctx->state_mutex,    NULL);
    pthread_mutex_init(&ctx->snapshot_mutex, NULL);
    pthread_mutex_init(&ctx->files_mutex,    NULL);

    /* Initialise state */
    ctx->ready                   = 0;
    ctx->stop                    = 0;
    ctx->gpu_present             = 1;
    ctx->gpu_available           = 1;
    ctx->consecutive_errors      = 0;
    ctx->consecutive_hard_errors = 0;
    ctx->last_retry_ms           = 0;
    ctx->collector_errors_total  = 0;
    memset(&ctx->state,    0, sizeof(ctx->state));
    memset(&ctx->snapshot, 0, sizeof(ctx->snapshot));

    /* Inherit static PCIe limits into initial state */
    ctx->state.pcie_link_gen_max   = ctx->pcie_link_gen_max;
    ctx->state.pcie_link_width_max = ctx->pcie_link_width_max;
    ctx->state.fan_speed_pct       = -1;

    /* Pass ctx + exp to the thread via a heap-allocated args struct */
    poll_args_t *args = malloc(sizeof(*args));
    if (!args) {
        log_error("collector[%d]: failed to allocate thread args",
                  ctx->gpu_index);
        free(ctx->ring.samples);
        ctx->ring.samples = NULL;
        return -1;
    }
    args->ctx = ctx;
    args->exp = exp;

    int rc = pthread_create(&ctx->thread, NULL, poll_thread, args);
    if (rc != 0) {
        log_error("collector[%d]: pthread_create failed: %s",
                  ctx->gpu_index, strerror(rc));
        free(args);
        free(ctx->ring.samples);
        ctx->ring.samples = NULL;
        return -1;
    }

    log_debug("collector[%d]: poll thread spawned", ctx->gpu_index);
    return 0;
}

void collector_stop(gpu_ctx_t *ctx)
{
    ctx->stop = 1;
    pthread_join(ctx->thread, NULL);
    log_debug("collector[%d]: poll thread joined", ctx->gpu_index);
}
