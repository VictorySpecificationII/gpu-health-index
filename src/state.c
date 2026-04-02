#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/inotify.h>

#include "state.h"
#include "util.h"

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* Parse ISO 8601 UTC string "2026-03-15T14:22:00Z" into Unix epoch seconds.
 * Returns 0 on parse failure (0 is never a valid timestamp here). */
static uint64_t parse_iso8601(const char *s)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));

    const char *end = strptime(s, "%Y-%m-%dT%H:%M:%SZ", &tm);
    if (!end || *end != '\0')
        return 0;

    tm.tm_isdst = 0;
    time_t t = timegm(&tm);
    if (t == (time_t)-1)
        return 0;

    return (uint64_t)t;
}

/* Parse one key=value line into key and val buffers.
 * Trims whitespace from both sides of '='.
 * Returns 1 if a valid key=value pair was found, 0 otherwise. */
static int split_kv(char *line, char **key_out, char **val_out)
{
    char *p = str_trim(line);
    if (*p == '\0' || *p == '#')
        return 0;

    char *eq = strchr(p, '=');
    if (!eq)
        return 0;

    *eq = '\0';
    *key_out = str_trim(p);
    *val_out = str_trim(eq + 1);
    return 1;
}

/* -------------------------------------------------------------------------
 * baseline_load
 * ------------------------------------------------------------------------- */

int baseline_load(const char *baseline_dir, const char *serial,
                  gpu_baseline_t *out)
{
    memset(out, 0, sizeof(*out));

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", baseline_dir, serial);

    FILE *f = fopen(path, "r");
    if (!f) {
        /* Not an error — baseline may simply not exist yet */
        log_debug("state: baseline not found: %s", path);
        return 0;
    }

    out->available = 1;

    /* Track which required fields were seen */
    int has_serial = 0, has_perf_w = 0, has_established = 0,
        has_workload = 0, has_sample_count = 0;

    char line[512];
    int  parse_error = 0;

    while (fgets(line, sizeof(line), f) && !parse_error) {
        char *key, *val;
        if (!split_kv(line, &key, &val))
            continue;

        if (strcmp(key, "serial") == 0) {
            safe_strncpy(out->serial, val, sizeof(out->serial));
            has_serial = 1;

        } else if (strcmp(key, "uuid") == 0) {
            safe_strncpy(out->uuid, val, sizeof(out->uuid));

        } else if (strcmp(key, "driver_version") == 0) {
            safe_strncpy(out->driver_version, val, sizeof(out->driver_version));

        } else if (strcmp(key, "perf_w_mean") == 0) {
            if (parse_double(val, 0.0, 1e9, &out->perf_w_mean) != 0) {
                log_error("state: baseline %s: invalid perf_w_mean '%s'", path, val);
                parse_error = 1;
            } else {
                has_perf_w = 1;
            }

        } else if (strcmp(key, "established_at") == 0) {
            out->established_at_s = parse_iso8601(val);
            if (out->established_at_s == 0) {
                log_error("state: baseline %s: invalid established_at '%s'", path, val);
                parse_error = 1;
            } else {
                has_established = 1;
            }

        } else if (strcmp(key, "workload") == 0) {
            safe_strncpy(out->workload, val, sizeof(out->workload));
            has_workload = 1;

        } else if (strcmp(key, "sample_count") == 0) {
            if (parse_int(val, 1, 1000000, &out->sample_count) != 0) {
                log_error("state: baseline %s: invalid sample_count '%s'", path, val);
                parse_error = 1;
            } else {
                has_sample_count = 1;
            }
        }
        /* unknown keys: silently ignored for forward compatibility */
    }

    fclose(f);

    if (parse_error) {
        out->valid = 0;
        return 0;
    }

    /* All required fields must be present */
    if (!has_serial || !has_perf_w || !has_established
            || !has_workload || !has_sample_count) {
        log_error("state: baseline %s: missing required field(s) "
                  "(serial=%d perf_w=%d established=%d workload=%d samples=%d)",
                  path, has_serial, has_perf_w, has_established,
                  has_workload, has_sample_count);
        out->valid = 0;
        return 0;
    }

    /* Serial must match the filename / requested serial */
    if (strcmp(out->serial, serial) != 0) {
        log_error("state: baseline %s: serial mismatch (file='%s', device='%s')",
                  path, out->serial, serial);
        out->serial_mismatch = 1;
        out->valid = 0;
        return 0;
    }

    /* Sanity bound: perf_w_mean must be positive */
    if (out->perf_w_mean <= 0.0) {
        log_error("state: baseline %s: perf_w_mean must be > 0 (got %g)",
                  path, out->perf_w_mean);
        out->valid = 0;
        return 0;
    }

    out->valid = 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * probe_load
 * ------------------------------------------------------------------------- */

int probe_load(const char *state_dir, const char *serial,
               int probe_ttl_s, gpu_probe_result_t *out)
{
    memset(out, 0, sizeof(*out));

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.probe", state_dir, serial);

    FILE *f = fopen(path, "r");
    if (!f) {
        log_debug("state: probe state not found: %s", path);
        return 0;
    }

    out->available = 1;

    int has_serial = 0, has_perf_w = 0, has_timestamp = 0, has_exit_code = 0;
    int parse_error = 0;
    char line[512];

    while (fgets(line, sizeof(line), f) && !parse_error) {
        char *key, *val;
        if (!split_kv(line, &key, &val))
            continue;

        if (strcmp(key, "serial") == 0) {
            safe_strncpy(out->serial, val, sizeof(out->serial));
            has_serial = 1;

        } else if (strcmp(key, "perf_w_mean") == 0) {
            if (parse_double(val, 0.0, 1e9, &out->perf_w_mean) != 0) {
                log_error("state: probe %s: invalid perf_w_mean '%s'", path, val);
                parse_error = 1;
            } else {
                has_perf_w = 1;
            }

        } else if (strcmp(key, "probe_timestamp") == 0) {
            out->probe_timestamp_s = parse_iso8601(val);
            if (out->probe_timestamp_s == 0) {
                log_error("state: probe %s: invalid probe_timestamp '%s'", path, val);
                parse_error = 1;
            } else {
                has_timestamp = 1;
            }

        } else if (strcmp(key, "probe_exit_code") == 0) {
            if (parse_int(val, -1000, 1000, &out->probe_exit_code) != 0) {
                log_error("state: probe %s: invalid probe_exit_code '%s'", path, val);
                parse_error = 1;
            } else {
                has_exit_code = 1;
            }

        } else if (strcmp(key, "workload") == 0) {
            safe_strncpy(out->workload, val, sizeof(out->workload));

        } else if (strcmp(key, "sample_count") == 0) {
            int v;
            if (parse_int(val, 0, 1000000, &v) == 0)
                out->sample_count = v;

        } else if (strcmp(key, "probe_duration_s") == 0) {
            parse_double(val, 0.0, 86400.0, &out->probe_duration_s);
        }
        /* serial, uuid, driver_version: parsed but not stored (probe is ephemeral) */
    }

    fclose(f);

    if (parse_error) {
        out->available = 0;
        return 0;
    }

    if (!has_serial || !has_perf_w || !has_timestamp || !has_exit_code) {
        log_warn("state: probe %s: missing required field(s), ignoring", path);
        out->available = 0;
        return 0;
    }

    /* TTL check */
    uint64_t now = time_now_s();
    if (now > out->probe_timestamp_s
            && (now - out->probe_timestamp_s) > (uint64_t)probe_ttl_s) {
        log_warn("state: probe %s: result is stale (age=%lus, ttl=%ds)",
                 path,
                 (unsigned long)(now - out->probe_timestamp_s),
                 probe_ttl_s);
        out->stale = 1;
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * inotify
 * ------------------------------------------------------------------------- */

int baseline_inotify_init(const char *baseline_dir)
{
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        log_error("state: inotify_init1 failed: %s", strerror(errno));
        return -1;
    }

    /* Watch for files being written and atomically renamed into the directory */
    uint32_t mask = IN_CLOSE_WRITE | IN_MOVED_TO;
    if (inotify_add_watch(fd, baseline_dir, mask) < 0) {
        log_error("state: inotify_add_watch(%s) failed: %s",
                  baseline_dir, strerror(errno));
        close(fd);
        return -1;
    }

    log_info("state: inotify watching %s for baseline changes", baseline_dir);
    return fd;
}

int baseline_inotify_check(int inotify_fd)
{
    /* Buffer sized for several events; aligned per inotify(7) requirements */
    char buf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));

    int any = 0;
    ssize_t n;

    while ((n = read(inotify_fd, buf, sizeof(buf))) > 0)
        any = 1;

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return any;   /* no more events — normal for non-blocking fd */
        log_error("state: inotify read error: %s", strerror(errno));
        return -1;
    }

    return any;
}
