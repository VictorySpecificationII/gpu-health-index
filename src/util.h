#ifndef GPU_HEALTH_UTIL_H
#define GPU_HEALTH_UTIL_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * Logging
 *
 * All output goes to stderr (captured by journald and container runtimes).
 * Level hierarchy: DEBUG < INFO < WARN < ERROR.
 * Default level: INFO. Override with GPU_HEALTH_LOG_LEVEL=debug|info|warn|error.
 * ------------------------------------------------------------------------- */

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3,
} log_level_t;

/* Read GPU_HEALTH_LOG_LEVEL and set the active log level. Call once at startup. */
void log_init(void);

/* Emit a log line to stderr. Format: [LEVEL] 2026-04-02T14:22:00Z <message>\n */
void log_msg(log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define log_debug(...)  log_msg(LOG_DEBUG, __VA_ARGS__)
#define log_info(...)   log_msg(LOG_INFO,  __VA_ARGS__)
#define log_warn(...)   log_msg(LOG_WARN,  __VA_ARGS__)
#define log_error(...)  log_msg(LOG_ERROR, __VA_ARGS__)

/* -------------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------------- */

/* Monotonic milliseconds — for internal timeouts, retry intervals, watchdogs.
   Value has no relation to wall clock; only differences are meaningful. */
uint64_t time_now_ms(void);

/* Wall-clock seconds (CLOCK_REALTIME) — for TTL checks, state file timestamps,
   and the Prometheus last_poll_timestamp metric. */
uint64_t time_now_s(void);

/* Format epoch_s as ISO 8601 UTC into buf. buf must be >= 21 bytes.
   Writes "2026-04-02T14:22:00Z\0". Truncates silently if buf is too small. */
void time_iso8601(uint64_t epoch_s, char *buf, size_t len);

/* -------------------------------------------------------------------------
 * String helpers
 * ------------------------------------------------------------------------- */

/* Bounds-checked copy. Always NUL-terminates dst. Returns 0. */
int safe_strncpy(char *dst, const char *src, size_t size);

/* Strip leading and trailing ASCII whitespace in-place. Returns s. */
char *str_trim(char *s);

/* Parse unsigned 64-bit integer from decimal string s.
   Returns 0 and writes *out if s is a valid integer within [min, max].
   Returns -1 on empty string, non-numeric chars, overflow, or range violation. */
int parse_uint64(const char *s, uint64_t min, uint64_t max, uint64_t *out);

/* Parse int from decimal string s.
   Returns 0 and writes *out if value is within [min, max].
   Returns -1 on parse error or range violation. */
int parse_int(const char *s, int min, int max, int *out);

/* Parse double from decimal string s.
   Returns 0 and writes *out if value is within [min, max].
   Returns -1 on parse error or range violation. */
int parse_double(const char *s, double min, double max, double *out);

#endif /* GPU_HEALTH_UTIL_H */
