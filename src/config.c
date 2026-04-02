#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>

#include "config.h"
#include "util.h"

/* -------------------------------------------------------------------------
 * Config entry table
 *
 * One row per config key. Drives parsing, env override, and range validation.
 * The parser iterates this table for both file keys and env var lookups —
 * no switch statement, no duplicated validation logic.
 * ------------------------------------------------------------------------- */

typedef enum {
    CFG_INT,
    CFG_DBL,
    CFG_STR,
} cfg_type_t;

typedef struct {
    const char *key;       /* config file key and env var suffix (lowercase) */
    cfg_type_t  type;
    size_t      offset;    /* offsetof(gpu_config_t, field)                  */
    double      min;       /* INT/DBL: inclusive lower bound                 */
    double      max;       /* INT/DBL: inclusive upper bound                 */
    size_t      maxlen;    /* STR: field size in struct (includes NUL)       */
} cfg_entry_t;

/* Shorthand for offsetof */
#define OFF(field) offsetof(gpu_config_t, field)
/* Shorthand for field size */
#define SZ(field)  sizeof(((gpu_config_t *)0)->field)

static const cfg_entry_t cfg_table[] = {
    /* Paths */
    { "state_dir",                   CFG_STR, OFF(state_dir),                   0,      0,       SZ(state_dir)                   },
    { "baseline_dir",                CFG_STR, OFF(baseline_dir),                0,      0,       SZ(baseline_dir)                },
    { "listen_addr",                 CFG_STR, OFF(listen_addr),                 0,      0,       SZ(listen_addr)                 },
    { "listen_port",                 CFG_INT, OFF(listen_port),                 1,      65535,   0                               },

    /* Polling */
    { "poll_interval_s",             CFG_INT, OFF(poll_interval_s),             1,      60,      0                               },
    { "window_s",                    CFG_INT, OFF(window_s),                    60,     3600,    0                               },
    { "state_write_interval_s",      CFG_INT, OFF(state_write_interval_s),      1,      3600,    0                               },

    /* Telemetry completeness gate */
    { "min_sample_ratio",            CFG_DBL, OFF(min_sample_ratio),            0.0,    1.0,     0                               },
    { "max_median_step_s",           CFG_DBL, OFF(max_median_step_s),           0.1,    30.0,    0                               },
    { "min_samples_absolute",        CFG_INT, OFF(min_samples_absolute),        1,      1000,    0                               },

    /* Scoring — thermal */
    { "temp_p95_warn_c",             CFG_DBL, OFF(temp_p95_warn_c),             50.0,   120.0,   0                               },
    { "temp_p95_bad_c",              CFG_DBL, OFF(temp_p95_bad_c),              50.0,   120.0,   0                               },
    { "hbm_temp_p95_warn_c",         CFG_DBL, OFF(hbm_temp_p95_warn_c),         50.0,   120.0,   0                               },
    { "hbm_temp_p95_bad_c",          CFG_DBL, OFF(hbm_temp_p95_bad_c),          50.0,   120.0,   0                               },

    /* Scoring — clocks */
    { "clk_std_warn_mhz",            CFG_DBL, OFF(clk_std_warn_mhz),            1.0,    1000.0,  0                               },

    /* Scoring — power */
    { "power_high_ratio",            CFG_DBL, OFF(power_high_ratio),            0.5,    1.0,     0                               },
    { "power_penalty_max",           CFG_DBL, OFF(power_penalty_max),           0.0,    100.0,   0                               },

    /* Scoring — memory */
    { "ecc_sbe_rate_warn_per_hour",  CFG_DBL, OFF(ecc_sbe_rate_warn_per_hour),  0.0,    1e9,     0                               },
    { "ecc_sbe_penalty",             CFG_DBL, OFF(ecc_sbe_penalty),             0.0,    100.0,   0                               },
    { "ecc_dbe_penalty",             CFG_DBL, OFF(ecc_dbe_penalty),             0.0,    100.0,   0                               },
    { "retired_pages_warn",          CFG_INT, OFF(retired_pages_warn),          0,      10000,   0                               },
    { "retired_pages_bad",           CFG_INT, OFF(retired_pages_bad),           0,      10000,   0                               },
    { "retired_pages_pen_warn",      CFG_DBL, OFF(retired_pages_pen_warn),      0.0,    100.0,   0                               },
    { "retired_pages_pen_bad",       CFG_DBL, OFF(retired_pages_pen_bad),       0.0,    100.0,   0                               },
    { "row_remap_failure_penalty",   CFG_DBL, OFF(row_remap_failure_penalty),   0.0,    100.0,   0                               },

    /* Scoring — PCIe */
    { "pcie_link_degraded_penalty",  CFG_DBL, OFF(pcie_link_degraded_penalty),  0.0,    100.0,   0                               },
    { "pcie_width_degraded_penalty", CFG_DBL, OFF(pcie_width_degraded_penalty), 0.0,    100.0,   0                               },

    /* Scoring — perf/W drift */
    { "perf_drop_warn",              CFG_DBL, OFF(perf_drop_warn),              0.0,    1.0,     0                               },
    { "perf_drop_bad",               CFG_DBL, OFF(perf_drop_bad),               0.0,    1.0,     0                               },
    { "perf_drop_severe",            CFG_DBL, OFF(perf_drop_severe),            0.0,    1.0,     0                               },
    { "perf_drop_pen_warn",          CFG_DBL, OFF(perf_drop_pen_warn),          0.0,    100.0,   0                               },
    { "perf_drop_pen_bad",           CFG_DBL, OFF(perf_drop_pen_bad),           0.0,    100.0,   0                               },
    { "perf_drop_pen_severe",        CFG_DBL, OFF(perf_drop_pen_severe),        0.0,    100.0,   0                               },

    /* Probe */
    { "probe_interval_s",            CFG_INT, OFF(probe_interval_s),            60,     604800,  0                               },
    { "probe_ttl_s",                 CFG_INT, OFF(probe_ttl_s),                 60,     604800,  0                               },

    /* NVML */
    { "nvml_timeout_ms",             CFG_INT, OFF(nvml_timeout_ms),             100,    60000,   0                               },
    { "nvml_error_threshold",        CFG_INT, OFF(nvml_error_threshold),        1,      1000,    0                               },
    { "nvml_hard_error_threshold",   CFG_INT, OFF(nvml_hard_error_threshold),   1,      100,     0                               },
    { "nvml_retry_interval_s",       CFG_INT, OFF(nvml_retry_interval_s),       1,      3600,    0                               },

    /* DCGM */
    { "dcgm_timeout_ms",             CFG_INT, OFF(dcgm_timeout_ms),             100,    60000,   0                               },
    { "dcgm_error_threshold",        CFG_INT, OFF(dcgm_error_threshold),        1,      1000,    0                               },
    { "dcgm_retry_interval_s",       CFG_INT, OFF(dcgm_retry_interval_s),       1,      3600,    0                               },

    /* TLS */
    { "tls_cert_path",               CFG_STR, OFF(tls_cert_path),               0,      0,       SZ(tls_cert_path)               },
    { "tls_key_path",                CFG_STR, OFF(tls_key_path),                0,      0,       SZ(tls_key_path)                },
};

#undef OFF
#undef SZ

static const size_t cfg_table_len = sizeof(cfg_table) / sizeof(cfg_table[0]);

/* -------------------------------------------------------------------------
 * Defaults
 * ------------------------------------------------------------------------- */

static const gpu_config_t cfg_defaults = {
    .state_dir                   = CFG_DEFAULT_STATE_DIR,
    .baseline_dir                = CFG_DEFAULT_BASELINE_DIR,
    .listen_addr                 = CFG_DEFAULT_LISTEN_ADDR,
    .listen_port                 = CFG_DEFAULT_LISTEN_PORT,
    .poll_interval_s             = CFG_DEFAULT_POLL_INTERVAL_S,
    .window_s                    = CFG_DEFAULT_WINDOW_S,
    .state_write_interval_s      = CFG_DEFAULT_STATE_WRITE_INTERVAL_S,
    .min_sample_ratio            = CFG_DEFAULT_MIN_SAMPLE_RATIO,
    .max_median_step_s           = CFG_DEFAULT_MAX_MEDIAN_STEP_S,
    .min_samples_absolute        = CFG_DEFAULT_MIN_SAMPLES_ABSOLUTE,
    .temp_p95_warn_c             = CFG_DEFAULT_TEMP_P95_WARN_C,
    .temp_p95_bad_c              = CFG_DEFAULT_TEMP_P95_BAD_C,
    .hbm_temp_p95_warn_c         = CFG_DEFAULT_HBM_TEMP_P95_WARN_C,
    .hbm_temp_p95_bad_c          = CFG_DEFAULT_HBM_TEMP_P95_BAD_C,
    .clk_std_warn_mhz            = CFG_DEFAULT_CLK_STD_WARN_MHZ,
    .power_high_ratio            = CFG_DEFAULT_POWER_HIGH_RATIO,
    .power_penalty_max           = CFG_DEFAULT_POWER_PENALTY_MAX,
    .ecc_sbe_rate_warn_per_hour  = CFG_DEFAULT_ECC_SBE_RATE_WARN,
    .ecc_sbe_penalty             = CFG_DEFAULT_ECC_SBE_PENALTY,
    .ecc_dbe_penalty             = CFG_DEFAULT_ECC_DBE_PENALTY,
    .retired_pages_warn          = CFG_DEFAULT_RETIRED_PAGES_WARN,
    .retired_pages_bad           = CFG_DEFAULT_RETIRED_PAGES_BAD,
    .retired_pages_pen_warn      = CFG_DEFAULT_RETIRED_PEN_WARN,
    .retired_pages_pen_bad       = CFG_DEFAULT_RETIRED_PEN_BAD,
    .row_remap_failure_penalty   = CFG_DEFAULT_ROW_REMAP_PENALTY,
    .pcie_link_degraded_penalty  = CFG_DEFAULT_PCIE_GEN_PENALTY,
    .pcie_width_degraded_penalty = CFG_DEFAULT_PCIE_WIDTH_PENALTY,
    .perf_drop_warn              = CFG_DEFAULT_PERF_DROP_WARN,
    .perf_drop_bad               = CFG_DEFAULT_PERF_DROP_BAD,
    .perf_drop_severe            = CFG_DEFAULT_PERF_DROP_SEVERE,
    .perf_drop_pen_warn          = CFG_DEFAULT_PERF_DROP_PEN_WARN,
    .perf_drop_pen_bad           = CFG_DEFAULT_PERF_DROP_PEN_BAD,
    .perf_drop_pen_severe        = CFG_DEFAULT_PERF_DROP_PEN_SEVERE,
    .probe_interval_s            = CFG_DEFAULT_PROBE_INTERVAL_S,
    .probe_ttl_s                 = CFG_DEFAULT_PROBE_TTL_S,
    .nvml_timeout_ms             = CFG_DEFAULT_NVML_TIMEOUT_MS,
    .nvml_error_threshold        = CFG_DEFAULT_NVML_ERROR_THRESHOLD,
    .nvml_hard_error_threshold   = CFG_DEFAULT_NVML_HARD_THRESHOLD,
    .nvml_retry_interval_s       = CFG_DEFAULT_NVML_RETRY_INTERVAL_S,
    .dcgm_timeout_ms             = CFG_DEFAULT_DCGM_TIMEOUT_MS,
    .dcgm_error_threshold        = CFG_DEFAULT_DCGM_ERROR_THRESHOLD,
    .dcgm_retry_interval_s       = CFG_DEFAULT_DCGM_RETRY_INTERVAL_S,
    .tls_cert_path               = "",
    .tls_key_path                = "",
};

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* Look up a key in the config table. Returns entry or NULL. */
static const cfg_entry_t *find_entry(const char *key)
{
    for (size_t i = 0; i < cfg_table_len; i++) {
        if (strcmp(cfg_table[i].key, key) == 0)
            return &cfg_table[i];
    }
    return NULL;
}

/* Apply a validated string value to the config struct field described by e.
 * `src` is a human-readable origin string used in error log messages.
 * Returns 0 on success, -1 on range or parse failure. */
static int apply_entry(gpu_config_t *cfg, const cfg_entry_t *e,
                        const char *val, const char *src)
{
    char *base = (char *)cfg;

    switch (e->type) {
    case CFG_INT: {
        int v;
        if (parse_int(val, (int)e->min, (int)e->max, &v) != 0) {
            log_error("config: %s: invalid value '%s' (expected int in [%d, %d]) from %s",
                      e->key, val, (int)e->min, (int)e->max, src);
            return -1;
        }
        *(int *)(base + e->offset) = v;
        break;
    }
    case CFG_DBL: {
        double v;
        if (parse_double(val, e->min, e->max, &v) != 0) {
            log_error("config: %s: invalid value '%s' (expected double in [%g, %g]) from %s",
                      e->key, val, e->min, e->max, src);
            return -1;
        }
        *(double *)(base + e->offset) = v;
        break;
    }
    case CFG_STR:
        safe_strncpy(base + e->offset, val, e->maxlen);
        break;
    }

    return 0;
}

/* Build the env var name for a config key.
 * listen_port -> GPU_HEALTH_LISTEN_PORT
 * buf must be at least strlen("GPU_HEALTH_") + strlen(key) + 1 bytes. */
static void key_to_envvar(const char *key, char *buf, size_t buflen)
{
    static const char prefix[] = "GPU_HEALTH_";
    size_t plen = sizeof(prefix) - 1;

    if (buflen <= plen) {
        if (buflen > 0) buf[0] = '\0';
        return;
    }

    memcpy(buf, prefix, plen);
    size_t i = plen;

    for (const char *p = key; *p && i < buflen - 1; p++, i++)
        buf[i] = (char)toupper((unsigned char)*p);

    buf[i] = '\0';
}

/* -------------------------------------------------------------------------
 * config_load
 * ------------------------------------------------------------------------- */

int config_load(const char *path, gpu_config_t *cfg)
{
    /* Start from compiled-in defaults */
    *cfg = cfg_defaults;

    /* --- Parse config file ------------------------------------------------ */
    if (path != NULL) {
        FILE *f = fopen(path, "r");
        if (!f) {
            log_error("config: cannot open '%s'", path);
            return -1;
        }

        char line[1024];
        int  lineno = 0;

        while (fgets(line, sizeof(line), f)) {
            lineno++;

            char *p = str_trim(line);

            /* Skip blank lines and comments */
            if (*p == '\0' || *p == '#')
                continue;

            /* Split on first '=' */
            char *eq = strchr(p, '=');
            if (!eq) {
                log_warn("config: %s:%d: no '=' found, skipping line", path, lineno);
                continue;
            }

            *eq = '\0';
            char *key = str_trim(p);
            char *val = str_trim(eq + 1);

            const cfg_entry_t *e = find_entry(key);
            if (!e) {
                log_warn("config: %s:%d: unknown key '%s', ignoring", path, lineno, key);
                continue;
            }

            char src[64];
            snprintf(src, sizeof(src), "%s:%d", path, lineno);
            if (apply_entry(cfg, e, val, src) != 0) {
                fclose(f);
                return -1;
            }
        }

        fclose(f);
    }

    /* --- Apply GPU_HEALTH_* env var overrides ----------------------------- */
    char envname[128];

    for (size_t i = 0; i < cfg_table_len; i++) {
        key_to_envvar(cfg_table[i].key, envname, sizeof(envname));

        const char *val = getenv(envname);
        if (!val)
            continue;

        if (apply_entry(cfg, &cfg_table[i], val, envname) != 0)
            return -1;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * config_validate
 * ------------------------------------------------------------------------- */

int config_validate(const gpu_config_t *cfg)
{
    /* Thermal thresholds: warn must be strictly less than bad */
    if (cfg->temp_p95_warn_c >= cfg->temp_p95_bad_c) {
        log_error("config: temp_p95_warn_c (%.1f) must be < temp_p95_bad_c (%.1f)",
                  cfg->temp_p95_warn_c, cfg->temp_p95_bad_c);
        return -1;
    }
    if (cfg->hbm_temp_p95_warn_c >= cfg->hbm_temp_p95_bad_c) {
        log_error("config: hbm_temp_p95_warn_c (%.1f) must be < hbm_temp_p95_bad_c (%.1f)",
                  cfg->hbm_temp_p95_warn_c, cfg->hbm_temp_p95_bad_c);
        return -1;
    }

    /* Perf/W drop thresholds must be strictly increasing */
    if (cfg->perf_drop_warn >= cfg->perf_drop_bad) {
        log_error("config: perf_drop_warn (%.3f) must be < perf_drop_bad (%.3f)",
                  cfg->perf_drop_warn, cfg->perf_drop_bad);
        return -1;
    }
    if (cfg->perf_drop_bad >= cfg->perf_drop_severe) {
        log_error("config: perf_drop_bad (%.3f) must be < perf_drop_severe (%.3f)",
                  cfg->perf_drop_bad, cfg->perf_drop_severe);
        return -1;
    }

    /* Retired pages: warn <= bad */
    if (cfg->retired_pages_warn > cfg->retired_pages_bad) {
        log_error("config: retired_pages_warn (%d) must be <= retired_pages_bad (%d)",
                  cfg->retired_pages_warn, cfg->retired_pages_bad);
        return -1;
    }

    /* Probe TTL must be >= probe interval */
    if (cfg->probe_ttl_s < cfg->probe_interval_s) {
        log_error("config: probe_ttl_s (%d) must be >= probe_interval_s (%d)",
                  cfg->probe_ttl_s, cfg->probe_interval_s);
        return -1;
    }

    return 0;
}
