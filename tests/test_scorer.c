#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "test_harness.h"
#include "scorer.h"
#include "config.h"
#include "types.h"
#include "util.h"

/* =========================================================================
 * Ring buffer helpers
 * ========================================================================= */

/* Allocate and zero a ring buffer with the given capacity. */
static gpu_ring_t make_ring(int capacity)
{
    gpu_ring_t r;
    r.samples  = calloc((size_t)capacity, sizeof(gpu_sample_t));
    r.head     = 0;
    r.count    = 0;
    r.capacity = capacity;
    return r;
}

static void free_ring(gpu_ring_t *r)
{
    free(r->samples);
    r->samples = NULL;
}

/* Push one sample into the ring buffer. timestamp_ms advances by step_ms. */
static void ring_push(gpu_ring_t *r, const gpu_sample_t *s)
{
    r->samples[r->head] = *s;
    r->head = (r->head + 1) % r->capacity;
    if (r->count < r->capacity)
        r->count++;
}

/* Fill a ring with `n` identical samples, timestamps spaced 1000ms apart. */
static void ring_fill_uniform(gpu_ring_t *r, int n,
                               double power_w, double power_limit_w,
                               double temp_c,  double hbm_temp_c,
                               double sm_mhz,
                               uint64_t sbe_count, uint64_t dbe_count)
{
    for (int i = 0; i < n; i++) {
        gpu_sample_t s = {
            .power_w        = power_w,
            .power_limit_w  = power_limit_w,
            .temp_c         = temp_c,
            .hbm_temp_c     = hbm_temp_c,
            .sm_clock_mhz   = sm_mhz,
            .ecc_sbe_volatile = sbe_count,
            .ecc_dbe_volatile = dbe_count,
            .timestamp_ms   = (uint64_t)(i + 1) * 1000ULL,
        };
        ring_push(r, &s);
    }
}

/* Default cfg — just enough to score cleanly. */
static gpu_config_t make_cfg(void)
{
    gpu_config_t cfg;
    config_load(NULL, &cfg);
    return cfg;
}

/* Default healthy state. */
static gpu_state_t make_state(void)
{
    gpu_state_t s;
    memset(&s, 0, sizeof(s));
    s.pcie_link_gen       = 4;
    s.pcie_link_gen_max   = 4;
    s.pcie_link_width     = 16;
    s.pcie_link_width_max = 16;
    s.fan_speed_pct       = 50;
    return s;
}

/* =========================================================================
 * Ring buffer statistics unit tests
 * ========================================================================= */

static void test_ring_mean(void)
{
    gpu_ring_t r = make_ring(10);
    ring_fill_uniform(&r, 5, 300.0, 400.0, 70.0, 65.0, 1400.0, 0, 0);

    double m = ring_mean(&r, offsetof(gpu_sample_t, temp_c));
    ASSERT(!isnan(m));
    ASSERT(fabs(m - 70.0) < 1e-9);

    /* Mean of power_w */
    double mp = ring_mean(&r, offsetof(gpu_sample_t, power_w));
    ASSERT(fabs(mp - 300.0) < 1e-9);

    free_ring(&r);
}

static void test_ring_stddev_uniform(void)
{
    gpu_ring_t r = make_ring(10);
    /* All same value → std dev should be 0 */
    ring_fill_uniform(&r, 8, 300.0, 400.0, 70.0, 65.0, 1410.0, 0, 0);

    double sd = ring_stddev(&r, offsetof(gpu_sample_t, sm_clock_mhz));
    ASSERT(!isnan(sd));
    ASSERT(fabs(sd) < 1e-9);

    free_ring(&r);
}

static void test_ring_stddev_known(void)
{
    /* Values: 2, 4, 4, 4, 5, 5, 7, 9 — sample std dev = 2.0 */
    double vals[] = { 2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0 };
    int n = 8;

    gpu_ring_t r = make_ring(n);
    for (int i = 0; i < n; i++) {
        gpu_sample_t s;
        memset(&s, 0, sizeof(s));
        s.sm_clock_mhz = vals[i];
        s.timestamp_ms = (uint64_t)(i + 1) * 1000ULL;
        ring_push(&r, &s);
    }

    double sd = ring_stddev(&r, offsetof(gpu_sample_t, sm_clock_mhz));
    ASSERT(!isnan(sd));
    /* Sample std dev (n-1): sum of sq deviations = 32, n-1 = 7 → sqrt(32/7) */
    ASSERT(fabs(sd - sqrt(32.0 / 7.0)) < 1e-9);

    free_ring(&r);
}

static void test_ring_p95(void)
{
    /* 20 samples: values 1..20. p95 = ceiling(0.95 * 20) = 19th value = 19. */
    gpu_ring_t r = make_ring(20);
    for (int i = 1; i <= 20; i++) {
        gpu_sample_t s;
        memset(&s, 0, sizeof(s));
        s.temp_c       = (double)i;
        s.timestamp_ms = (uint64_t)i * 1000ULL;
        ring_push(&r, &s);
    }

    double p = ring_p95(&r, offsetof(gpu_sample_t, temp_c));
    ASSERT(!isnan(p));
    ASSERT(fabs(p - 19.0) < 1e-9);

    free_ring(&r);
}

static void test_ring_empty(void)
{
    gpu_ring_t r = make_ring(10);
    /* empty ring → NaN */
    ASSERT(isnan(ring_mean(&r, offsetof(gpu_sample_t, temp_c))));
    ASSERT(isnan(ring_stddev(&r, offsetof(gpu_sample_t, temp_c))));
    ASSERT(isnan(ring_p95(&r, offsetof(gpu_sample_t, temp_c))));
    free_ring(&r);
}

static void test_ring_wraparound(void)
{
    /* Capacity 4, push 6 samples — ring wraps. Latest 4 should be used. */
    gpu_ring_t r = make_ring(4);
    for (int i = 1; i <= 6; i++) {
        gpu_sample_t s;
        memset(&s, 0, sizeof(s));
        s.temp_c       = (double)i;
        s.timestamp_ms = (uint64_t)i * 1000ULL;
        ring_push(&r, &s);
    }
    /* ring holds samples 3,4,5,6 — mean = 4.5 */
    double m = ring_mean(&r, offsetof(gpu_sample_t, temp_c));
    ASSERT(fabs(m - 4.5) < 1e-9);

    free_ring(&r);
}

/* =========================================================================
 * Completeness gate
 * ========================================================================= */

static void test_gate_passes_with_enough_samples(void)
{
    gpu_config_t cfg = make_cfg();
    /* window_s=300, poll_interval_s=1 → expected=300; need 0.8*300=240 */
    gpu_ring_t r = make_ring(300);
    ring_fill_uniform(&r, 250, 300.0, 400.0, 70.0, 65.0, 1410.0, 0, 0);

    gpu_state_t     st  = make_state();
    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT_EQ_INT(out.telemetry_ok, 1);
    ASSERT(out.classification != GPU_CLASS_NA);

    free_ring(&r);
}

static void test_gate_fails_too_few_samples(void)
{
    gpu_config_t cfg = make_cfg();
    /* Only 5 samples — below min_samples_absolute=10 */
    gpu_ring_t r = make_ring(300);
    ring_fill_uniform(&r, 5, 300.0, 400.0, 70.0, 65.0, 1410.0, 0, 0);

    gpu_state_t     st  = make_state();
    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT_EQ_INT(out.telemetry_ok, 0);
    ASSERT_EQ_INT(out.classification, GPU_CLASS_NA);
    ASSERT(out.reason_mask & GPU_REASON_TELEMETRY_INCOMPLETE);

    free_ring(&r);
}

static void test_gate_fails_irregular_samples(void)
{
    gpu_config_t cfg = make_cfg();
    gpu_ring_t r = make_ring(300);

    /* Push 200 samples but with large gaps (10s steps, max_median_step_s=2.5) */
    for (int i = 0; i < 200; i++) {
        gpu_sample_t s;
        memset(&s, 0, sizeof(s));
        s.temp_c       = 70.0;
        s.power_w      = 300.0;
        s.power_limit_w = 400.0;
        s.sm_clock_mhz = 1410.0;
        s.timestamp_ms = (uint64_t)(i + 1) * 10000ULL;  /* 10s step */
        ring_push(&r, &s);
    }

    gpu_state_t     st  = make_state();
    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT_EQ_INT(out.telemetry_ok, 0);
    ASSERT_EQ_INT(out.classification, GPU_CLASS_NA);

    free_ring(&r);
}

/* =========================================================================
 * Scoring — healthy baseline
 * ========================================================================= */

static void test_healthy_score(void)
{
    gpu_config_t cfg = make_cfg();
    gpu_ring_t r = make_ring(300);
    /* Normal operating conditions: 300W/400W limit, 72°C, 68°C HBM, stable clocks */
    ring_fill_uniform(&r, 300, 300.0, 400.0, 72.0, 68.0, 1410.0, 0, 0);

    gpu_state_t     st  = make_state();
    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT_EQ_INT(out.telemetry_ok, 1);
    ASSERT_EQ_INT(out.classification, GPU_CLASS_HEALTHY);
    ASSERT(out.score >= 85.0);
    ASSERT_EQ_INT(out.reason_mask, 0);
    ASSERT_EQ_INT(out.ecc_dbe_in_window, 0);

    free_ring(&r);
}

/* =========================================================================
 * Scoring — thermal penalties
 * ========================================================================= */

static void test_temp_warn_penalty(void)
{
    gpu_config_t cfg = make_cfg();  /* temp_p95_warn=80, bad=90 */
    gpu_ring_t r = make_ring(300);
    ring_fill_uniform(&r, 300, 300.0, 400.0, 85.0, 68.0, 1410.0, 0, 0);

    gpu_state_t     st  = make_state();
    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT(out.reason_mask & GPU_REASON_TEMP_WARN);
    ASSERT(!(out.reason_mask & GPU_REASON_TEMP_BAD));
    ASSERT(fabs(out.score - 90.0) < 1e-6);  /* 100 - 10 */

    free_ring(&r);
}

static void test_temp_bad_penalty_replaces_warn(void)
{
    gpu_config_t cfg = make_cfg();
    gpu_ring_t r = make_ring(300);
    ring_fill_uniform(&r, 300, 300.0, 400.0, 95.0, 68.0, 1410.0, 0, 0);

    gpu_state_t     st  = make_state();
    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    /* -25 only (bad), not -10 -25 */
    ASSERT(out.reason_mask & GPU_REASON_TEMP_BAD);
    ASSERT(!(out.reason_mask & GPU_REASON_TEMP_WARN));
    ASSERT(fabs(out.score - 75.0) < 1e-6);

    free_ring(&r);
}

static void test_hbm_temp_bad_penalty(void)
{
    gpu_config_t cfg = make_cfg();  /* hbm_warn=85, hbm_bad=95 */
    gpu_ring_t r = make_ring(300);
    ring_fill_uniform(&r, 300, 300.0, 400.0, 70.0, 100.0, 1410.0, 0, 0);

    gpu_state_t     st  = make_state();
    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT(out.reason_mask & GPU_REASON_HBM_TEMP_BAD);
    ASSERT(!(out.reason_mask & GPU_REASON_HBM_TEMP_WARN));
    ASSERT(fabs(out.score - 75.0) < 1e-6);

    free_ring(&r);
}

/* =========================================================================
 * Scoring — clock instability
 * ========================================================================= */

static void test_clk_std_penalty_scaled(void)
{
    /* clk_std_warn_mhz=120; feed alternating 1410/1650 → std ~120 → small penalty */
    gpu_config_t cfg = make_cfg();
    gpu_ring_t r = make_ring(300);

    for (int i = 0; i < 300; i++) {
        gpu_sample_t s;
        memset(&s, 0, sizeof(s));
        s.power_w       = 300.0;
        s.power_limit_w = 400.0;
        s.temp_c        = 70.0;
        s.hbm_temp_c    = 65.0;
        s.sm_clock_mhz  = (i % 2 == 0) ? 1170.0 : 1650.0;  /* std ~240 */
        s.timestamp_ms  = (uint64_t)(i + 1) * 1000ULL;
        ring_push(&r, &s);
    }

    gpu_state_t     st  = make_state();
    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT(out.reason_mask & GPU_REASON_CLK_STD_HIGH);
    ASSERT(out.clk_std_mhz > cfg.clk_std_warn_mhz);
    /* Penalty is > 0 and <= 15 */
    double penalty = 100.0 - out.score;
    ASSERT(penalty > 0.0 && penalty <= 15.0);

    free_ring(&r);
}

/* =========================================================================
 * Scoring — ECC
 * ========================================================================= */

static void test_ecc_sbe_rate_penalty(void)
{
    gpu_config_t cfg = make_cfg();  /* ecc_sbe_rate_warn=100/hr */
    gpu_ring_t r = make_ring(300);

    /* 300s window, 200 SBE events → rate = 200 / (300/3600) = 2400/hr >> 100 */
    for (int i = 0; i < 300; i++) {
        gpu_sample_t s;
        memset(&s, 0, sizeof(s));
        s.power_w          = 300.0;
        s.power_limit_w    = 400.0;
        s.temp_c           = 70.0;
        s.hbm_temp_c       = 65.0;
        s.sm_clock_mhz     = 1410.0;
        s.ecc_sbe_volatile = (uint64_t)i;  /* incrementing counter */
        s.ecc_dbe_volatile = 0;
        s.timestamp_ms     = (uint64_t)(i + 1) * 1000ULL;
        ring_push(&r, &s);
    }

    gpu_state_t     st  = make_state();
    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT(out.reason_mask & GPU_REASON_ECC_SBE_HIGH);
    ASSERT(out.ecc_sbe_rate_per_hour > cfg.ecc_sbe_rate_warn_per_hour);

    free_ring(&r);
}

static void test_ecc_dbe_active_penalty(void)
{
    gpu_config_t cfg = make_cfg();
    gpu_ring_t r = make_ring(300);

    /* DBE counter increments by 1 at midpoint */
    for (int i = 0; i < 300; i++) {
        gpu_sample_t s;
        memset(&s, 0, sizeof(s));
        s.power_w          = 300.0;
        s.power_limit_w    = 400.0;
        s.temp_c           = 70.0;
        s.hbm_temp_c       = 65.0;
        s.sm_clock_mhz     = 1410.0;
        s.ecc_sbe_volatile = 0;
        s.ecc_dbe_volatile = (i >= 150) ? 1ULL : 0ULL;
        s.timestamp_ms     = (uint64_t)(i + 1) * 1000ULL;
        ring_push(&r, &s);
    }

    gpu_state_t     st  = make_state();
    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT(out.reason_mask & GPU_REASON_ECC_DBE_ACTIVE);
    ASSERT_EQ_INT(out.ecc_dbe_in_window, 1);
    /* -25 penalty */
    ASSERT(fabs(out.score - 75.0) < 1e-6);

    free_ring(&r);
}

/* =========================================================================
 * Scoring — current state path
 * ========================================================================= */

static void test_retired_pages_warn(void)
{
    gpu_config_t cfg = make_cfg();  /* retired_pages_warn=1, bad=10 */
    gpu_ring_t r = make_ring(300);
    ring_fill_uniform(&r, 300, 300.0, 400.0, 70.0, 65.0, 1410.0, 0, 0);

    gpu_state_t st = make_state();
    st.retired_pages_dbe = 5;  /* >= warn(1), < bad(10) */

    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT(out.reason_mask & GPU_REASON_RETIRED_PAGES_WARN);
    ASSERT(!(out.reason_mask & GPU_REASON_RETIRED_PAGES_BAD));
    ASSERT(fabs(out.score - 95.0) < 1e-6);  /* 100 - 5 */

    free_ring(&r);
}

static void test_retired_pages_bad(void)
{
    gpu_config_t cfg = make_cfg();
    gpu_ring_t r = make_ring(300);
    ring_fill_uniform(&r, 300, 300.0, 400.0, 70.0, 65.0, 1410.0, 0, 0);

    gpu_state_t st = make_state();
    st.retired_pages_dbe = 15;  /* >= bad(10) */

    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT(out.reason_mask & GPU_REASON_RETIRED_PAGES_BAD);
    ASSERT(!(out.reason_mask & GPU_REASON_RETIRED_PAGES_WARN));
    ASSERT(fabs(out.score - 85.0) < 1e-6);  /* 100 - 15 */

    free_ring(&r);
}

static void test_row_remap_failure(void)
{
    gpu_config_t cfg = make_cfg();
    gpu_ring_t r = make_ring(300);
    ring_fill_uniform(&r, 300, 300.0, 400.0, 70.0, 65.0, 1410.0, 0, 0);

    gpu_state_t st = make_state();
    st.row_remap_failures = 1;

    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT(out.reason_mask & GPU_REASON_ROW_REMAP);
    ASSERT(fabs(out.score - 75.0) < 1e-6);  /* 100 - 25 */

    free_ring(&r);
}

static void test_pcie_gen_degraded(void)
{
    gpu_config_t cfg = make_cfg();
    gpu_ring_t r = make_ring(300);
    ring_fill_uniform(&r, 300, 300.0, 400.0, 70.0, 65.0, 1410.0, 0, 0);

    gpu_state_t st = make_state();
    st.pcie_link_gen     = 3;
    st.pcie_link_gen_max = 4;

    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT(out.reason_mask & GPU_REASON_PCIE_GEN);
    ASSERT(fabs(out.score - 90.0) < 1e-6);  /* 100 - 10 */

    free_ring(&r);
}

static void test_pcie_width_degraded(void)
{
    gpu_config_t cfg = make_cfg();
    gpu_ring_t r = make_ring(300);
    ring_fill_uniform(&r, 300, 300.0, 400.0, 70.0, 65.0, 1410.0, 0, 0);

    gpu_state_t st = make_state();
    st.pcie_link_width     = 8;
    st.pcie_link_width_max = 16;

    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT(out.reason_mask & GPU_REASON_PCIE_WIDTH);
    ASSERT(fabs(out.score - 85.0) < 1e-6);  /* 100 - 15 */

    free_ring(&r);
}

/* =========================================================================
 * Scoring — perf/W path
 * ========================================================================= */

static void test_perf_drop_warn(void)
{
    gpu_config_t cfg = make_cfg();  /* perf_drop_warn=0.03, pen=5 */
    gpu_ring_t r = make_ring(300);
    ring_fill_uniform(&r, 300, 300.0, 400.0, 70.0, 65.0, 1410.0, 0, 0);

    gpu_baseline_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.available   = 1;
    baseline.valid       = 1;
    baseline.perf_w_mean = 1.0;

    gpu_probe_result_t probe;
    memset(&probe, 0, sizeof(probe));
    probe.available    = 1;
    probe.stale        = 0;
    probe.perf_w_mean  = 0.96;  /* 4% drop — between warn(3%) and bad(7%) */

    gpu_state_t     st  = make_state();
    gpu_score_result_t out;
    score_gpu(&r, &st, &baseline, &probe, &cfg, &out);

    ASSERT(out.reason_mask & GPU_REASON_PERF_DROP_WARN);
    ASSERT(!(out.reason_mask & GPU_REASON_PERF_DROP_BAD));
    ASSERT(fabs(out.score - 95.0) < 1e-6);  /* 100 - 5 */
    ASSERT(fabs(out.perf_drop_frac - 0.04) < 1e-9);

    free_ring(&r);
}

static void test_perf_drop_severe(void)
{
    gpu_config_t cfg = make_cfg();  /* perf_drop_severe=0.12, pen=30 */
    gpu_ring_t r = make_ring(300);
    ring_fill_uniform(&r, 300, 300.0, 400.0, 70.0, 65.0, 1410.0, 0, 0);

    gpu_baseline_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.available   = 1;
    baseline.valid       = 1;
    baseline.perf_w_mean = 1.0;

    gpu_probe_result_t probe;
    memset(&probe, 0, sizeof(probe));
    probe.available   = 1;
    probe.stale       = 0;
    probe.perf_w_mean = 0.80;  /* 20% drop — > severe(12%) */

    gpu_state_t     st  = make_state();
    gpu_score_result_t out;
    score_gpu(&r, &st, &baseline, &probe, &cfg, &out);

    ASSERT(out.reason_mask & GPU_REASON_PERF_DROP_SEVERE);
    ASSERT(!(out.reason_mask & GPU_REASON_PERF_DROP_BAD));
    ASSERT(fabs(out.score - 70.0) < 1e-6);  /* 100 - 30 */

    free_ring(&r);
}

static void test_perf_improvement_no_penalty(void)
{
    gpu_config_t cfg = make_cfg();
    gpu_ring_t r = make_ring(300);
    ring_fill_uniform(&r, 300, 300.0, 400.0, 70.0, 65.0, 1410.0, 0, 0);

    gpu_baseline_t baseline;
    memset(&baseline, 0, sizeof(baseline));
    baseline.available   = 1;
    baseline.valid       = 1;
    baseline.perf_w_mean = 1.0;

    gpu_probe_result_t probe;
    memset(&probe, 0, sizeof(probe));
    probe.available   = 1;
    probe.stale       = 0;
    probe.perf_w_mean = 1.10;  /* 10% improvement vs baseline */

    gpu_state_t     st  = make_state();
    gpu_score_result_t out;
    score_gpu(&r, &st, &baseline, &probe, &cfg, &out);

    ASSERT(!(out.reason_mask & GPU_REASON_PERF_DROP_WARN));
    ASSERT(fabs(out.perf_drop_frac - 0.0) < 1e-9);
    ASSERT(fabs(out.score - 100.0) < 1e-6);

    free_ring(&r);
}

static void test_stale_probe_sets_reason(void)
{
    gpu_config_t cfg = make_cfg();
    gpu_ring_t r = make_ring(300);
    ring_fill_uniform(&r, 300, 300.0, 400.0, 70.0, 65.0, 1410.0, 0, 0);

    gpu_probe_result_t probe;
    memset(&probe, 0, sizeof(probe));
    probe.available = 1;
    probe.stale     = 1;  /* stale — no perf/W component, but reason bit set */

    gpu_state_t     st  = make_state();
    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, &probe, &cfg, &out);

    ASSERT(out.reason_mask & GPU_REASON_PROBE_STALE);
    ASSERT(isnan(out.perf_drop_frac));  /* no perf/W computed */

    free_ring(&r);
}

/* =========================================================================
 * Scoring — cumulative and edge cases
 * ========================================================================= */

static void test_score_clamps_at_zero(void)
{
    gpu_config_t cfg = make_cfg();
    gpu_ring_t r = make_ring(300);
    /* Terrible GPU: hot, DBE active, row remap, PCIe degraded */
    ring_fill_uniform(&r, 300, 300.0, 400.0, 95.0, 100.0, 1410.0, 0, 1);

    gpu_state_t st = make_state();
    st.row_remap_failures  = 2;
    st.pcie_link_gen       = 3;
    st.pcie_link_gen_max   = 4;
    st.pcie_link_width     = 8;
    st.pcie_link_width_max = 16;

    gpu_score_result_t out;
    score_gpu(&r, &st, NULL, NULL, &cfg, &out);

    ASSERT(out.score >= 0.0);
    ASSERT_EQ_INT(out.classification, GPU_CLASS_DECOMMISSION);

    free_ring(&r);
}

static void test_classification_boundaries(void)
{
    /* Test each classification boundary directly by manipulating the score.
     * We use a clean ring + state and rely on penalty accumulation. */
    gpu_config_t cfg = make_cfg();

    /* Healthy: no penalties, score = 100 */
    {
        gpu_ring_t r = make_ring(300);
        ring_fill_uniform(&r, 300, 300.0, 400.0, 70.0, 65.0, 1410.0, 0, 0);
        gpu_state_t st = make_state();
        gpu_score_result_t out;
        score_gpu(&r, &st, NULL, NULL, &cfg, &out);
        ASSERT_EQ_INT(out.classification, GPU_CLASS_HEALTHY);
        free_ring(&r);
    }

    /* Monitor: temp_bad (-25) + row_remap (-25) = 50 → Degrading actually.
     * Use just temp_warn (-10) → 90 = Healthy.
     * Use retired_pages_bad (-15) + pcie_width (-15) = 70 → Monitor. */
    {
        gpu_ring_t r = make_ring(300);
        ring_fill_uniform(&r, 300, 300.0, 400.0, 85.0, 65.0, 1410.0, 0, 0);
        gpu_state_t st = make_state();
        st.retired_pages_dbe   = 15;   /* -15 */
        st.pcie_link_width     = 8;
        st.pcie_link_width_max = 16;   /* -15 */
        /* total penalties: -10 (temp_warn) -15 (retired_bad) -15 (pcie_width) = -40 → 60 */
        gpu_score_result_t out;
        score_gpu(&r, &st, NULL, NULL, &cfg, &out);
        ASSERT(out.score >= 50.0 && out.score < 70.0);
        ASSERT_EQ_INT(out.classification, GPU_CLASS_DEGRADING);
        free_ring(&r);
    }
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

int main(void)
{
    fprintf(stderr, "test_scorer\n");

    RUN_TEST(test_ring_mean);
    RUN_TEST(test_ring_stddev_uniform);
    RUN_TEST(test_ring_stddev_known);
    RUN_TEST(test_ring_p95);
    RUN_TEST(test_ring_empty);
    RUN_TEST(test_ring_wraparound);

    RUN_TEST(test_gate_passes_with_enough_samples);
    RUN_TEST(test_gate_fails_too_few_samples);
    RUN_TEST(test_gate_fails_irregular_samples);

    RUN_TEST(test_healthy_score);
    RUN_TEST(test_temp_warn_penalty);
    RUN_TEST(test_temp_bad_penalty_replaces_warn);
    RUN_TEST(test_hbm_temp_bad_penalty);
    RUN_TEST(test_clk_std_penalty_scaled);
    RUN_TEST(test_ecc_sbe_rate_penalty);
    RUN_TEST(test_ecc_dbe_active_penalty);

    RUN_TEST(test_retired_pages_warn);
    RUN_TEST(test_retired_pages_bad);
    RUN_TEST(test_row_remap_failure);
    RUN_TEST(test_pcie_gen_degraded);
    RUN_TEST(test_pcie_width_degraded);

    RUN_TEST(test_perf_drop_warn);
    RUN_TEST(test_perf_drop_severe);
    RUN_TEST(test_perf_improvement_no_penalty);
    RUN_TEST(test_stale_probe_sets_reason);

    RUN_TEST(test_score_clamps_at_zero);
    RUN_TEST(test_classification_boundaries);

    return TEST_RESULT();
}
