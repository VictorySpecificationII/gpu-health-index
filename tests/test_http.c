/*
 * test_http.c — integration tests for http.c
 *
 * Strategy: fork the HTTP child with a real socketpair, send fake
 * gpu_ipc_msg_t snapshots from the parent, then connect via TCP to
 * localhost:TEST_PORT and issue HTTP requests.  The child process runs
 * http_child_run() and responds to requests normally.
 *
 * Tests are ordered deliberately:
 *   Group A — before any snapshot is sent (/ready 503, /live 503)
 *   Group B — after one snapshot is sent   (/ready 200, /live 200,
 *             /metrics content checks)
 */

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "http.h"
#include "snapshot.h"
#include "types.h"
#include "util.h"
#include "test_harness.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#define TEST_PORT    19108
#define TEST_SERIAL  "1321021036987"
#define TEST_UUID    "GPU-deadbeef-1234-5678-abcd-000000000001"
#define TEST_MODEL   "NVIDIA A100-SXM4-80GB"
#define TEST_DRIVER  "535.104.05"
#define NUM_GPUS     1

/* --------------------------------------------------------------------------
 * Shared fixture state
 * -------------------------------------------------------------------------- */

static int   g_parent_fd = -1;
static pid_t g_child_pid = -1;

/* --------------------------------------------------------------------------
 * Build a realistic fake snapshot.
 * Set mem_bw_util_pct = NaN to simulate DCGM-unavailable state.
 * -------------------------------------------------------------------------- */

static void make_fake_snapshot(gpu_snapshot_t *s) {
    memset(s, 0, sizeof(*s));

    strncpy(s->serial,         TEST_SERIAL, sizeof(s->serial) - 1);
    strncpy(s->uuid,           TEST_UUID,   sizeof(s->uuid)   - 1);
    strncpy(s->gpu_model,      TEST_MODEL,  sizeof(s->gpu_model) - 1);
    strncpy(s->driver_version, TEST_DRIVER, sizeof(s->driver_version) - 1);

    s->gpu_index           = 0;
    s->identity_source     = GPU_IDENTITY_SERIAL;
    s->pcie_link_gen_max   = 4;
    s->pcie_link_width_max = 16;

    s->score          = 91.5;
    s->classification = GPU_CLASS_HEALTHY;
    s->reason_mask    = 0;
    s->telemetry_ok   = 1;

    s->temp_p95_c            = 72.3;
    s->hbm_temp_p95_c        = 68.1;
    s->clk_std_mhz           = 15.2;
    s->power_saturation_frac = 0.12;
    s->ecc_sbe_rate_per_hour = 0.0;
    s->ecc_dbe_in_window     = 0;

    s->perf_drop_frac  = NAN; /* no baseline — NaN expected in output */
    s->probe_available = 0;
    s->probe_stale     = 0;

    s->baseline_available = 0;
    s->baseline_valid     = 0;

    s->temp_c     = 73.0;
    s->hbm_temp_c = 69.0;
    s->fan_speed_pct = -1; /* liquid-cooled — should be absent from /metrics */

    s->power_w       = 312.5;
    s->power_limit_w = 400.0;
    s->board_power_w = NAN; /* DCGM unavailable */
    s->energy_j      = NAN; /* DCGM unavailable */

    s->mem_bw_util_pct = NAN; /* DCGM unavailable — infers dcgm_available=0 */

    s->mem_used_bytes  = (uint64_t)40 * 1024 * 1024 * 1024;
    s->mem_total_bytes = (uint64_t)80 * 1024 * 1024 * 1024;
    s->mem_free_bytes  = s->mem_total_bytes - s->mem_used_bytes;

    s->pcie_link_gen   = 4;
    s->pcie_link_width = 16;

    s->sm_clock_mhz  = 1410.0;
    s->mem_clock_mhz = 1215.0;
    s->util_gpu_pct  = 87;
    s->util_mem_pct  = 42;
    s->pstate        = 0;

    s->gpu_present   = 1;
    s->gpu_available = 1;
    s->last_poll_ms  = time_now_ms();
}

/* --------------------------------------------------------------------------
 * Send one gpu_ipc_msg_t to the child via the socketpair.
 * -------------------------------------------------------------------------- */

static void send_snapshot(const gpu_snapshot_t *snap, int gpu_index) {
    gpu_ipc_msg_t msg;
    msg.gpu_index = (int32_t)gpu_index;
    msg.snapshot  = *snap;

    const char *p = (const char *)&msg;
    size_t rem = sizeof(msg);
    while (rem > 0) {
        ssize_t n = write(g_parent_fd, p, rem);
        if (n <= 0) break;
        p   += n;
        rem -= (size_t)n;
    }
}

/* --------------------------------------------------------------------------
 * Connect to localhost:TEST_PORT with retry (child may not be bound yet).
 * Returns a connected fd, or -1 on failure after all retries.
 * -------------------------------------------------------------------------- */

static int connect_to_server(void) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(TEST_PORT);
    inet_aton("127.0.0.1", &addr.sin_addr);

    for (int i = 0; i < 50; i++) {   /* up to 5 s */
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return fd;

        close(fd);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };
        nanosleep(&ts, NULL);
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * Issue one HTTP request; return the status code.
 * resp_buf receives the full response (headers + body); may be NULL.
 * -------------------------------------------------------------------------- */

static int http_request(const char *method, const char *path,
                        char *resp_buf, size_t resp_cap) {
    int fd = connect_to_server();
    if (fd < 0)
        return -1;

    /* 3-second receive timeout — tests should never block. */
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Send request. */
    char req[256];
    int req_len = snprintf(req, sizeof(req),
                           "%s %s HTTP/1.0\r\nHost: localhost\r\n\r\n",
                           method, path);
    {
        const char *p = req;
        int rem = req_len;
        while (rem > 0) {
            ssize_t n = write(fd, p, (size_t)rem);
            if (n <= 0) break;
            p += n; rem -= (int)n;
        }
    }

    /* Read full response until EOF (Connection: close). */
    char  local_buf[256 * 1024];
    char *buf = resp_buf ? resp_buf : local_buf;
    size_t cap = resp_buf ? resp_cap : sizeof(local_buf);
    size_t pos = 0;

    while (pos < cap - 1) {
        ssize_t n = read(fd, buf + pos, cap - pos - 1);
        if (n <= 0) break;
        pos += (size_t)n;
    }
    buf[pos] = '\0';
    close(fd);

    /* Parse status code from first line: "HTTP/1.1 200 OK\r\n" */
    int status = 0;
    sscanf(buf, "HTTP/1.%*d %d", &status);
    return status;
}

/* --------------------------------------------------------------------------
 * Fixture: start the HTTP child, wait until it is listening.
 * -------------------------------------------------------------------------- */

static void fixture_setup(void) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        perror("socketpair");
        exit(1);
    }

    /* Write the init message before fork so the child sees it immediately. */
    gpu_ipc_init_t init = { .num_gpus = (int32_t)NUM_GPUS };
    {
        const char *p = (const char *)&init;
        size_t rem = sizeof(init);
        while (rem > 0) {
            ssize_t n = write(fds[0], p, rem);
            if (n <= 0) { perror("write init"); exit(1); }
            p += n; rem -= (size_t)n;
        }
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }

    if (pid == 0) {
        /* Child — run the HTTP server. */
        close(fds[0]);
        signal(SIGPIPE, SIG_IGN);

        gpu_config_t cfg;
        config_load(NULL, &cfg);     /* get defaults */
        cfg.listen_port      = TEST_PORT;
        cfg.poll_interval_s  = 1;

        strncpy(cfg.listen_addr, "127.0.0.1", sizeof(cfg.listen_addr) - 1);

        http_child_run(fds[1], &cfg);
        _exit(0);
    }

    /* Parent */
    close(fds[1]);
    g_parent_fd = fds[0];
    g_child_pid = pid;

    /*
     * Wait for the child to bind.  connect_to_server() already retries for
     * up to 5 s, so any initial sleep here is just a hint, not a guarantee.
     */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000L }; /* 50 ms */
    nanosleep(&ts, NULL);
}

static void fixture_teardown(void) {
    if (g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
        int status;
        waitpid(g_child_pid, &status, 0);
        g_child_pid = -1;
    }
    if (g_parent_fd >= 0) {
        close(g_parent_fd);
        g_parent_fd = -1;
    }
}

/* ==========================================================================
 * Group A — before any snapshot is sent
 * ========================================================================== */

static void test_ready_before_snapshot(void) {
    int status = http_request("GET", "/ready", NULL, 0);
    ASSERT_EQ_INT(status, 503);
}

static void test_live_before_snapshot(void) {
    /* No snapshots yet — /live should return 200 (no stale data, just empty). */
    int status = http_request("GET", "/live", NULL, 0);
    ASSERT_EQ_INT(status, 200);
}

static void test_metrics_before_snapshot(void) {
    /* /metrics is always 200, even with no data — body just has no GPU lines. */
    int status = http_request("GET", "/metrics", NULL, 0);
    ASSERT_EQ_INT(status, 200);
}

static void test_404_unknown_path(void) {
    int status = http_request("GET", "/bogus", NULL, 0);
    ASSERT_EQ_INT(status, 404);
}

static void test_405_non_get(void) {
    int status = http_request("POST", "/metrics", NULL, 0);
    ASSERT_EQ_INT(status, 405);
}

/* ==========================================================================
 * Group B — after one snapshot is sent
 * ========================================================================== */

static void test_ready_after_snapshot(void) {
    /* Small pause to let the receiver thread process the snapshot. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000L };
    nanosleep(&ts, NULL);

    int status = http_request("GET", "/ready", NULL, 0);
    ASSERT_EQ_INT(status, 200);
}

static void test_live_after_snapshot(void) {
    int status = http_request("GET", "/live", NULL, 0);
    ASSERT_EQ_INT(status, 200);
}

static void test_metrics_status_and_content_type(void) {
    char resp[256 * 1024];
    int status = http_request("GET", "/metrics", resp, sizeof(resp));
    ASSERT_EQ_INT(status, 200);
    ASSERT(strstr(resp, "text/plain; version=0.0.4") != NULL);
}

static void test_metrics_contains_help_and_type_lines(void) {
    char resp[256 * 1024];
    http_request("GET", "/metrics", resp, sizeof(resp));
    ASSERT(strstr(resp, "# HELP gpu_health_score") != NULL);
    ASSERT(strstr(resp, "# TYPE gpu_health_score gauge")  != NULL);
    ASSERT(strstr(resp, "# HELP gpu_info")                != NULL);
    ASSERT(strstr(resp, "# TYPE gpu_info gauge")          != NULL);
}

static void test_metrics_contains_exporter_info(void) {
    char resp[256 * 1024];
    http_request("GET", "/metrics", resp, sizeof(resp));
    ASSERT(strstr(resp, "gpu_health_exporter_info{") != NULL);
    ASSERT(strstr(resp, "version=\"0.1.0\"")          != NULL);
}

static void test_metrics_contains_gpu_info_with_labels(void) {
    char resp[256 * 1024];
    http_request("GET", "/metrics", resp, sizeof(resp));
    ASSERT(strstr(resp, "gpu_info{")          != NULL);
    ASSERT(strstr(resp, TEST_SERIAL)          != NULL);
    ASSERT(strstr(resp, TEST_MODEL)           != NULL);
    ASSERT(strstr(resp, TEST_DRIVER)          != NULL);
}

static void test_metrics_score_value(void) {
    char resp[256 * 1024];
    http_request("GET", "/metrics", resp, sizeof(resp));

    /* Check the score line contains our serial and the expected value. */
    char expected[128];
    snprintf(expected, sizeof(expected),
             "gpu_health_score{serial=\"%s\"}", TEST_SERIAL);
    ASSERT(strstr(resp, expected) != NULL);

    /* Value 91.5 rendered with %.2f — look for "91.50" */
    ASSERT(strstr(resp, "91.50") != NULL);
}

static void test_metrics_nan_fields_present(void) {
    /* board_power_w, energy_j, mem_bw_util_pct were NaN in our snapshot.
     * Each should appear as "NaN" in the response body. */
    char resp[256 * 1024];
    http_request("GET", "/metrics", resp, sizeof(resp));
    ASSERT(strstr(resp, "gpu_board_power_watts")                    != NULL);
    ASSERT(strstr(resp, "gpu_energy_joules_total")                  != NULL);
    ASSERT(strstr(resp, "gpu_memory_bandwidth_utilization_ratio")   != NULL);
    ASSERT(strstr(resp, "NaN")                                      != NULL);
}

static void test_metrics_fan_absent_for_liquid_cooled(void) {
    /* fan_speed_pct = -1 → gpu_fan_speed_ratio must NOT appear in output. */
    char resp[256 * 1024];
    http_request("GET", "/metrics", resp, sizeof(resp));
    ASSERT(strstr(resp, "gpu_fan_speed_ratio{") == NULL);
}

static void test_metrics_dcgm_available_inferred_zero(void) {
    /* mem_bw_util_pct is NaN → dcgm_available should be 0. */
    char resp[256 * 1024];
    http_request("GET", "/metrics", resp, sizeof(resp));
    char expected[128];
    snprintf(expected, sizeof(expected),
             "gpu_dcgm_available{serial=\"%s\"} 0", TEST_SERIAL);
    ASSERT(strstr(resp, expected) != NULL);
}

static void test_metrics_telemetry_ok(void) {
    char resp[256 * 1024];
    http_request("GET", "/metrics", resp, sizeof(resp));
    char expected[128];
    snprintf(expected, sizeof(expected),
             "gpu_telemetry_ok{serial=\"%s\"} 1", TEST_SERIAL);
    ASSERT(strstr(resp, expected) != NULL);
}

static void test_metrics_pcie_values(void) {
    char resp[256 * 1024];
    http_request("GET", "/metrics", resp, sizeof(resp));
    char expected_gen[128], expected_width[128];
    snprintf(expected_gen,   sizeof(expected_gen),
             "gpu_pcie_link_gen{serial=\"%s\"} 4", TEST_SERIAL);
    snprintf(expected_width, sizeof(expected_width),
             "gpu_pcie_link_width{serial=\"%s\"} 16", TEST_SERIAL);
    ASSERT(strstr(resp, expected_gen)   != NULL);
    ASSERT(strstr(resp, expected_width) != NULL);
}

static void test_metrics_perf_drop_nan(void) {
    /* perf_drop_frac was NaN (no baseline) → should render as NaN. */
    char resp[256 * 1024];
    http_request("GET", "/metrics", resp, sizeof(resp));
    ASSERT(strstr(resp, "gpu_perf_drop_ratio") != NULL);
    /* The line for our GPU should contain NaN. */
    char *p = strstr(resp, "gpu_perf_drop_ratio{");
    ASSERT(p != NULL);
    if (p) ASSERT(strstr(p, "NaN") != NULL);
}

/* ==========================================================================
 * main
 * ========================================================================== */

int main(void) {
    log_init();
    fixture_setup();

    /* ---- Group A: no snapshot yet ----------------------------------------- */
    RUN_TEST(test_ready_before_snapshot);
    RUN_TEST(test_live_before_snapshot);
    RUN_TEST(test_metrics_before_snapshot);
    RUN_TEST(test_404_unknown_path);
    RUN_TEST(test_405_non_get);

    /* Send one snapshot; Group B tests verify /ready 200 and metrics content. */
    {
        gpu_snapshot_t snap;
        make_fake_snapshot(&snap);
        send_snapshot(&snap, 0);
    }

    /* ---- Group B: after snapshot ------------------------------------------ */
    RUN_TEST(test_ready_after_snapshot);
    RUN_TEST(test_live_after_snapshot);
    RUN_TEST(test_metrics_status_and_content_type);
    RUN_TEST(test_metrics_contains_help_and_type_lines);
    RUN_TEST(test_metrics_contains_exporter_info);
    RUN_TEST(test_metrics_contains_gpu_info_with_labels);
    RUN_TEST(test_metrics_score_value);
    RUN_TEST(test_metrics_nan_fields_present);
    RUN_TEST(test_metrics_fan_absent_for_liquid_cooled);
    RUN_TEST(test_metrics_dcgm_available_inferred_zero);
    RUN_TEST(test_metrics_telemetry_ok);
    RUN_TEST(test_metrics_pcie_values);
    RUN_TEST(test_metrics_perf_drop_nan);

    fixture_teardown();
    return TEST_RESULT();
}
