/*
 * gpu_health_probe.cu — BF16 GEMM burn-in probe for gpu-health-exporter.
 *
 * Runs a sustained cuBLAS BF16 GEMM workload on a single GPU, samples NVML
 * power at 1-second intervals during the measurement window, and writes a
 * .probe file in the key=value format read by gpu-health-exporter's state.c.
 *
 * With --establish-baseline the same run also writes the long-lived baseline
 * file.  This is the correct path for initial fleet deployment: run once per
 * GPU after installation, promote the result to the baseline, then schedule
 * periodic probe runs via systemd timer (bare metal) or Kubernetes CronJob.
 *
 * Metric: TFLOPS/W (BF16 tensor-op TFLOPS divided by mean board power in W).
 * The workload field in both output files records the matrix size and metric
 * unit so the exporter can sanity-check that probe and baseline used the same
 * workload.
 *
 * Build: see probe/Makefile
 * Arch:  sm_80 (A100/A10), sm_89 (L40S), sm_90 (H100/H200)
 */

#include <cublas_v2.h>
#include <cuda_bf16.h>
#include <cuda_runtime.h>
#include <nvml.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#define PROBE_VERSION        "0.1.0"
#define DEFAULT_STATE_DIR    "/var/run/gpu-health"
#define DEFAULT_BASELINE_DIR "/etc/gpu-health/baseline"
#define DEFAULT_DURATION_S   300
#define DEFAULT_WARMUP_S     30
#define DEFAULT_MATRIX_N     8192
#define MAX_SAMPLES          4096
#define MIN_VALID_SAMPLES    5
#define MIN_POWER_W          10.0   /* ignore samples below this (idle/init) */

/* --------------------------------------------------------------------------
 * Error-checking macros
 * -------------------------------------------------------------------------- */

#define CUDA_CHECK(expr) do {                                               \
    cudaError_t _e = (expr);                                                \
    if (_e != cudaSuccess) {                                                \
        fprintf(stderr, "probe: CUDA error at %s:%d: %s\n",                \
                __FILE__, __LINE__, cudaGetErrorString(_e));                \
        return 1;                                                           \
    }                                                                       \
} while (0)

#define CUBLAS_CHECK(expr) do {                                             \
    cublasStatus_t _s = (expr);                                             \
    if (_s != CUBLAS_STATUS_SUCCESS) {                                      \
        fprintf(stderr, "probe: cuBLAS error at %s:%d: status=%d\n",       \
                __FILE__, __LINE__, (int)_s);                               \
        return 1;                                                           \
    }                                                                       \
} while (0)

#define NVML_CHECK(expr) do {                                               \
    nvmlReturn_t _r = (expr);                                               \
    if (_r != NVML_SUCCESS) {                                               \
        fprintf(stderr, "probe: NVML error at %s:%d: %s\n",                \
                __FILE__, __LINE__, nvmlErrorString(_r));                   \
        return 2;                                                           \
    }                                                                       \
} while (0)

/* --------------------------------------------------------------------------
 * Utilities
 * -------------------------------------------------------------------------- */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

static void iso8601_now(char *buf, size_t len) {
    time_t t = time(NULL);
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

/* --------------------------------------------------------------------------
 * File writers — atomic (write to .tmp, rename)
 * -------------------------------------------------------------------------- */

static int write_probe_file(
    const char *state_dir,
    const char *serial,
    const char *timestamp,
    double      perf_w_mean,
    double      duration_s,
    int         sample_count,
    const char *workload)
{
    char path[512], tmp[520];
    snprintf(path, sizeof(path), "%s/%s.probe",     state_dir, serial);
    snprintf(tmp,  sizeof(tmp),  "%s/%s.probe.tmp", state_dir, serial);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "probe: cannot write %s: %s\n", tmp, strerror(errno));
        return -1;
    }

    fprintf(f, "# Written by gpu_health_probe %s\n", PROBE_VERSION);
    fprintf(f, "serial=%s\n",            serial);
    fprintf(f, "probe_timestamp=%s\n",   timestamp);
    fprintf(f, "probe_duration_s=%.3f\n", duration_s);
    fprintf(f, "perf_w_mean=%.6f\n",     perf_w_mean);
    fprintf(f, "workload=%s\n",          workload);
    fprintf(f, "sample_count=%d\n",      sample_count);
    fprintf(f, "probe_exit_code=0\n");

    fclose(f);

    if (rename(tmp, path) < 0) {
        fprintf(stderr, "probe: rename %s -> %s: %s\n", tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int write_baseline_file(
    const char *baseline_dir,
    const char *serial,
    const char *uuid,
    const char *driver_version,
    const char *timestamp,
    double      perf_w_mean,
    int         sample_count,
    const char *workload)
{
    char path[512], tmp[520];
    snprintf(path, sizeof(path), "%s/%s",     baseline_dir, serial);
    snprintf(tmp,  sizeof(tmp),  "%s/%s.tmp", baseline_dir, serial);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "probe: cannot write baseline %s: %s\n",
                tmp, strerror(errno));
        return -1;
    }

    fprintf(f, "# Written by gpu_health_probe %s --establish-baseline\n",
            PROBE_VERSION);
    fprintf(f, "serial=%s\n",          serial);
    if (uuid[0])           fprintf(f, "uuid=%s\n",           uuid);
    if (driver_version[0]) fprintf(f, "driver_version=%s\n", driver_version);
    fprintf(f, "perf_w_mean=%.6f\n",   perf_w_mean);
    fprintf(f, "established_at=%s\n",  timestamp);
    fprintf(f, "workload=%s\n",        workload);
    fprintf(f, "sample_count=%d\n",    sample_count);

    fclose(f);

    if (rename(tmp, path) < 0) {
        fprintf(stderr, "probe: rename %s -> %s: %s\n", tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Usage
 * -------------------------------------------------------------------------- */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Runs a sustained BF16 GEMM workload, samples power at 1-second\n"
        "intervals, and writes a .probe file for gpu-health-exporter.\n"
        "\n"
        "Options:\n"
        "  -i N               GPU index (default: 0)\n"
        "  -s PATH            State directory for .probe file\n"
        "                     (default: %s)\n"
        "  -b PATH            Baseline directory (default: %s)\n"
        "  -d N               Measurement duration in seconds (default: %d)\n"
        "  -w N               Warmup duration in seconds (default: %d)\n"
        "  -n N               Square matrix dimension (default: %d)\n"
        "  --establish-baseline  Also write long-lived baseline file to -b\n"
        "  --dry-run             Print results only, do not write files\n"
        "  -h, --help         Show this help\n"
        "\n"
        "Exit codes: 0=success, 1=CUDA/cuBLAS error, 2=NVML error, 3=I/O error\n",
        prog,
        DEFAULT_STATE_DIR, DEFAULT_BASELINE_DIR,
        DEFAULT_DURATION_S, DEFAULT_WARMUP_S, DEFAULT_MATRIX_N);
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int         gpu_index          = 0;
    const char *state_dir          = DEFAULT_STATE_DIR;
    const char *baseline_dir       = DEFAULT_BASELINE_DIR;
    int         duration_s         = DEFAULT_DURATION_S;
    int         warmup_s           = DEFAULT_WARMUP_S;
    int         matrix_n           = DEFAULT_MATRIX_N;
    int         establish_baseline = 0;
    int         dry_run            = 0;

    /* ---- Argument parsing ------------------------------------------------ */
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-i") && i+1 < argc) { gpu_index    = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-s") && i+1 < argc) { state_dir    = argv[++i]; }
        else if (!strcmp(argv[i], "-b") && i+1 < argc) { baseline_dir = argv[++i]; }
        else if (!strcmp(argv[i], "-d") && i+1 < argc) { duration_s   = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-w") && i+1 < argc) { warmup_s     = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-n") && i+1 < argc) { matrix_n     = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--establish-baseline")) { establish_baseline = 1; }
        else if (!strcmp(argv[i], "--dry-run"))            { dry_run = 1; }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]); return 0;
        }
        else { fprintf(stderr, "Unknown argument: %s\n", argv[i]); usage(argv[0]); return 1; }
    }

    if (duration_s < 10) {
        fprintf(stderr, "probe: duration must be >= 10s\n"); return 1;
    }
    if (matrix_n < 512 || matrix_n > 32768) {
        fprintf(stderr, "probe: matrix_n must be in [512, 32768]\n"); return 1;
    }
    if (establish_baseline && dry_run) {
        fprintf(stderr, "probe: --establish-baseline and --dry-run are mutually exclusive\n");
        return 1;
    }

    /* ---- NVML init ------------------------------------------------------- */
    NVML_CHECK(nvmlInit_v2());

    nvmlDevice_t dev;
    NVML_CHECK(nvmlDeviceGetHandleByIndex((unsigned int)gpu_index, &dev));

    char serial[64]  = {0};
    char uuid[128]   = {0};
    char driver[64]  = {0};
    char model[128]  = {0};

    nvmlReturn_t sr = nvmlDeviceGetSerial(dev, serial, sizeof(serial));
    if (sr != NVML_SUCCESS) {
        fprintf(stderr, "probe: nvmlDeviceGetSerial: %s\n", nvmlErrorString(sr));
        nvmlShutdown();
        return 2;
    }

    nvmlDeviceGetUUID(dev, uuid, sizeof(uuid));
    nvmlSystemGetDriverVersion(driver, sizeof(driver));
    nvmlDeviceGetName(dev, model, sizeof(model));

    printf("probe: GPU %d — %s  serial=%s  driver=%s\n",
           gpu_index, model, serial, driver);
    printf("probe: warmup=%ds  measurement=%ds  matrix=%dx%d BF16\n",
           warmup_s, duration_s, matrix_n, matrix_n);

    /* ---- CUDA / cuBLAS init --------------------------------------------- */
    CUDA_CHECK(cudaSetDevice(gpu_index));

    cublasHandle_t handle;
    CUBLAS_CHECK(cublasCreate(&handle));

    /* Allocate device matrices: A(BF16), B(BF16), C(FP32 accumulator) */
    size_t n     = (size_t)matrix_n;
    size_t bytes = n * n * sizeof(__nv_bfloat16);

    __nv_bfloat16 *d_A = NULL, *d_B = NULL;
    float         *d_C = NULL;

    CUDA_CHECK(cudaMalloc((void **)&d_A, bytes));
    CUDA_CHECK(cudaMalloc((void **)&d_B, bytes));
    CUDA_CHECK(cudaMalloc((void **)&d_C, n * n * sizeof(float)));

    /* Fill with a small nonzero constant — real data not needed */
    CUDA_CHECK(cudaMemset(d_A, 0x3C, bytes)); /* ~1.0 in BF16 */
    CUDA_CHECK(cudaMemset(d_B, 0x3C, bytes));
    CUDA_CHECK(cudaMemset(d_C, 0,    n * n * sizeof(float)));

    const float alpha = 1.0f, beta = 0.0f;
    const int   N     = matrix_n;

    /* Pre-flight: one GEMM to warm the CUDA context and driver */
    CUBLAS_CHECK(cublasGemmEx(
        handle,
        CUBLAS_OP_N, CUBLAS_OP_N,
        N, N, N,
        &alpha,
        d_A, CUDA_R_16BF, N,
        d_B, CUDA_R_16BF, N,
        &beta,
        d_C, CUDA_R_32F,  N,
        CUBLAS_COMPUTE_32F,
        CUBLAS_GEMM_DEFAULT_TENSOR_OP));
    CUDA_CHECK(cudaDeviceSynchronize());

    /* ---- Warmup phase ---------------------------------------------------- */
    printf("probe: warming up...\n");
    uint64_t warmup_end = now_ms() + (uint64_t)warmup_s * 1000u;
    while (now_ms() < warmup_end) {
        cublasGemmEx(
            handle,
            CUBLAS_OP_N, CUBLAS_OP_N,
            N, N, N,
            &alpha,
            d_A, CUDA_R_16BF, N,
            d_B, CUDA_R_16BF, N,
            &beta,
            d_C, CUDA_R_32F,  N,
            CUBLAS_COMPUTE_32F,
            CUBLAS_GEMM_DEFAULT_TENSOR_OP);
    }
    cudaDeviceSynchronize();
    printf("probe: warmup complete — entering measurement window\n");

    /* ---- Measurement phase ----------------------------------------------- */
    double   perf_w_samples[MAX_SAMPLES];
    int      n_samples      = 0;
    double   flops_per_gemm = 2.0 * (double)N * (double)N * (double)N;

    uint64_t meas_start     = now_ms();
    uint64_t meas_end       = meas_start + (uint64_t)duration_s * 1000u;
    uint64_t interval_start = meas_start;
    long     interval_iters = 0;

    while (now_ms() < meas_end) {
        cublasGemmEx(
            handle,
            CUBLAS_OP_N, CUBLAS_OP_N,
            N, N, N,
            &alpha,
            d_A, CUDA_R_16BF, N,
            d_B, CUDA_R_16BF, N,
            &beta,
            d_C, CUDA_R_32F,  N,
            CUBLAS_COMPUTE_32F,
            CUBLAS_GEMM_DEFAULT_TENSOR_OP);
        interval_iters++;

        uint64_t now = now_ms();
        if (now - interval_start >= 1000u || now >= meas_end) {
            cudaDeviceSynchronize();

            double elapsed_s = (double)(now - interval_start) / 1000.0;
            double tflops    = (flops_per_gemm * (double)interval_iters)
                               / 1e12 / elapsed_s;

            unsigned int power_mw = 0;
            nvmlDeviceGetPowerUsage(dev, &power_mw);
            double power_w = (double)power_mw / 1000.0;

            if (power_w >= MIN_POWER_W && n_samples < MAX_SAMPLES) {
                perf_w_samples[n_samples++] = tflops / power_w;
            }

            printf("probe: [%3d] %.2f TFLOPS  %.1f W  %.4f TFLOPS/W\n",
                   n_samples, tflops, power_w,
                   power_w > 0.0 ? tflops / power_w : 0.0);

            interval_start = now;
            interval_iters = 0;
        }
    }
    cudaDeviceSynchronize();

    if (n_samples < MIN_VALID_SAMPLES) {
        fprintf(stderr, "probe: too few valid samples (%d < %d) — aborting\n",
                n_samples, MIN_VALID_SAMPLES);
        cublasDestroy(handle);
        cudaFree(d_A); cudaFree(d_B); cudaFree(d_C);
        nvmlShutdown();
        return 1;
    }

    /* ---- Compute mean perf/W -------------------------------------------- */
    double sum = 0.0;
    for (int i = 0; i < n_samples; i++) sum += perf_w_samples[i];
    double perf_w_mean     = sum / (double)n_samples;
    double actual_duration = (double)(now_ms() - meas_start) / 1000.0;

    char workload[64];
    snprintf(workload, sizeof(workload), "bf16_gemm_%d_tflops_per_w", matrix_n);

    char timestamp[32];
    iso8601_now(timestamp, sizeof(timestamp));

    printf("\nprobe: ── result ──────────────────────────────────────────\n");
    printf("probe: perf_w_mean  = %.6f TFLOPS/W\n", perf_w_mean);
    printf("probe: samples      = %d\n", n_samples);
    printf("probe: duration     = %.1f s\n", actual_duration);
    printf("probe: workload     = %s\n", workload);
    printf("probe: serial       = %s\n", serial);
    printf("probe: ────────────────────────────────────────────────────\n");

    /* ---- Write output files ---------------------------------------------- */
    int rc = 0;

    if (!dry_run) {
        if (write_probe_file(state_dir, serial, timestamp,
                             perf_w_mean, actual_duration,
                             n_samples, workload) == 0) {
            printf("probe: wrote %s/%s.probe\n", state_dir, serial);
        } else {
            rc = 3;
            goto cleanup;
        }

        if (establish_baseline) {
            if (write_baseline_file(baseline_dir, serial, uuid, driver,
                                    timestamp, perf_w_mean,
                                    n_samples, workload) == 0) {
                printf("probe: wrote baseline %s/%s\n", baseline_dir, serial);
            } else {
                rc = 3;
                goto cleanup;
            }
        }
    } else {
        printf("probe: dry-run — no files written\n");
    }

cleanup:
    cublasDestroy(handle);
    cudaFree(d_A);
    cudaFree(d_B);
    cudaFree(d_C);
    nvmlShutdown();
    return rc;
}
