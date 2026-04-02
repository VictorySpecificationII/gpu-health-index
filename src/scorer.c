#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "scorer.h"
#include "util.h"

/* =========================================================================
 * Ring buffer statistics
 * ========================================================================= */

/* Extract a double field from sample i (where i is a logical index,
 * not a ring position). */
static inline double sample_field(const gpu_ring_t *ring,
                                   int i, size_t field_offset)
{
    int idx = (ring->head - ring->count + i + ring->capacity) % ring->capacity;
    const char *base = (const char *)&ring->samples[idx];
    double val;
    memcpy(&val, base + field_offset, sizeof(double));
    return val;
}

double ring_mean(const gpu_ring_t *ring, size_t field_offset)
{
    if (ring->count == 0)
        return NAN;

    double sum = 0.0;
    for (int i = 0; i < ring->count; i++)
        sum += sample_field(ring, i, field_offset);

    return sum / ring->count;
}

double ring_stddev(const gpu_ring_t *ring, size_t field_offset)
{
    if (ring->count < 2)
        return NAN;

    double mean = ring_mean(ring, field_offset);
    double sq_sum = 0.0;

    for (int i = 0; i < ring->count; i++) {
        double d = sample_field(ring, i, field_offset) - mean;
        sq_sum += d * d;
    }

    return sqrt(sq_sum / (double)(ring->count - 1));
}

/* Comparison function for qsort — ascending double order. */
static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return  1;
    return 0;
}

double ring_p95(const gpu_ring_t *ring, size_t field_offset)
{
    if (ring->count == 0)
        return NAN;

    /* Stack-allocate up to 300 samples (max window_s=3600, poll=1s → 3600
     * slots, but we only call p95 with the ring as-is). Use heap if large. */
    double *vals;
    double  stack_buf[300];
    double *heap_buf = NULL;

    if (ring->count <= 300) {
        vals = stack_buf;
    } else {
        heap_buf = malloc((size_t)ring->count * sizeof(double));
        if (!heap_buf)
            return NAN;
        vals = heap_buf;
    }

    for (int i = 0; i < ring->count; i++)
        vals[i] = sample_field(ring, i, field_offset);

    qsort(vals, (size_t)ring->count, sizeof(double), cmp_double);

    /* Nearest-rank p95 */
    int rank = (int)ceil(0.95 * ring->count);
    if (rank < 1)          rank = 1;
    if (rank > ring->count) rank = ring->count;

    double result = vals[rank - 1];

    free(heap_buf);
    return result;
}

/* =========================================================================
 * Telemetry completeness gate
 * ========================================================================= */

/* Returns 1 if the ring buffer has sufficient, regularly-spaced samples.
 * Returns 0 if gate fails (score should return GPU_CLASS_NA). */
static int completeness_gate(const gpu_ring_t *ring, const gpu_config_t *cfg)
{
    /* Absolute minimum */
    if (ring->count < cfg->min_samples_absolute)
        return 0;

    /* Ratio minimum: need at least min_sample_ratio of the expected window */
    int expected = cfg->window_s / cfg->poll_interval_s;
    if ((double)ring->count < cfg->min_sample_ratio * (double)expected)
        return 0;

    /* Median step check: collect inter-sample deltas, find median.
     * If the median step is too large, telemetry is too sparse or irregular. */
    if (ring->count < 2)
        return 0;

    double deltas[3600];  /* max capacity from SPEC (window_s/poll_interval_s) */
    int n = ring->count - 1;

    for (int i = 0; i < n; i++) {
        int cur  = (ring->head - ring->count + i + 1 + ring->capacity) % ring->capacity;
        int prev = (ring->head - ring->count + i     + ring->capacity) % ring->capacity;
        double dt = (double)(ring->samples[cur].timestamp_ms
                             - ring->samples[prev].timestamp_ms) / 1000.0;
        deltas[i] = dt < 0 ? -dt : dt;  /* guard against clock oddities */
    }

    qsort(deltas, (size_t)n, sizeof(double), cmp_double);
    double median_step = (n % 2 == 1)
        ? deltas[n / 2]
        : (deltas[n / 2 - 1] + deltas[n / 2]) / 2.0;

    if (median_step > cfg->max_median_step_s)
        return 0;

    return 1;
}

/* =========================================================================
 * score_gpu
 * ========================================================================= */

int score_gpu(const gpu_ring_t        *ring,
              const gpu_state_t       *state,
              const gpu_baseline_t    *baseline,
              const gpu_probe_result_t *probe,
              const gpu_config_t      *cfg,
              gpu_score_result_t      *out)
{
    memset(out, 0, sizeof(*out));
    out->perf_drop_frac = NAN;

    /* Completeness gate ---------------------------------------------------- */
    out->telemetry_ok = completeness_gate(ring, cfg);
    if (!out->telemetry_ok) {
        out->score          = 0.0;
        out->classification = GPU_CLASS_NA;
        out->reason_mask    = GPU_REASON_TELEMETRY_INCOMPLETE;
        return 0;
    }

    double score    = 100.0;
    uint32_t reasons = 0;

    /* --- Ring buffer path ------------------------------------------------- */

    /* Temperature p95 */
    double temp_p95 = ring_p95(ring, offsetof(gpu_sample_t, temp_c));
    out->temp_p95_c = temp_p95;
    if (!isnan(temp_p95)) {
        if (temp_p95 > cfg->temp_p95_bad_c) {
            score   -= 25.0;
            reasons |= GPU_REASON_TEMP_BAD;
        } else if (temp_p95 > cfg->temp_p95_warn_c) {
            score   -= 10.0;
            reasons |= GPU_REASON_TEMP_WARN;
        }
    }

    /* HBM temperature p95 */
    double hbm_p95 = ring_p95(ring, offsetof(gpu_sample_t, hbm_temp_c));
    out->hbm_temp_p95_c = hbm_p95;
    if (!isnan(hbm_p95)) {
        if (hbm_p95 > cfg->hbm_temp_p95_bad_c) {
            score   -= 25.0;
            reasons |= GPU_REASON_HBM_TEMP_BAD;
        } else if (hbm_p95 > cfg->hbm_temp_p95_warn_c) {
            score   -= 10.0;
            reasons |= GPU_REASON_HBM_TEMP_WARN;
        }
    }

    /* SM clock standard deviation */
    double clk_std = ring_stddev(ring, offsetof(gpu_sample_t, sm_clock_mhz));
    out->clk_std_mhz = clk_std;
    if (!isnan(clk_std) && clk_std > cfg->clk_std_warn_mhz) {
        /* Scaled penalty: proportional to how far over threshold, capped at -15 */
        double penalty = 15.0 * (clk_std - cfg->clk_std_warn_mhz)
                         / cfg->clk_std_warn_mhz;
        if (penalty > 15.0) penalty = 15.0;
        score   -= penalty;
        reasons |= GPU_REASON_CLK_STD_HIGH;
    }

    /* Power saturation: fraction of window samples at >= power_high_ratio of limit */
    {
        int sat_count = 0;
        for (int i = 0; i < ring->count; i++) {
            double pw = sample_field(ring, i, offsetof(gpu_sample_t, power_w));
            double lim = sample_field(ring, i, offsetof(gpu_sample_t, power_limit_w));
            if (lim > 0.0 && pw >= cfg->power_high_ratio * lim)
                sat_count++;
        }
        double sat_frac = (double)sat_count / (double)ring->count;
        out->power_saturation_frac = sat_frac;

        if (sat_frac > 0.0) {
            /* Penalty scaled by fraction, capped at power_penalty_max */
            double penalty = cfg->power_penalty_max * sat_frac;
            if (penalty > cfg->power_penalty_max)
                penalty = cfg->power_penalty_max;
            score   -= penalty;
            reasons |= GPU_REASON_POWER_SATURATION;
        }
    }

    /* ECC SBE and DBE rates over window.
     * ecc_*_volatile are uint64_t — cannot use sample_field (double memcpy).
     * Read the first and last ring entries directly by index. */
    {
        int idx_first = (ring->head - ring->count + ring->capacity) % ring->capacity;
        int idx_last  = (ring->head - 1 + ring->capacity) % ring->capacity;

        const gpu_sample_t *s_first = &ring->samples[idx_first];
        const gpu_sample_t *s_last  = &ring->samples[idx_last];

        double ts_first_s = (double)s_first->timestamp_ms / 1000.0;
        double ts_last_s  = (double)s_last->timestamp_ms  / 1000.0;
        double window_hrs = (ts_last_s - ts_first_s) / 3600.0;

        /* SBE rate */
        double sbe_delta = (s_last->ecc_sbe_volatile >= s_first->ecc_sbe_volatile)
            ? (double)(s_last->ecc_sbe_volatile - s_first->ecc_sbe_volatile)
            : 0.0;  /* counter reset guard */
        double sbe_rate = (window_hrs > 0.0) ? sbe_delta / window_hrs : 0.0;
        out->ecc_sbe_rate_per_hour = sbe_rate;

        if (sbe_rate > cfg->ecc_sbe_rate_warn_per_hour) {
            score   -= cfg->ecc_sbe_penalty;
            reasons |= GPU_REASON_ECC_SBE_HIGH;
        }

        /* DBE delta */
        int dbe_active = (s_last->ecc_dbe_volatile > s_first->ecc_dbe_volatile) ? 1 : 0;
        out->ecc_dbe_in_window = dbe_active;

        if (dbe_active) {
            score   -= cfg->ecc_dbe_penalty;
            reasons |= GPU_REASON_ECC_DBE_ACTIVE;
        }
    }

    /* --- Current state path ---------------------------------------------- */

    /* Retired pages (DBE cause) */
    if (state->retired_pages_dbe >= (uint32_t)cfg->retired_pages_bad) {
        score   -= cfg->retired_pages_pen_bad;
        reasons |= GPU_REASON_RETIRED_PAGES_BAD;
    } else if (state->retired_pages_dbe >= (uint32_t)cfg->retired_pages_warn) {
        score   -= cfg->retired_pages_pen_warn;
        reasons |= GPU_REASON_RETIRED_PAGES_WARN;
    }

    /* Row remap failures */
    if (state->row_remap_failures > 0) {
        score   -= cfg->row_remap_failure_penalty;
        reasons |= GPU_REASON_ROW_REMAP;
    }

    /* PCIe link generation degraded */
    if (state->pcie_link_gen > 0 && state->pcie_link_gen_max > 0
            && state->pcie_link_gen < state->pcie_link_gen_max) {
        score   -= cfg->pcie_link_degraded_penalty;
        reasons |= GPU_REASON_PCIE_GEN;
    }

    /* PCIe link width degraded */
    if (state->pcie_link_width > 0 && state->pcie_link_width_max > 0
            && state->pcie_link_width < state->pcie_link_width_max) {
        score   -= cfg->pcie_width_degraded_penalty;
        reasons |= GPU_REASON_PCIE_WIDTH;
    }

    /* --- Perf/W path (probe + baseline) ---------------------------------- */
    if (baseline && baseline->available && baseline->valid
            && baseline->perf_w_mean > 0.0
            && probe && probe->available && !probe->stale
            && probe->perf_w_mean > 0.0) {

        double drop = (baseline->perf_w_mean - probe->perf_w_mean)
                      / baseline->perf_w_mean;
        if (drop < 0.0) drop = 0.0;  /* improvement vs baseline: no penalty */
        out->perf_drop_frac = drop;

        if (drop > cfg->perf_drop_severe) {
            score   -= cfg->perf_drop_pen_severe;
            reasons |= GPU_REASON_PERF_DROP_SEVERE;
        } else if (drop > cfg->perf_drop_bad) {
            score   -= cfg->perf_drop_pen_bad;
            reasons |= GPU_REASON_PERF_DROP_BAD;
        } else if (drop > cfg->perf_drop_warn) {
            score   -= cfg->perf_drop_pen_warn;
            reasons |= GPU_REASON_PERF_DROP_WARN;
        }
    }

    if (probe && probe->stale)
        reasons |= GPU_REASON_PROBE_STALE;

    /* --- Clamp and classify ---------------------------------------------- */
    if (score < 0.0) score = 0.0;

    out->score       = score;
    out->reason_mask = reasons;

    if (score >= 85.0)
        out->classification = GPU_CLASS_HEALTHY;
    else if (score >= 70.0)
        out->classification = GPU_CLASS_MONITOR;
    else if (score >= 50.0)
        out->classification = GPU_CLASS_DEGRADING;
    else
        out->classification = GPU_CLASS_DECOMMISSION;

    return 0;
}
