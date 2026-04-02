#ifndef GPU_HEALTH_STATE_H
#define GPU_HEALTH_STATE_H

#include "types.h"

/* -------------------------------------------------------------------------
 * Baseline loading
 *
 * Reads and validates the baseline file at {baseline_dir}/{serial}.
 *
 * File format: key=value, one per line, # comments. Required fields:
 *   serial, perf_w_mean, established_at, workload, sample_count.
 *   Optional: uuid, driver_version.
 *
 * Validation:
 *   serial mismatch  → out->serial_mismatch=1, out->valid=0 (hard fail)
 *   missing required → out->valid=0             (hard fail)
 *   file not found   → out->available=0         (soft — no baseline yet)
 *
 * Driver mismatch is the caller's responsibility: compare out->driver_version
 * against the GPU's actual driver version and set out->driver_mismatch.
 *
 * Always returns 0. Errors are reflected in out->available and out->valid.
 * ------------------------------------------------------------------------- */
int baseline_load(const char *baseline_dir, const char *serial,
                  gpu_baseline_t *out);

/* -------------------------------------------------------------------------
 * Probe state loading
 *
 * Reads the probe state file at {state_dir}/{serial}.probe.
 *
 * File format: same key=value as baseline. Required: serial, perf_w_mean,
 * probe_timestamp, probe_exit_code. Optional: uuid, driver_version,
 * workload, sample_count, probe_duration_s.
 *
 * TTL check: if (now - probe_timestamp) > probe_ttl_s, out->stale = 1.
 *   A stale probe is still available — the scorer excludes it from scoring
 *   but the http child emits gpu_probe_result_stale=1.
 *
 * Always returns 0. Errors reflected in out->available.
 * ------------------------------------------------------------------------- */
int probe_load(const char *state_dir, const char *serial,
               int probe_ttl_s, gpu_probe_result_t *out);

/* -------------------------------------------------------------------------
 * inotify-based baseline hot-reload
 *
 * baseline_inotify_init: set up a non-blocking inotify watch on baseline_dir.
 * Returns the inotify fd (>= 0) on success, -1 on error.
 * Caller must close the fd on shutdown.
 *
 * baseline_inotify_check: non-blocking drain of pending inotify events.
 * Returns 1 if any events were present (caller should reload all baselines),
 * 0 if no events, -1 on read error.
 * ------------------------------------------------------------------------- */
int baseline_inotify_init(const char *baseline_dir);
int baseline_inotify_check(int inotify_fd);

#endif /* GPU_HEALTH_STATE_H */
