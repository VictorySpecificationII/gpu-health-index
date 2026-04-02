#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>

#include "util.h"

/* -------------------------------------------------------------------------
 * Logging
 * ------------------------------------------------------------------------- */

static log_level_t g_log_level = LOG_INFO;

static const char *level_label[] = {
    [LOG_DEBUG] = "DEBUG",
    [LOG_INFO]  = "INFO ",
    [LOG_WARN]  = "WARN ",
    [LOG_ERROR] = "ERROR",
};

void log_init(void)
{
    const char *env = getenv("GPU_HEALTH_LOG_LEVEL");
    if (!env)
        return;
    if (strcasecmp(env, "debug") == 0)      g_log_level = LOG_DEBUG;
    else if (strcasecmp(env, "info")  == 0) g_log_level = LOG_INFO;
    else if (strcasecmp(env, "warn")  == 0) g_log_level = LOG_WARN;
    else if (strcasecmp(env, "error") == 0) g_log_level = LOG_ERROR;
    /* Unknown value: leave at current level (default INFO) */
}

void log_msg(log_level_t level, const char *fmt, ...)
{
    if (level < g_log_level)
        return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);

    char tsbuf[24];
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);

    fprintf(stderr, "[%s] %s ", level_label[level], tsbuf);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}

/* -------------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------------- */

uint64_t time_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

uint64_t time_now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec;
}

void time_iso8601(uint64_t epoch_s, char *buf, size_t len)
{
    time_t t = (time_t)epoch_s;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* -------------------------------------------------------------------------
 * String helpers
 * ------------------------------------------------------------------------- */

int safe_strncpy(char *dst, const char *src, size_t size)
{
    if (size == 0)
        return 0;
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
    return 0;
}

char *str_trim(char *s)
{
    if (!s)
        return s;

    /* Trim leading whitespace */
    while (isspace((unsigned char)*s))
        s++;

    if (*s == '\0')
        return s;

    /* Trim trailing whitespace */
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        end--;
    *(end + 1) = '\0';

    return s;
}

int parse_uint64(const char *s, uint64_t min, uint64_t max, uint64_t *out)
{
    if (!s || *s == '\0')
        return -1;

    char *end;
    errno = 0;
    unsigned long long val = strtoull(s, &end, 10);

    if (errno != 0 || end == s || *end != '\0')
        return -1;
    if ((uint64_t)val < min || (uint64_t)val > max)
        return -1;

    *out = (uint64_t)val;
    return 0;
}

int parse_int(const char *s, int min, int max, int *out)
{
    if (!s || *s == '\0')
        return -1;

    char *end;
    errno = 0;
    long val = strtol(s, &end, 10);

    if (errno != 0 || end == s || *end != '\0')
        return -1;
    if (val < (long)min || val > (long)max)
        return -1;

    *out = (int)val;
    return 0;
}

int parse_double(const char *s, double min, double max, double *out)
{
    if (!s || *s == '\0')
        return -1;

    char *end;
    errno = 0;
    double val = strtod(s, &end);

    if (errno != 0 || end == s || *end != '\0')
        return -1;
    if (val < min || val > max)
        return -1;

    *out = val;
    return 0;
}
