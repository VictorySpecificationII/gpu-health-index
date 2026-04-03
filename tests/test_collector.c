/*
 * test_collector.c — unit tests for collector.c and snapshot.c
 *
 * Covers:
 *   - ring_push: wraparound, head advancement, count saturation
 *   - snapshot_update: all fields populated from score + state + ctx
 *   - snapshot_send / snapshot_recv: round-trip over a real socketpair
 *   - collector_start / collector_stop: thread lifecycle with fake vtable
 *   - Error tracking: fake vtable errors trigger consecutive_errors/gpu_available
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include "collector.h"
#include "snapshot.h"
#include "config.h"
#include "test_harness.h"

/* =========================================================================
 * Fake NVML vtable
 * ========================================================================= */

static int fake_nvml_ret = 0;   /* 0 = NVML_SUCCESS */
static int fake_nvml_calls = 0;

static int fake_Init(void)    { return 0; }
static int fake_Shutdown(void){ return 0; }
static const char *fake_ErrorString(int r) { (void)r; return "fake"; }
static int fake_DeviceGetCount(unsigned int *c) { *c = 1; return 0; }
static int fake_DeviceGetHandleByIndex(unsigned int i, void **d)
    { (void)i; *d = (void *)0xDEADBEEF; return 0; }
static int fake_DeviceGetSerial(void *d, char *s, unsigned int l)
    { (void)d; (void)l; strncpy(s, "SN0001", l > 0 ? l-1 : 0); return 0; }
static int fake_DeviceGetUUID(void *d, char *s, unsigned int l)
    { (void)d; (void)l; strncpy(s, "GPU-FAKE-UUID", l > 0 ? l-1 : 0); return 0; }
static int fake_DeviceGetName(void *d, char *s, unsigned int l)
    { (void)d; (void)l; strncpy(s, "FakeGPU A100", l > 0 ? l-1 : 0); return 0; }
static int fake_SystemGetDriverVersion(char *s, unsigned int l)
    { strncpy(s, "525.85.12", l > 0 ? l-1 : 0); return 0; }
static int fake_DeviceGetMaxPcieLinkGeneration(void *d, unsigned int *v)
    { (void)d; *v = 4; return 0; }
static int fake_DeviceGetMaxPcieLinkWidth(void *d, unsigned int *v)
    { (void)d; *v = 16; return 0; }

static int fake_DeviceGetTemperature(void *d, int sensor, unsigned int *v)
{
    (void)d;
    fake_nvml_calls++;
    if (fake_nvml_ret != 0)
        return fake_nvml_ret;
    *v = (sensor == NVML_TEMPERATURE_GPU) ? 72 : 65;
    return 0;
}
static int fake_DeviceGetPowerUsage(void *d, unsigned int *v)
    { (void)d; if (fake_nvml_ret) return fake_nvml_ret; *v = 300000; return 0; }
static int fake_DeviceGetEnforcedPowerLimit(void *d, unsigned int *v)
    { (void)d; if (fake_nvml_ret) return fake_nvml_ret; *v = 400000; return 0; }
static int fake_DeviceGetTotalEccErrors(void *d, int t, int c,
                                         unsigned long long *v)
    { (void)d;(void)t;(void)c; if (fake_nvml_ret) return fake_nvml_ret;
      *v = 0; return 0; }
static int fake_DeviceGetMemoryInfo(void *d, nvml_memory_t *m)
    { (void)d; if (fake_nvml_ret) return fake_nvml_ret;
      m->total = 80ULL<<30; m->used = 10ULL<<30; m->free = 70ULL<<30; return 0; }
static int fake_DeviceGetClockInfo(void *d, int t, unsigned int *v)
{
    (void)d;
    if (fake_nvml_ret) return fake_nvml_ret;
    *v = (t == NVML_CLOCK_SM) ? 1410 : 1215;
    return 0;
}
static int fake_DeviceGetUtilizationRates(void *d, nvml_utilization_t *u)
    { (void)d; if (fake_nvml_ret) return fake_nvml_ret;
      u->gpu = 80; u->memory = 50; return 0; }
static int fake_DeviceGetCurrentClocksThrottleReasons(void *d,
                                                       unsigned long long *r)
    { (void)d; if (fake_nvml_ret) return fake_nvml_ret; *r = 0; return 0; }
static int fake_DeviceGetPerformanceState(void *d, int *p)
    { (void)d; if (fake_nvml_ret) return fake_nvml_ret; *p = 0; return 0; }
static int fake_DeviceGetCurrPcieLinkGeneration(void *d, unsigned int *v)
    { (void)d; if (fake_nvml_ret) return fake_nvml_ret; *v = 4; return 0; }
static int fake_DeviceGetCurrPcieLinkWidth(void *d, unsigned int *v)
    { (void)d; if (fake_nvml_ret) return fake_nvml_ret; *v = 16; return 0; }

static nvml_vtable_t make_nvml_vt(void)
{
    nvml_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.Init                               = fake_Init;
    vt.Shutdown                           = fake_Shutdown;
    vt.ErrorString                        = fake_ErrorString;
    vt.DeviceGetCount                     = fake_DeviceGetCount;
    vt.DeviceGetHandleByIndex             = fake_DeviceGetHandleByIndex;
    vt.DeviceGetSerial                    = fake_DeviceGetSerial;
    vt.DeviceGetUUID                      = fake_DeviceGetUUID;
    vt.DeviceGetName                      = fake_DeviceGetName;
    vt.SystemGetDriverVersion             = fake_SystemGetDriverVersion;
    vt.DeviceGetMaxPcieLinkGeneration     = fake_DeviceGetMaxPcieLinkGeneration;
    vt.DeviceGetMaxPcieLinkWidth          = fake_DeviceGetMaxPcieLinkWidth;
    vt.DeviceGetTemperature               = fake_DeviceGetTemperature;
    vt.DeviceGetPowerUsage                = fake_DeviceGetPowerUsage;
    vt.DeviceGetEnforcedPowerLimit        = fake_DeviceGetEnforcedPowerLimit;
    vt.DeviceGetTotalEccErrors            = fake_DeviceGetTotalEccErrors;
    vt.DeviceGetMemoryInfo                = fake_DeviceGetMemoryInfo;
    vt.DeviceGetClockInfo                 = fake_DeviceGetClockInfo;
    vt.DeviceGetUtilizationRates          = fake_DeviceGetUtilizationRates;
    vt.DeviceGetCurrentClocksThrottleReasons =
        fake_DeviceGetCurrentClocksThrottleReasons;
    vt.DeviceGetPerformanceState          = fake_DeviceGetPerformanceState;
    vt.DeviceGetCurrPcieLinkGeneration    = fake_DeviceGetCurrPcieLinkGeneration;
    vt.DeviceGetCurrPcieLinkWidth         = fake_DeviceGetCurrPcieLinkWidth;
    /* Optional functions left NULL */
    return vt;
}

/* =========================================================================
 * Test helpers
 * ========================================================================= */

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

/* Build a minimal gpu_ctx_t for snapshot tests (no threads) */
static gpu_ctx_t *make_ctx(int capacity)
{
    gpu_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->ring = make_ring(capacity);
    pthread_mutex_init(&ctx->ring_mutex,     NULL);
    pthread_mutex_init(&ctx->state_mutex,    NULL);
    pthread_mutex_init(&ctx->snapshot_mutex, NULL);
    pthread_mutex_init(&ctx->files_mutex,    NULL);
    ctx->gpu_index        = 0;
    ctx->gpu_present      = 1;
    ctx->gpu_available    = 1;
    ctx->pcie_link_gen_max   = 4;
    ctx->pcie_link_width_max = 16;
    strncpy(ctx->serial,  "SN0001",     sizeof(ctx->serial)  - 1);
    strncpy(ctx->uuid,    "GPU-UUID-1", sizeof(ctx->uuid)    - 1);
    strncpy(ctx->gpu_model, "A100",     sizeof(ctx->gpu_model) - 1);
    strncpy(ctx->driver_version, "525.85.12", sizeof(ctx->driver_version) - 1);
    return ctx;
}

static void free_ctx(gpu_ctx_t *ctx)
{
    free_ring(&ctx->ring);
    pthread_mutex_destroy(&ctx->ring_mutex);
    pthread_mutex_destroy(&ctx->state_mutex);
    pthread_mutex_destroy(&ctx->snapshot_mutex);
    pthread_mutex_destroy(&ctx->files_mutex);
    free(ctx);
}

/* Build a minimal exporter_t for thread tests */
static exporter_t *make_exp(void)
{
    exporter_t *exp = calloc(1, sizeof(*exp));
    config_load(NULL, &exp->cfg);
    exp->nvml              = make_nvml_vt();
    exp->dcgm_available    = 0;
    exp->baseline_inotify_fd = -1;
    exp->parent_fd         = -1;   /* overridden per-test if needed */
    exp->child_fd          = -1;
    return exp;
}

/* =========================================================================
 * Tests: ring_push
 * ========================================================================= */

static void test_ring_push_first_entry(void)
{
    gpu_ring_t r = make_ring(4);
    gpu_sample_t s = {.temp_c = 70.0, .timestamp_ms = 1000};

    ring_push(&r, &s);

    ASSERT_EQ_INT(1, r.count);
    ASSERT_EQ_INT(1, r.head);
    ASSERT(fabs(r.samples[0].temp_c - 70.0) < 1e-9);
    free_ring(&r);
}

static void test_ring_push_advances_head(void)
{
    gpu_ring_t r = make_ring(4);
    gpu_sample_t s = {0};

    for (int i = 0; i < 3; i++) {
        s.sm_clock_mhz = (double)(i + 1);
        ring_push(&r, &s);
    }

    ASSERT_EQ_INT(3, r.count);
    ASSERT_EQ_INT(3, r.head);
    free_ring(&r);
}

static void test_ring_push_wraps_at_capacity(void)
{
    gpu_ring_t r = make_ring(4);
    gpu_sample_t s = {0};

    /* Push 5 samples into a capacity-4 ring */
    for (int i = 0; i < 5; i++) {
        s.temp_c = (double)i;
        ring_push(&r, &s);
    }

    /* Count saturates at capacity */
    ASSERT_EQ_INT(4, r.count);
    /* head wraps to index 1 (5 % 4 == 1) */
    ASSERT_EQ_INT(1, r.head);
    /* Slot 0 holds sample 4 (the last write overwrote the first) */
    ASSERT(fabs(r.samples[0].temp_c - 4.0) < 1e-9);
    free_ring(&r);
}

static void test_ring_push_count_never_exceeds_capacity(void)
{
    gpu_ring_t r = make_ring(3);
    gpu_sample_t s = {0};

    for (int i = 0; i < 100; i++) {
        s.power_w = (double)i;
        ring_push(&r, &s);
    }

    ASSERT_EQ_INT(3, r.count);
    free_ring(&r);
}

static void test_ring_push_single_capacity(void)
{
    gpu_ring_t r = make_ring(1);
    gpu_sample_t s = {.power_w = 200.0};

    ring_push(&r, &s);
    ASSERT_EQ_INT(1, r.count);
    ASSERT_EQ_INT(0, r.head);   /* wraps back to 0 */

    s.power_w = 250.0;
    ring_push(&r, &s);
    ASSERT_EQ_INT(1, r.count);
    ASSERT_EQ_INT(0, r.head);
    ASSERT(fabs(r.samples[0].power_w - 250.0) < 1e-9);
    free_ring(&r);
}

/* =========================================================================
 * Tests: snapshot_update
 * ========================================================================= */

static void test_snapshot_update_identity(void)
{
    gpu_ctx_t *ctx = make_ctx(10);

    gpu_score_result_t score;
    memset(&score, 0, sizeof(score));
    score.score          = 95.0;
    score.classification = GPU_CLASS_HEALTHY;

    snapshot_update(ctx, &score, 0);

    ASSERT_STR("SN0001",  ctx->snapshot.serial);
    ASSERT_STR("A100",    ctx->snapshot.gpu_model);
    ASSERT_EQ_INT(0, ctx->snapshot.gpu_index);
    ASSERT_EQ_INT(4,  ctx->snapshot.pcie_link_gen_max);
    ASSERT_EQ_INT(16, ctx->snapshot.pcie_link_width_max);

    free_ctx(ctx);
}

static void test_snapshot_update_score_fields(void)
{
    gpu_ctx_t *ctx = make_ctx(10);

    gpu_score_result_t score;
    memset(&score, 0, sizeof(score));
    score.score                 = 78.5;
    score.classification        = GPU_CLASS_MONITOR;
    score.reason_mask           = GPU_REASON_TEMP_WARN;
    score.telemetry_ok          = 1;
    score.temp_p95_c            = 82.0;
    score.hbm_temp_p95_c        = 78.0;
    score.clk_std_mhz           = 15.0;
    score.power_saturation_frac = 0.2;
    score.ecc_sbe_rate_per_hour = 3.0;
    score.ecc_dbe_in_window     = 0;
    score.perf_drop_frac        = 0.02;

    snapshot_update(ctx, &score, 0);

    ASSERT(fabs(ctx->snapshot.score - 78.5) < 1e-9);
    ASSERT_EQ_INT(GPU_CLASS_MONITOR, (int)ctx->snapshot.classification);
    ASSERT_EQ_INT((int)GPU_REASON_TEMP_WARN, (int)ctx->snapshot.reason_mask);
    ASSERT_EQ_INT(1, ctx->snapshot.telemetry_ok);
    ASSERT(fabs(ctx->snapshot.temp_p95_c - 82.0) < 1e-9);
    ASSERT(fabs(ctx->snapshot.perf_drop_frac - 0.02) < 1e-9);
    ASSERT_EQ_INT(0, ctx->snapshot.ecc_dbe_in_window);

    free_ctx(ctx);
}

static void test_snapshot_update_state_fields(void)
{
    gpu_ctx_t *ctx = make_ctx(10);

    /* Set some state fields */
    ctx->state.ecc_sbe_aggregate  = 42;
    ctx->state.mem_total_bytes    = 80ULL << 30;
    ctx->state.util_gpu_pct       = 75;
    ctx->state.pcie_link_gen      = 4;
    ctx->state.pcie_link_width    = 16;
    ctx->state.fan_speed_pct      = 60;

    gpu_score_result_t score;
    memset(&score, 0, sizeof(score));
    snapshot_update(ctx, &score, 0);

    ASSERT(ctx->snapshot.ecc_sbe_aggregate == 42ULL);
    ASSERT(ctx->snapshot.mem_total_bytes   == (80ULL << 30));
    ASSERT_EQ_INT(75, ctx->snapshot.util_gpu_pct);
    ASSERT_EQ_INT(4,  ctx->snapshot.pcie_link_gen);
    ASSERT_EQ_INT(16, ctx->snapshot.pcie_link_width);
    ASSERT_EQ_INT(60, ctx->snapshot.fan_speed_pct);

    free_ctx(ctx);
}

static void test_snapshot_update_exporter_state(void)
{
    gpu_ctx_t *ctx = make_ctx(10);
    ctx->gpu_present   = 1;
    ctx->gpu_available = 0;

    gpu_score_result_t score;
    memset(&score, 0, sizeof(score));
    snapshot_update(ctx, &score, 0);

    ASSERT_EQ_INT(1, ctx->snapshot.gpu_present);
    ASSERT_EQ_INT(0, ctx->snapshot.gpu_available);
    ASSERT(ctx->snapshot.last_poll_ms > 0);

    free_ctx(ctx);
}

static void test_snapshot_update_ring_values_reflected(void)
{
    gpu_ctx_t *ctx = make_ctx(10);

    /* Push a sample into the ring so snapshot can read latest values */
    gpu_sample_t s = {
        .power_w        = 320.0,
        .power_limit_w  = 400.0,
        .temp_c         = 74.0,
        .hbm_temp_c     = 69.0,
        .sm_clock_mhz   = 1410.0,
        .timestamp_ms   = 1000,
    };
    ring_push(&ctx->ring, &s);

    gpu_score_result_t score;
    memset(&score, 0, sizeof(score));
    snapshot_update(ctx, &score, 0);

    ASSERT(fabs(ctx->snapshot.power_w       - 320.0) < 1e-9);
    ASSERT(fabs(ctx->snapshot.power_limit_w - 400.0) < 1e-9);
    ASSERT(fabs(ctx->snapshot.temp_c        - 74.0)  < 1e-9);
    ASSERT(fabs(ctx->snapshot.hbm_temp_c    - 69.0)  < 1e-9);
    ASSERT(fabs(ctx->snapshot.sm_clock_mhz  - 1410.0) < 1e-9);

    free_ctx(ctx);
}

static void test_snapshot_update_baseline_age(void)
{
    gpu_ctx_t *ctx = make_ctx(4);

    /* Baseline established 3600 s ago */
    ctx->baseline.available      = 1;
    ctx->baseline.valid          = 1;
    ctx->baseline.established_at_s =
        (uint64_t)time(NULL) - 3600;

    gpu_score_result_t score;
    memset(&score, 0, sizeof(score));
    snapshot_update(ctx, &score, 0);

    ASSERT_EQ_INT(1, ctx->snapshot.baseline_available);
    ASSERT_EQ_INT(1, ctx->snapshot.baseline_valid);
    /* Age should be approximately 3600 s, within a few seconds of scheduling jitter */
    ASSERT(ctx->snapshot.baseline_age_s >= 3598 &&
           ctx->snapshot.baseline_age_s <= 3605);

    free_ctx(ctx);
}

/* =========================================================================
 * Tests: snapshot_send / snapshot_recv
 * ========================================================================= */

static void test_snapshot_send_recv_roundtrip(void)
{
    int sv[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    gpu_ctx_t *ctx = make_ctx(4);
    ctx->gpu_index = 3;

    /* Populate snapshot with known data */
    ctx->snapshot.gpu_index  = 3;
    ctx->snapshot.score      = 88.5;
    ctx->snapshot.util_gpu_pct = 70;
    strncpy(ctx->snapshot.serial, "SN0001", sizeof(ctx->snapshot.serial) - 1);

    int ret = snapshot_send(sv[0], ctx);
    ASSERT_EQ_INT(0, ret);

    gpu_ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    ret = snapshot_recv(sv[1], &msg);
    ASSERT_EQ_INT(0, ret);

    ASSERT_EQ_INT(3, msg.gpu_index);
    ASSERT(fabs(msg.snapshot.score - 88.5) < 1e-9);
    ASSERT_EQ_INT(70, msg.snapshot.util_gpu_pct);
    ASSERT_STR("SN0001", msg.snapshot.serial);

    close(sv[0]);
    close(sv[1]);
    free_ctx(ctx);
}

static void test_snapshot_recv_eof_returns_minus1(void)
{
    int sv[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    /* Close write end immediately — recv should see EOF */
    close(sv[0]);

    gpu_ipc_msg_t msg;
    int ret = snapshot_recv(sv[1], &msg);
    ASSERT_EQ_INT(-1, ret);

    close(sv[1]);
}

static void test_snapshot_send_multiple_gpus(void)
{
    int sv[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    /* Send two messages */
    for (int i = 0; i < 2; i++) {
        gpu_ctx_t *ctx = make_ctx(4);
        ctx->gpu_index = i;
        ctx->snapshot.gpu_index = i;
        ctx->snapshot.score     = (double)(i * 10 + 80);
        snapshot_send(sv[0], ctx);
        free_ctx(ctx);
    }

    /* Receive both */
    for (int i = 0; i < 2; i++) {
        gpu_ipc_msg_t msg;
        ASSERT(snapshot_recv(sv[1], &msg) == 0);
        ASSERT_EQ_INT(i, msg.gpu_index);
        ASSERT(fabs(msg.snapshot.score - (double)(i * 10 + 80)) < 1e-9);
    }

    close(sv[0]);
    close(sv[1]);
}

/* =========================================================================
 * Tests: collector_start / collector_stop lifecycle
 * ========================================================================= */

static void test_collector_start_stop_immediate(void)
{
    /*
     * Allocate a socketpair so snapshot_send has a valid fd.
     * Set stop=1 immediately so the poll thread exits before sleeping.
     */
    int sv[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    exporter_t *exp = make_exp();
    exp->parent_fd = sv[0];

    gpu_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->gpu_index        = 0;
    ctx->gpu_present      = 1;
    ctx->gpu_available    = 1;
    ctx->nvml_handle      = (void *)0xDEADBEEF;
    ctx->pcie_link_gen_max   = 4;
    ctx->pcie_link_width_max = 16;

    ASSERT(collector_start(ctx, exp) == 0);

    /* Signal stop right away */
    collector_stop(ctx);

    /* Thread joined cleanly: gpu_present still 1 (no hard errors) */
    ASSERT_EQ_INT(1, ctx->gpu_present);

    free(ctx->ring.samples);
    pthread_mutex_destroy(&ctx->ring_mutex);
    pthread_mutex_destroy(&ctx->state_mutex);
    pthread_mutex_destroy(&ctx->snapshot_mutex);
    pthread_mutex_destroy(&ctx->files_mutex);
    free(ctx);
    free(exp);
    close(sv[0]);
    close(sv[1]);
}

static void test_collector_completes_one_poll_cycle(void)
{
    /*
     * Let the thread run until ctx->ready is set (one full poll cycle),
     * then stop it.  Verifies ring has an entry and state was populated.
     */
    int sv[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    exporter_t *exp = make_exp();
    exp->parent_fd = sv[0];

    gpu_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->gpu_index        = 0;
    ctx->gpu_present      = 1;
    ctx->gpu_available    = 1;
    ctx->nvml_handle      = (void *)0xDEADBEEF;
    ctx->pcie_link_gen_max   = 4;
    ctx->pcie_link_width_max = 16;
    fake_nvml_ret = 0;

    ASSERT(collector_start(ctx, exp) == 0);

    /* Spin-wait for ctx->ready with a 2-second timeout */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 2;
    while (!ctx->ready) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec))
            break;
        struct timespec ts = {0, 1000000L};  /* 1 ms */
        nanosleep(&ts, NULL);
    }

    collector_stop(ctx);

    ASSERT_EQ_INT(1, ctx->ready);
    ASSERT(ctx->ring.count > 0);
    /* Verify a few state fields were populated by the fake vtable */
    ASSERT_EQ_INT(4,  ctx->state.pcie_link_gen);
    ASSERT_EQ_INT(16, ctx->state.pcie_link_width);
    ASSERT_EQ_INT(1,  ctx->gpu_available);
    ASSERT_EQ_INT(1,  ctx->gpu_present);

    /* Consume the snapshot message sent to the socketpair */
    close(sv[0]);
    close(sv[1]);

    free(ctx->ring.samples);
    pthread_mutex_destroy(&ctx->ring_mutex);
    pthread_mutex_destroy(&ctx->state_mutex);
    pthread_mutex_destroy(&ctx->snapshot_mutex);
    pthread_mutex_destroy(&ctx->files_mutex);
    free(ctx);
    free(exp);
}

static void test_collector_nvml_error_increments_counter(void)
{
    /*
     * Fake vtable returns errors — verify collector_errors_total increments
     * and gpu_available eventually goes to 0.
     */
    int sv[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    exporter_t *exp = make_exp();
    exp->parent_fd = sv[0];
    /* Set threshold low so it trips after first error */
    exp->cfg.nvml_error_threshold = 1;
    exp->cfg.nvml_hard_error_threshold = 3;  /* don't want hard trip here */

    gpu_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->gpu_index        = 0;
    ctx->gpu_present      = 1;
    ctx->gpu_available    = 1;
    ctx->nvml_handle      = (void *)0xDEADBEEF;
    ctx->pcie_link_gen_max   = 4;
    ctx->pcie_link_width_max = 16;

    fake_nvml_ret = NVML_ERROR_UNKNOWN;  /* cause NVML to fail */

    ASSERT(collector_start(ctx, exp) == 0);

    /* Wait for ready (set even on errors so startup doesn't block forever) */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 2;
    while (!ctx->ready) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec)
            break;
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }

    collector_stop(ctx);

    ASSERT_EQ_INT(1, ctx->ready);
    ASSERT(ctx->collector_errors_total > 0);
    ASSERT_EQ_INT(0, ctx->gpu_available);

    fake_nvml_ret = 0;  /* restore for subsequent tests */

    close(sv[0]);
    close(sv[1]);
    free(ctx->ring.samples);
    pthread_mutex_destroy(&ctx->ring_mutex);
    pthread_mutex_destroy(&ctx->state_mutex);
    pthread_mutex_destroy(&ctx->snapshot_mutex);
    pthread_mutex_destroy(&ctx->files_mutex);
    free(ctx);
    free(exp);
}

static void test_collector_hard_error_marks_gpu_lost(void)
{
    int sv[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    exporter_t *exp = make_exp();
    exp->parent_fd = sv[0];
    exp->cfg.nvml_hard_error_threshold = 1;  /* trip on first hard error */

    gpu_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->gpu_index        = 0;
    ctx->gpu_present      = 1;
    ctx->gpu_available    = 1;
    ctx->nvml_handle      = (void *)0xDEADBEEF;
    ctx->pcie_link_gen_max   = 4;
    ctx->pcie_link_width_max = 16;

    fake_nvml_ret = NVML_ERROR_GPU_IS_LOST;

    ASSERT(collector_start(ctx, exp) == 0);

    /* Thread exits on its own once gpu_present=0; wait for join */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 3;
    while (ctx->gpu_present) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec)
            break;
        struct timespec ts = {0, 5000000L};
        nanosleep(&ts, NULL);
    }

    /* Thread breaks out of loop when gpu_present=0; stop completes the join */
    ctx->stop = 1;
    collector_stop(ctx);

    ASSERT_EQ_INT(0, ctx->gpu_present);

    fake_nvml_ret = 0;
    close(sv[0]);
    close(sv[1]);
    free(ctx->ring.samples);
    pthread_mutex_destroy(&ctx->ring_mutex);
    pthread_mutex_destroy(&ctx->state_mutex);
    pthread_mutex_destroy(&ctx->snapshot_mutex);
    pthread_mutex_destroy(&ctx->files_mutex);
    free(ctx);
    free(exp);
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void)
{
    RUN_TEST(test_ring_push_first_entry);
    RUN_TEST(test_ring_push_advances_head);
    RUN_TEST(test_ring_push_wraps_at_capacity);
    RUN_TEST(test_ring_push_count_never_exceeds_capacity);
    RUN_TEST(test_ring_push_single_capacity);

    RUN_TEST(test_snapshot_update_identity);
    RUN_TEST(test_snapshot_update_score_fields);
    RUN_TEST(test_snapshot_update_state_fields);
    RUN_TEST(test_snapshot_update_exporter_state);
    RUN_TEST(test_snapshot_update_ring_values_reflected);
    RUN_TEST(test_snapshot_update_baseline_age);

    RUN_TEST(test_snapshot_send_recv_roundtrip);
    RUN_TEST(test_snapshot_recv_eof_returns_minus1);
    RUN_TEST(test_snapshot_send_multiple_gpus);

    RUN_TEST(test_collector_start_stop_immediate);
    RUN_TEST(test_collector_completes_one_poll_cycle);
    RUN_TEST(test_collector_nvml_error_increments_counter);
    RUN_TEST(test_collector_hard_error_marks_gpu_lost);

    return TEST_RESULT();
}
