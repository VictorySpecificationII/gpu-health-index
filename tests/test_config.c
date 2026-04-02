#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_harness.h"
#include "config.h"
#include "types.h"
#include "util.h"

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Write content to a mkstemp temp file. Caller must unlink path when done. */
static int write_temp(const char *content, char *path, size_t pathlen)
{
    safe_strncpy(path, "/tmp/test_config_XXXXXX", pathlen);
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    size_t len = strlen(content);
    if (write(fd, content, len) != (ssize_t)len) {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    return 0;
}

/* -------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void test_defaults(void)
{
    gpu_config_t cfg;
    int r = config_load(NULL, &cfg);

    ASSERT_EQ_INT(r, 0);
    ASSERT_EQ_INT(cfg.listen_port,                 CFG_DEFAULT_LISTEN_PORT);
    ASSERT_EQ_INT(cfg.poll_interval_s,             CFG_DEFAULT_POLL_INTERVAL_S);
    ASSERT_EQ_INT(cfg.window_s,                    CFG_DEFAULT_WINDOW_S);
    ASSERT_EQ_INT(cfg.state_write_interval_s,      CFG_DEFAULT_STATE_WRITE_INTERVAL_S);
    ASSERT_EQ_INT(cfg.min_samples_absolute,        CFG_DEFAULT_MIN_SAMPLES_ABSOLUTE);
    ASSERT_EQ_INT(cfg.retired_pages_warn,          CFG_DEFAULT_RETIRED_PAGES_WARN);
    ASSERT_EQ_INT(cfg.retired_pages_bad,           CFG_DEFAULT_RETIRED_PAGES_BAD);
    ASSERT_EQ_INT(cfg.probe_interval_s,            CFG_DEFAULT_PROBE_INTERVAL_S);
    ASSERT_EQ_INT(cfg.probe_ttl_s,                 CFG_DEFAULT_PROBE_TTL_S);
    ASSERT_EQ_INT(cfg.nvml_timeout_ms,             CFG_DEFAULT_NVML_TIMEOUT_MS);
    ASSERT_EQ_INT(cfg.nvml_error_threshold,        CFG_DEFAULT_NVML_ERROR_THRESHOLD);
    ASSERT_EQ_INT(cfg.nvml_hard_error_threshold,   CFG_DEFAULT_NVML_HARD_THRESHOLD);
    ASSERT_EQ_INT(cfg.nvml_retry_interval_s,       CFG_DEFAULT_NVML_RETRY_INTERVAL_S);
    ASSERT_EQ_INT(cfg.dcgm_timeout_ms,             CFG_DEFAULT_DCGM_TIMEOUT_MS);
    ASSERT_EQ_INT(cfg.dcgm_error_threshold,        CFG_DEFAULT_DCGM_ERROR_THRESHOLD);
    ASSERT_EQ_INT(cfg.dcgm_retry_interval_s,       CFG_DEFAULT_DCGM_RETRY_INTERVAL_S);

    ASSERT_EQ_DBL(cfg.min_sample_ratio,            CFG_DEFAULT_MIN_SAMPLE_RATIO);
    ASSERT_EQ_DBL(cfg.max_median_step_s,           CFG_DEFAULT_MAX_MEDIAN_STEP_S);
    ASSERT_EQ_DBL(cfg.temp_p95_warn_c,             CFG_DEFAULT_TEMP_P95_WARN_C);
    ASSERT_EQ_DBL(cfg.temp_p95_bad_c,              CFG_DEFAULT_TEMP_P95_BAD_C);
    ASSERT_EQ_DBL(cfg.hbm_temp_p95_warn_c,         CFG_DEFAULT_HBM_TEMP_P95_WARN_C);
    ASSERT_EQ_DBL(cfg.hbm_temp_p95_bad_c,          CFG_DEFAULT_HBM_TEMP_P95_BAD_C);
    ASSERT_EQ_DBL(cfg.clk_std_warn_mhz,            CFG_DEFAULT_CLK_STD_WARN_MHZ);
    ASSERT_EQ_DBL(cfg.power_high_ratio,            CFG_DEFAULT_POWER_HIGH_RATIO);
    ASSERT_EQ_DBL(cfg.power_penalty_max,           CFG_DEFAULT_POWER_PENALTY_MAX);
    ASSERT_EQ_DBL(cfg.ecc_sbe_rate_warn_per_hour,  CFG_DEFAULT_ECC_SBE_RATE_WARN);
    ASSERT_EQ_DBL(cfg.ecc_sbe_penalty,             CFG_DEFAULT_ECC_SBE_PENALTY);
    ASSERT_EQ_DBL(cfg.ecc_dbe_penalty,             CFG_DEFAULT_ECC_DBE_PENALTY);
    ASSERT_EQ_DBL(cfg.row_remap_failure_penalty,   CFG_DEFAULT_ROW_REMAP_PENALTY);
    ASSERT_EQ_DBL(cfg.pcie_link_degraded_penalty,  CFG_DEFAULT_PCIE_GEN_PENALTY);
    ASSERT_EQ_DBL(cfg.pcie_width_degraded_penalty, CFG_DEFAULT_PCIE_WIDTH_PENALTY);
    ASSERT_EQ_DBL(cfg.perf_drop_warn,              CFG_DEFAULT_PERF_DROP_WARN);
    ASSERT_EQ_DBL(cfg.perf_drop_bad,               CFG_DEFAULT_PERF_DROP_BAD);
    ASSERT_EQ_DBL(cfg.perf_drop_severe,            CFG_DEFAULT_PERF_DROP_SEVERE);
    ASSERT_EQ_DBL(cfg.perf_drop_pen_warn,          CFG_DEFAULT_PERF_DROP_PEN_WARN);
    ASSERT_EQ_DBL(cfg.perf_drop_pen_bad,           CFG_DEFAULT_PERF_DROP_PEN_BAD);
    ASSERT_EQ_DBL(cfg.perf_drop_pen_severe,        CFG_DEFAULT_PERF_DROP_PEN_SEVERE);

    ASSERT_STR(cfg.state_dir,    CFG_DEFAULT_STATE_DIR);
    ASSERT_STR(cfg.baseline_dir, CFG_DEFAULT_BASELINE_DIR);
    ASSERT_STR(cfg.listen_addr,  CFG_DEFAULT_LISTEN_ADDR);
    ASSERT_STR(cfg.tls_cert_path, "");
    ASSERT_STR(cfg.tls_key_path,  "");
}

static void test_parse_file(void)
{
    char path[64];
    const char *content =
        "# full-line comment\n"
        "listen_port = 9999\n"
        "poll_interval_s=2\n"
        "  temp_p95_warn_c  =  75.0  \n"   /* whitespace around = and value */
        "state_dir = /tmp/gpu-health-test\n"
        "power_high_ratio = 0.95\n"
        "ecc_sbe_rate_warn_per_hour = 50.0\n";

    ASSERT(write_temp(content, path, sizeof(path)) == 0);

    gpu_config_t cfg;
    ASSERT_EQ_INT(config_load(path, &cfg), 0);
    ASSERT_EQ_INT(cfg.listen_port,             9999);
    ASSERT_EQ_INT(cfg.poll_interval_s,         2);
    ASSERT_EQ_DBL(cfg.temp_p95_warn_c,         75.0);
    ASSERT_EQ_DBL(cfg.power_high_ratio,        0.95);
    ASSERT_EQ_DBL(cfg.ecc_sbe_rate_warn_per_hour, 50.0);
    ASSERT_STR(cfg.state_dir, "/tmp/gpu-health-test");

    /* Fields not in the file must still be defaults */
    ASSERT_EQ_INT(cfg.window_s, CFG_DEFAULT_WINDOW_S);

    unlink(path);
}

static void test_unknown_key_ignored(void)
{
    char path[64];
    /* Unknown key must not cause failure — forward compatibility */
    const char *content =
        "unknown_key_that_does_not_exist = 42\n"
        "listen_port = 1234\n";

    ASSERT(write_temp(content, path, sizeof(path)) == 0);

    gpu_config_t cfg;
    ASSERT_EQ_INT(config_load(path, &cfg), 0);
    ASSERT_EQ_INT(cfg.listen_port, 1234);

    unlink(path);
}

static void test_invalid_value_fails(void)
{
    char path[64];
    const char *content = "listen_port = notanumber\n";

    ASSERT(write_temp(content, path, sizeof(path)) == 0);
    gpu_config_t cfg;
    ASSERT_EQ_INT(config_load(path, &cfg), -1);
    unlink(path);
}

static void test_out_of_range_fails(void)
{
    char path[64];
    gpu_config_t cfg;

    /* Port above 65535 */
    ASSERT(write_temp("listen_port = 99999\n", path, sizeof(path)) == 0);
    ASSERT_EQ_INT(config_load(path, &cfg), -1);
    unlink(path);

    /* Port = 0 (below min of 1) */
    ASSERT(write_temp("listen_port = 0\n", path, sizeof(path)) == 0);
    ASSERT_EQ_INT(config_load(path, &cfg), -1);
    unlink(path);

    /* min_sample_ratio above 1.0 */
    ASSERT(write_temp("min_sample_ratio = 1.5\n", path, sizeof(path)) == 0);
    ASSERT_EQ_INT(config_load(path, &cfg), -1);
    unlink(path);

    /* Negative poll interval */
    ASSERT(write_temp("poll_interval_s = -1\n", path, sizeof(path)) == 0);
    ASSERT_EQ_INT(config_load(path, &cfg), -1);
    unlink(path);
}

static void test_env_override(void)
{
    setenv("GPU_HEALTH_LISTEN_PORT", "8888", 1);
    setenv("GPU_HEALTH_POLL_INTERVAL_S", "5", 1);
    setenv("GPU_HEALTH_STATE_DIR", "/tmp/env-override", 1);

    gpu_config_t cfg;
    ASSERT_EQ_INT(config_load(NULL, &cfg), 0);
    ASSERT_EQ_INT(cfg.listen_port,      8888);
    ASSERT_EQ_INT(cfg.poll_interval_s,  5);
    ASSERT_STR(cfg.state_dir, "/tmp/env-override");

    unsetenv("GPU_HEALTH_LISTEN_PORT");
    unsetenv("GPU_HEALTH_POLL_INTERVAL_S");
    unsetenv("GPU_HEALTH_STATE_DIR");
}

static void test_env_overrides_file(void)
{
    char path[64];
    const char *content = "listen_port = 7777\n";
    ASSERT(write_temp(content, path, sizeof(path)) == 0);

    setenv("GPU_HEALTH_LISTEN_PORT", "6666", 1);  /* env must win */

    gpu_config_t cfg;
    ASSERT_EQ_INT(config_load(path, &cfg), 0);
    ASSERT_EQ_INT(cfg.listen_port, 6666);

    unsetenv("GPU_HEALTH_LISTEN_PORT");
    unlink(path);
}

static void test_env_invalid_value_fails(void)
{
    setenv("GPU_HEALTH_LISTEN_PORT", "notanumber", 1);

    gpu_config_t cfg;
    ASSERT_EQ_INT(config_load(NULL, &cfg), -1);

    unsetenv("GPU_HEALTH_LISTEN_PORT");
}

static void test_validate_accepts_defaults(void)
{
    gpu_config_t cfg;
    ASSERT_EQ_INT(config_load(NULL, &cfg), 0);
    ASSERT_EQ_INT(config_validate(&cfg), 0);
}

static void test_validate_temp_ordering(void)
{
    gpu_config_t cfg;
    config_load(NULL, &cfg);

    /* warn >= bad: must fail */
    cfg.temp_p95_warn_c = 90.0;
    cfg.temp_p95_bad_c  = 80.0;
    ASSERT_EQ_INT(config_validate(&cfg), -1);

    /* warn == bad: must fail */
    cfg.temp_p95_warn_c = 80.0;
    cfg.temp_p95_bad_c  = 80.0;
    ASSERT_EQ_INT(config_validate(&cfg), -1);

    /* warn < bad: must pass (reset hbm to valid first) */
    cfg.temp_p95_warn_c = 80.0;
    cfg.temp_p95_bad_c  = 90.0;
    ASSERT_EQ_INT(config_validate(&cfg), 0);
}

static void test_validate_perf_drop_ordering(void)
{
    gpu_config_t cfg;
    config_load(NULL, &cfg);

    cfg.perf_drop_warn   = 0.10;
    cfg.perf_drop_bad    = 0.05;  /* warn >= bad */
    cfg.perf_drop_severe = 0.12;
    ASSERT_EQ_INT(config_validate(&cfg), -1);

    cfg.perf_drop_warn   = 0.03;
    cfg.perf_drop_bad    = 0.07;
    cfg.perf_drop_severe = 0.07;  /* bad >= severe */
    ASSERT_EQ_INT(config_validate(&cfg), -1);
}

static void test_validate_probe_ttl(void)
{
    gpu_config_t cfg;
    config_load(NULL, &cfg);

    cfg.probe_interval_s = 86400;
    cfg.probe_ttl_s      = 60;    /* ttl < interval: must fail */
    ASSERT_EQ_INT(config_validate(&cfg), -1);

    cfg.probe_ttl_s = 86400;      /* ttl == interval: must pass */
    ASSERT_EQ_INT(config_validate(&cfg), 0);
}

static void test_validate_retired_pages_ordering(void)
{
    gpu_config_t cfg;
    config_load(NULL, &cfg);

    cfg.retired_pages_warn = 10;
    cfg.retired_pages_bad  = 5;   /* warn > bad: must fail */
    ASSERT_EQ_INT(config_validate(&cfg), -1);

    cfg.retired_pages_warn = 5;
    cfg.retired_pages_bad  = 5;   /* equal: must pass */
    ASSERT_EQ_INT(config_validate(&cfg), 0);
}

static void test_missing_file_fails(void)
{
    gpu_config_t cfg;
    ASSERT_EQ_INT(config_load("/tmp/does_not_exist_gpu_health.conf", &cfg), -1);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

int main(void)
{
    fprintf(stderr, "test_config\n");

    RUN_TEST(test_defaults);
    RUN_TEST(test_parse_file);
    RUN_TEST(test_unknown_key_ignored);
    RUN_TEST(test_invalid_value_fails);
    RUN_TEST(test_out_of_range_fails);
    RUN_TEST(test_env_override);
    RUN_TEST(test_env_overrides_file);
    RUN_TEST(test_env_invalid_value_fails);
    RUN_TEST(test_validate_accepts_defaults);
    RUN_TEST(test_validate_temp_ordering);
    RUN_TEST(test_validate_perf_drop_ordering);
    RUN_TEST(test_validate_probe_ttl);
    RUN_TEST(test_validate_retired_pages_ordering);
    RUN_TEST(test_missing_file_fails);

    return TEST_RESULT();
}
