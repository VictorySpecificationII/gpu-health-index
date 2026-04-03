#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "test_harness.h"
#include "state.h"
#include "types.h"
#include "util.h"

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static int write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(content, f);
    fclose(f);
    return 0;
}

/* Make a temp directory, returns 0 on success. */
static int make_tmpdir(char *path, size_t len)
{
    safe_strncpy(path, "/tmp/test_state_dir_XXXXXX", len);
    return mkdtemp(path) ? 0 : -1;
}

/* Remove a directory and its contents (one level deep). */
static void rm_tmpdir(const char *path)
{
    /* We only ever put serial-named files in our test dirs, so this is safe */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", path);
    int rc = system(cmd); (void)rc;
}

/* =========================================================================
 * baseline_load tests
 * ========================================================================= */

static void test_baseline_missing_file(void)
{
    gpu_baseline_t b;
    baseline_load("/tmp/no_such_dir_xyz", "1234567890", &b);

    ASSERT_EQ_INT(b.available, 0);
    ASSERT_EQ_INT(b.valid,     0);
}

static void test_baseline_valid(void)
{
    char dir[64], path[128];
    ASSERT(make_tmpdir(dir, sizeof(dir)) == 0);

    snprintf(path, sizeof(path), "%s/1321021036987", dir);
    ASSERT(write_file(path,
        "serial=1321021036987\n"
        "uuid=GPU-abc123\n"
        "driver_version=535.104.05\n"
        "perf_w_mean=0.874878\n"
        "established_at=2026-03-15T14:22:00Z\n"
        "workload=cublas_bf16_gemm_n8192\n"
        "sample_count=270\n"
    ) == 0);

    gpu_baseline_t b;
    baseline_load(dir, "1321021036987", &b);

    ASSERT_EQ_INT(b.available,       1);
    ASSERT_EQ_INT(b.valid,           1);
    ASSERT_EQ_INT(b.serial_mismatch, 0);
    ASSERT_EQ_INT(b.driver_mismatch, 0);
    ASSERT_STR(b.serial,          "1321021036987");
    ASSERT_STR(b.uuid,            "GPU-abc123");
    ASSERT_STR(b.driver_version,  "535.104.05");
    ASSERT_STR(b.workload,        "cublas_bf16_gemm_n8192");
    ASSERT_EQ_INT(b.sample_count, 270);
    ASSERT(b.perf_w_mean > 0.87 && b.perf_w_mean < 0.88);
    ASSERT(b.established_at_s > 0);

    rm_tmpdir(dir);
}

static void test_baseline_serial_mismatch(void)
{
    char dir[64], path[128];
    ASSERT(make_tmpdir(dir, sizeof(dir)) == 0);

    snprintf(path, sizeof(path), "%s/9999999999", dir);
    ASSERT(write_file(path,
        "serial=1321021036987\n"           /* file serial != request serial */
        "perf_w_mean=0.874878\n"
        "established_at=2026-03-15T14:22:00Z\n"
        "workload=cublas_bf16_gemm_n8192\n"
        "sample_count=270\n"
    ) == 0);

    gpu_baseline_t b;
    baseline_load(dir, "9999999999", &b);

    ASSERT_EQ_INT(b.available,       1);
    ASSERT_EQ_INT(b.valid,           0);
    ASSERT_EQ_INT(b.serial_mismatch, 1);

    rm_tmpdir(dir);
}

static void test_baseline_missing_required_field(void)
{
    char dir[64], path[128];
    ASSERT(make_tmpdir(dir, sizeof(dir)) == 0);

    /* perf_w_mean is absent */
    snprintf(path, sizeof(path), "%s/1321021036987", dir);
    ASSERT(write_file(path,
        "serial=1321021036987\n"
        "established_at=2026-03-15T14:22:00Z\n"
        "workload=cublas_bf16_gemm_n8192\n"
        "sample_count=270\n"
    ) == 0);

    gpu_baseline_t b;
    baseline_load(dir, "1321021036987", &b);

    ASSERT_EQ_INT(b.available, 1);
    ASSERT_EQ_INT(b.valid,     0);

    rm_tmpdir(dir);
}

static void test_baseline_invalid_perf_w(void)
{
    char dir[64], path[128];
    ASSERT(make_tmpdir(dir, sizeof(dir)) == 0);

    snprintf(path, sizeof(path), "%s/1321021036987", dir);
    ASSERT(write_file(path,
        "serial=1321021036987\n"
        "perf_w_mean=not_a_number\n"
        "established_at=2026-03-15T14:22:00Z\n"
        "workload=cublas_bf16_gemm_n8192\n"
        "sample_count=270\n"
    ) == 0);

    gpu_baseline_t b;
    baseline_load(dir, "1321021036987", &b);

    ASSERT_EQ_INT(b.available, 1);
    ASSERT_EQ_INT(b.valid,     0);

    rm_tmpdir(dir);
}

static void test_baseline_invalid_timestamp(void)
{
    char dir[64], path[128];
    ASSERT(make_tmpdir(dir, sizeof(dir)) == 0);

    snprintf(path, sizeof(path), "%s/1321021036987", dir);
    ASSERT(write_file(path,
        "serial=1321021036987\n"
        "perf_w_mean=0.874878\n"
        "established_at=not-a-date\n"
        "workload=cublas_bf16_gemm_n8192\n"
        "sample_count=270\n"
    ) == 0);

    gpu_baseline_t b;
    baseline_load(dir, "1321021036987", &b);

    ASSERT_EQ_INT(b.valid, 0);

    rm_tmpdir(dir);
}

static void test_baseline_unknown_keys_ignored(void)
{
    char dir[64], path[128];
    ASSERT(make_tmpdir(dir, sizeof(dir)) == 0);

    snprintf(path, sizeof(path), "%s/1321021036987", dir);
    ASSERT(write_file(path,
        "serial=1321021036987\n"
        "perf_w_mean=0.874878\n"
        "established_at=2026-03-15T14:22:00Z\n"
        "workload=cublas_bf16_gemm_n8192\n"
        "sample_count=270\n"
        "future_field=some_value\n"    /* unknown — must be ignored */
        "another_unknown=42\n"
    ) == 0);

    gpu_baseline_t b;
    baseline_load(dir, "1321021036987", &b);

    ASSERT_EQ_INT(b.available, 1);
    ASSERT_EQ_INT(b.valid,     1);

    rm_tmpdir(dir);
}

static void test_baseline_caller_sets_driver_mismatch(void)
{
    /* Verify driver_version is parsed; mismatch detection is caller's job */
    char dir[64], path[128];
    ASSERT(make_tmpdir(dir, sizeof(dir)) == 0);

    snprintf(path, sizeof(path), "%s/1321021036987", dir);
    ASSERT(write_file(path,
        "serial=1321021036987\n"
        "driver_version=535.104.05\n"
        "perf_w_mean=0.874878\n"
        "established_at=2026-03-15T14:22:00Z\n"
        "workload=cublas_bf16_gemm_n8192\n"
        "sample_count=270\n"
    ) == 0);

    gpu_baseline_t b;
    baseline_load(dir, "1321021036987", &b);

    ASSERT_EQ_INT(b.valid, 1);
    ASSERT_STR(b.driver_version, "535.104.05");

    /* Caller simulates driver mismatch detection */
    if (strcmp(b.driver_version, "560.35.03") != 0)
        b.driver_mismatch = 1;

    ASSERT_EQ_INT(b.driver_mismatch, 1);
    ASSERT_EQ_INT(b.valid,           1);   /* mismatch is soft — still valid */

    rm_tmpdir(dir);
}

/* =========================================================================
 * probe_load tests
 * ========================================================================= */

static void test_probe_missing_file(void)
{
    gpu_probe_result_t p;
    probe_load("/tmp/no_such_dir_xyz", "1234567890", 129600, &p);
    ASSERT_EQ_INT(p.available, 0);
}

static void test_probe_valid_fresh(void)
{
    char dir[64], path[128];
    ASSERT(make_tmpdir(dir, sizeof(dir)) == 0);

    /* Use a recent timestamp so it won't be stale */
    uint64_t now = time_now_s();
    char ts[32];
    time_iso8601(now - 3600, ts, sizeof(ts));  /* 1 hour ago */

    char content[512];
    snprintf(content, sizeof(content),
        "serial=1321021036987\n"
        "perf_w_mean=0.874878\n"
        "probe_timestamp=%s\n"
        "probe_exit_code=0\n"
        "workload=cublas_bf16_gemm_n8192\n"
        "sample_count=270\n"
        "probe_duration_s=330.5\n",
        ts);

    snprintf(path, sizeof(path), "%s/1321021036987.probe", dir);
    ASSERT(write_file(path, content) == 0);

    gpu_probe_result_t p;
    probe_load(dir, "1321021036987", 129600, &p);

    ASSERT_EQ_INT(p.available,       1);
    ASSERT_EQ_INT(p.stale,           0);
    ASSERT_EQ_INT(p.probe_exit_code, 0);
    ASSERT_EQ_INT(p.sample_count,    270);
    ASSERT(p.perf_w_mean > 0.87 && p.perf_w_mean < 0.88);
    ASSERT(p.probe_duration_s > 330.0 && p.probe_duration_s < 331.0);
    ASSERT_STR(p.workload, "cublas_bf16_gemm_n8192");

    rm_tmpdir(dir);
}

static void test_probe_stale(void)
{
    char dir[64], path[128];
    ASSERT(make_tmpdir(dir, sizeof(dir)) == 0);

    /* Timestamp is 48 hours ago; ttl is 36 hours → stale */
    uint64_t old = time_now_s() - (48 * 3600);
    char ts[32];
    time_iso8601(old, ts, sizeof(ts));

    char content[512];
    snprintf(content, sizeof(content),
        "serial=1321021036987\n"
        "perf_w_mean=0.874878\n"
        "probe_timestamp=%s\n"
        "probe_exit_code=0\n",
        ts);

    snprintf(path, sizeof(path), "%s/1321021036987.probe", dir);
    ASSERT(write_file(path, content) == 0);

    gpu_probe_result_t p;
    probe_load(dir, "1321021036987", 129600, &p);   /* 129600s = 36h */

    ASSERT_EQ_INT(p.available, 1);
    ASSERT_EQ_INT(p.stale,     1);

    rm_tmpdir(dir);
}

static void test_probe_fresh_at_ttl_boundary(void)
{
    char dir[64], path[128];
    ASSERT(make_tmpdir(dir, sizeof(dir)) == 0);

    /* Timestamp is 35h 59m ago; ttl is 36h → not stale */
    uint64_t recent = time_now_s() - (36 * 3600 - 60);
    char ts[32];
    time_iso8601(recent, ts, sizeof(ts));

    char content[512];
    snprintf(content, sizeof(content),
        "serial=1321021036987\n"
        "perf_w_mean=0.874878\n"
        "probe_timestamp=%s\n"
        "probe_exit_code=0\n",
        ts);

    snprintf(path, sizeof(path), "%s/1321021036987.probe", dir);
    ASSERT(write_file(path, content) == 0);

    gpu_probe_result_t p;
    probe_load(dir, "1321021036987", 129600, &p);

    ASSERT_EQ_INT(p.available, 1);
    ASSERT_EQ_INT(p.stale,     0);

    rm_tmpdir(dir);
}

static void test_probe_nonzero_exit_available(void)
{
    /* Non-zero exit code: probe is still parsed and available.
     * The scorer may choose to ignore it, but we report it faithfully. */
    char dir[64], path[128];
    ASSERT(make_tmpdir(dir, sizeof(dir)) == 0);

    uint64_t now = time_now_s();
    char ts[32];
    time_iso8601(now - 60, ts, sizeof(ts));

    char content[512];
    snprintf(content, sizeof(content),
        "serial=1321021036987\n"
        "perf_w_mean=0.874878\n"
        "probe_timestamp=%s\n"
        "probe_exit_code=1\n",
        ts);

    snprintf(path, sizeof(path), "%s/1321021036987.probe", dir);
    ASSERT(write_file(path, content) == 0);

    gpu_probe_result_t p;
    probe_load(dir, "1321021036987", 129600, &p);

    ASSERT_EQ_INT(p.available,       1);
    ASSERT_EQ_INT(p.stale,           0);
    ASSERT_EQ_INT(p.probe_exit_code, 1);

    rm_tmpdir(dir);
}

static void test_probe_missing_required_field(void)
{
    char dir[64], path[128];
    ASSERT(make_tmpdir(dir, sizeof(dir)) == 0);

    /* probe_timestamp is missing */
    snprintf(path, sizeof(path), "%s/1321021036987.probe", dir);
    ASSERT(write_file(path,
        "serial=1321021036987\n"
        "perf_w_mean=0.874878\n"
        "probe_exit_code=0\n"
    ) == 0);

    gpu_probe_result_t p;
    probe_load(dir, "1321021036987", 129600, &p);

    ASSERT_EQ_INT(p.available, 0);

    rm_tmpdir(dir);
}

/* =========================================================================
 * inotify tests
 * ========================================================================= */

static void test_inotify_init_valid_dir(void)
{
    char dir[64];
    ASSERT(make_tmpdir(dir, sizeof(dir)) == 0);

    int fd = baseline_inotify_init(dir);
    ASSERT(fd >= 0);

    /* No events yet — check should return 0 */
    ASSERT_EQ_INT(baseline_inotify_check(fd), 0);

    close(fd);
    rm_tmpdir(dir);
}

static void test_inotify_detects_write(void)
{
    char dir[64], path[128];
    ASSERT(make_tmpdir(dir, sizeof(dir)) == 0);

    int fd = baseline_inotify_init(dir);
    ASSERT(fd >= 0);

    /* Write a file into the watched directory */
    snprintf(path, sizeof(path), "%s/1321021036987", dir);
    ASSERT(write_file(path, "serial=1321021036987\n") == 0);

    /* Give inotify a moment (non-blocking, event should already be queued) */
    ASSERT(baseline_inotify_check(fd) == 1);

    /* Second check — events were drained, should be 0 */
    ASSERT_EQ_INT(baseline_inotify_check(fd), 0);

    close(fd);
    rm_tmpdir(dir);
}

static void test_inotify_invalid_dir(void)
{
    int fd = baseline_inotify_init("/tmp/no_such_dir_for_inotify_xyz");
    ASSERT_EQ_INT(fd, -1);
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

int main(void)
{
    fprintf(stderr, "test_state\n");

    RUN_TEST(test_baseline_missing_file);
    RUN_TEST(test_baseline_valid);
    RUN_TEST(test_baseline_serial_mismatch);
    RUN_TEST(test_baseline_missing_required_field);
    RUN_TEST(test_baseline_invalid_perf_w);
    RUN_TEST(test_baseline_invalid_timestamp);
    RUN_TEST(test_baseline_unknown_keys_ignored);
    RUN_TEST(test_baseline_caller_sets_driver_mismatch);

    RUN_TEST(test_probe_missing_file);
    RUN_TEST(test_probe_valid_fresh);
    RUN_TEST(test_probe_stale);
    RUN_TEST(test_probe_fresh_at_ttl_boundary);
    RUN_TEST(test_probe_nonzero_exit_available);
    RUN_TEST(test_probe_missing_required_field);

    RUN_TEST(test_inotify_init_valid_dir);
    RUN_TEST(test_inotify_detects_write);
    RUN_TEST(test_inotify_invalid_dir);

    return TEST_RESULT();
}
