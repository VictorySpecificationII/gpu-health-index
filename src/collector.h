#ifndef GPU_HEALTH_COLLECTOR_H
#define GPU_HEALTH_COLLECTOR_H

/*
 * collector.h — per-GPU poll thread, ring buffer, and top-level context.
 *
 * Defines gpu_ctx_t (one per GPU) and exporter_t (one per process).
 * Both live in the parent process; the HTTP child never sees them.
 */

#include <pthread.h>
#include <stdint.h>

#include "types.h"
#include "nvml.h"
#include "dcgm.h"

/* =========================================================================
 * Per-GPU context
 *
 * One instance per GPU, allocated as an array in exporter_t.gpus.
 * All fields set at startup; poll thread is the sole writer thereafter
 * (with the exceptions noted per field).
 * ========================================================================= */

typedef struct {
    /* Identity — set once at startup, never modified */
    char                serial[32];
    char                uuid[48];
    char                gpu_model[64];
    char                driver_version[64];
    int                 gpu_index;
    gpu_identity_src_t  identity_source;
    int                 pcie_link_gen_max;    /* max capability, static */
    int                 pcie_link_width_max;  /* max capability, static */
    void               *nvml_handle;          /* nvmlDevice_t, opaque */

    /* Ring buffer — written by poll thread under ring_mutex */
    gpu_ring_t          ring;
    pthread_mutex_t     ring_mutex;

    /* Current point-in-time state — written by poll thread under state_mutex */
    gpu_state_t         state;
    pthread_mutex_t     state_mutex;

    /* Latest IPC snapshot — written by poll thread under snapshot_mutex;
       read by snapshot_send() to serialise into the socketpair */
    gpu_snapshot_t      snapshot;
    pthread_mutex_t     snapshot_mutex;

    /* Baseline and probe files — hot-reloaded via inotify, under files_mutex */
    gpu_baseline_t      baseline;
    gpu_probe_result_t  probe;
    pthread_mutex_t     files_mutex;

    /* Poll thread lifecycle — written by collector_start/stop */
    pthread_t           thread;
    volatile int        ready;   /* set to 1 after first successful poll cycle */
    volatile int        stop;    /* set to 1 to request graceful exit */

    /* Error tracking — written by poll thread only */
    volatile int        gpu_present;          /* 0 if GPU disappeared */
    volatile int        gpu_available;        /* 0 if above soft error threshold */
    int                 consecutive_errors;
    int                 consecutive_hard_errors;
    uint64_t            last_retry_ms;        /* wall-clock ms of last retry attempt */

    /* Self-health counters */
    uint64_t            collector_errors_total;
} gpu_ctx_t;

/* =========================================================================
 * Top-level exporter context
 *
 * One instance, stack-allocated in main(). Passed by pointer everywhere.
 * ========================================================================= */

typedef struct {
    gpu_config_t     cfg;
    nvml_vtable_t    nvml;
    dcgm_vtable_t    dcgm;
    int              dcgm_available;
    long             dcgm_handle;          /* connection handle from dcgm_setup */
    int              num_gpus;
    gpu_ctx_t       *gpus;                 /* heap array of num_gpus entries */

    /* Socketpair for IPC with HTTP child */
    int              parent_fd;            /* parent writes snapshots here */
    int              child_fd;             /* child reads from here */
    pid_t            child_pid;

    /* Baseline hot-reload (shared across all GPU poll threads) */
    int              baseline_inotify_fd;  /* -1 if inotify unavailable */

    volatile int     running;              /* cleared on SIGTERM/SIGINT */
} exporter_t;

/* =========================================================================
 * Ring buffer helper — exposed for unit testing
 * ========================================================================= */

/*
 * Push one sample into the ring buffer.  Wraps at capacity.
 * Caller must hold ctx->ring_mutex.
 */
void ring_push(gpu_ring_t *ring, const gpu_sample_t *s);

/* =========================================================================
 * Public API
 * ========================================================================= */

/*
 * Allocate the ring buffer, initialise all mutexes, and spawn the poll
 * thread for ctx.  exp is passed through to the thread for vtable and
 * socketpair access.
 * Returns 0 on success, -1 on failure (logged).
 */
int  collector_start(gpu_ctx_t *ctx, exporter_t *exp);

/*
 * Signal the poll thread to stop (sets ctx->stop = 1) and block until
 * the thread has joined.  Safe to call after a failed collector_start.
 */
void collector_stop(gpu_ctx_t *ctx);

#endif /* GPU_HEALTH_COLLECTOR_H */
