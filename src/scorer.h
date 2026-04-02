#ifndef GPU_HEALTH_SCORER_H
#define GPU_HEALTH_SCORER_H

#include <stddef.h>
#include "types.h"

/* -------------------------------------------------------------------------
 * Scoring
 *
 * score_gpu() is a pure function: no I/O, no globals, no mutex.
 * The caller must hold ring_mutex and state_mutex before calling.
 *
 * Two input paths (see DESIGN §4 and SPEC §6.2.6):
 *
 *   Ring buffer path  — statistical computation over the 300s window:
 *     power saturation fraction, temp p95, HBM temp p95, SM clock std dev,
 *     ECC SBE rate, ECC DBE delta.
 *
 *   Current state path — threshold check on latest value:
 *     retired pages, row remap failures, pending remap, PCIe link
 *     degradation, XID delta, throttle reasons.
 *
 * Telemetry completeness gate: if the ring buffer does not have enough
 * samples, or sample spacing is too irregular, score returns GPU_CLASS_NA
 * and sets reason_mask |= GPU_REASON_TELEMETRY_INCOMPLETE.
 * ------------------------------------------------------------------------- */

/* Compute health score. Returns 0 on success (out populated).
 * baseline and probe may be NULL — scoring proceeds without perf/W component.
 * cfg must not be NULL. */
int score_gpu(const gpu_ring_t        *ring,
              const gpu_state_t       *state,
              const gpu_baseline_t    *baseline,
              const gpu_probe_result_t *probe,
              const gpu_config_t      *cfg,
              gpu_score_result_t      *out);

/* -------------------------------------------------------------------------
 * Ring buffer statistics — exposed so tests can validate each stat
 * independently without going through the full scoring path.
 *
 * field_offset: offsetof(gpu_sample_t, field_name) for the double field.
 * All return NaN if ring->count == 0.
 * ------------------------------------------------------------------------- */

double ring_p95(const gpu_ring_t *ring, size_t field_offset);
double ring_stddev(const gpu_ring_t *ring, size_t field_offset);
double ring_mean(const gpu_ring_t *ring, size_t field_offset);

#endif /* GPU_HEALTH_SCORER_H */
