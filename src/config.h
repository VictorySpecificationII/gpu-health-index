#ifndef GPU_HEALTH_CONFIG_H
#define GPU_HEALTH_CONFIG_H

#include "types.h"

/* -------------------------------------------------------------------------
 * Config loading
 *
 * Precedence (lowest to highest):
 *   1. Compiled-in defaults (CFG_DEFAULT_* in types.h)
 *   2. Config file at `path`
 *   3. GPU_HEALTH_<KEY> environment variables
 *
 * path may be NULL — defaults and env vars only.
 *
 * Returns 0 on success.
 * Returns -1 on invalid value (range violation, bad format) — logs reason.
 * Unknown config keys are logged as warnings and ignored (forward compat).
 * ------------------------------------------------------------------------- */
int config_load(const char *path, gpu_config_t *cfg);

/* -------------------------------------------------------------------------
 * Cross-field consistency validation
 *
 * Checks relationships between fields that config_load() cannot enforce
 * individually (e.g. warn_c < bad_c, probe_ttl >= probe_interval).
 *
 * Call after config_load() in main before proceeding with startup.
 *
 * Returns 0 if valid. Returns -1 and logs reason on first violation.
 * ------------------------------------------------------------------------- */
int config_validate(const gpu_config_t *cfg);

#endif /* GPU_HEALTH_CONFIG_H */
