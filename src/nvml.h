#ifndef GPU_HEALTH_NVML_H
#define GPU_HEALTH_NVML_H

/*
 * Self-contained NVML interface — does NOT include NVIDIA's nvml.h.
 *
 * All types, constants, and struct layouts are mirrored from the stable
 * NVML ABI. The library is loaded at runtime via dlopen; there is no
 * hard link dependency on libnvidia-ml.so.
 *
 * Usage:
 *   void *dl;
 *   nvml_vtable_t vt;
 *   if (nvml_load(&vt, &dl) != 0) abort();
 *   vt.Init();
 *   ...
 *   vt.Shutdown();
 *   nvml_unload(dl);
 */

/* =========================================================================
 * Mirrored struct types — layout must match NVML ABI exactly.
 * These are stable across all NVML versions targeting Ampere and later.
 * ========================================================================= */

typedef struct {
    unsigned long long total;   /* bytes */
    unsigned long long free;
    unsigned long long used;
} nvml_memory_t;

typedef struct {
    unsigned int gpu;           /* percent */
    unsigned int memory;        /* percent */
} nvml_utilization_t;

typedef struct {
    unsigned long long referenceTime;   /* µs since some epoch (monotonic) */
    unsigned long long violationTime;   /* cumulative µs throttled          */
} nvml_violation_time_t;

typedef enum
{
    NVML_VALUE_TYPE_DOUBLE = 0,
    NVML_VALUE_TYPE_UNSIGNED_INT = 1,
    NVML_VALUE_TYPE_UNSIGNED_LONG = 2,
    NVML_VALUE_TYPE_UNSIGNED_LONG_LONG = 3,
    NVML_VALUE_TYPE_SIGNED_LONG_LONG = 4,
    NVML_VALUE_TYPE_SIGNED_INT = 5,
    NVML_VALUE_TYPE_UNSIGNED_SHORT = 6,

    /* Keep this last */
    NVML_VALUE_TYPE_COUNT
} nvml_value_type_t;

typedef union
{
    double dVal;
    int siVal;
    unsigned int uiVal;
    unsigned long ulVal;
    unsigned long long ullVal;
    signed long long sllVal;
    unsigned short usVal;
} nvml_value_t;

typedef struct {
    unsigned int fieldId;
    unsigned int scopeId;
    long long timestamp;
    long long latencyUsec;
    nvml_value_type_t valueType;
    int nvmlReturn;
    nvml_value_t value;
} nvml_field_value_t;

/* =========================================================================
 * Constants
 *
 * Defined here so the collector does not need NVIDIA's nvml.h.
 * ========================================================================= */

/* Return codes */
#define NVML_SUCCESS                     0
#define NVML_ERROR_UNINITIALIZED         1
#define NVML_ERROR_INVALID_ARGUMENT      2
#define NVML_ERROR_NOT_SUPPORTED         3
#define NVML_ERROR_NO_PERMISSION         4
#define NVML_ERROR_ALREADY_INITIALIZED   5
#define NVML_ERROR_NOT_FOUND             6
#define NVML_ERROR_INSUFFICIENT_SIZE     7
#define NVML_ERROR_TIMEOUT               9    /* also DRIVER_NOT_LOADED on some versions */
#define NVML_ERROR_DRIVER_NOT_LOADED     9
#define NVML_ERROR_FUNCTION_NOT_FOUND    13
#define NVML_ERROR_GPU_IS_LOST           15
#define NVML_ERROR_RESET_REQUIRED        16
#define NVML_ERROR_OPERATING_SYSTEM      17
#define NVML_ERROR_UNKNOWN               999

/* Temperature sensors */
#define NVML_TEMPERATURE_GPU             0

/* Field IDs */
#define NVML_FI_DEV_MEMORY_TEMP          82

/* Clock types */
#define NVML_CLOCK_GRAPHICS              0
#define NVML_CLOCK_SM                    1
#define NVML_CLOCK_MEM                   2
#define NVML_CLOCK_VIDEO                 3

/* ECC error types */
#define NVML_MEMORY_ERROR_TYPE_CORRECTED   0  /* SBE */
#define NVML_MEMORY_ERROR_TYPE_UNCORRECTED 1  /* DBE */
#define NVML_VOLATILE_ECC                  0
#define NVML_AGGREGATE_ECC                 1

/* Retired page causes */
#define NVML_PAGE_RETIREMENT_CAUSE_MULTIPLE_SINGLE_BIT_ECC_ERRORS 0
#define NVML_PAGE_RETIREMENT_CAUSE_DOUBLE_BIT_ECC_ERRORS           1

/* Performance policy types (for violation status) */
#define NVML_PERF_POLICY_POWER           0
#define NVML_PERF_POLICY_THERMAL         4

/* Throttle reason bitmasks (nvmlClocksThrottleReasons) */
#define NVML_THROTTLE_REASON_SW_POWER_CAP   0x0000000000000004ULL
#define NVML_THROTTLE_REASON_HW_SLOWDOWN    0x0000000000000008ULL
#define NVML_THROTTLE_REASON_SW_THERMAL     0x0000000000000020ULL
#define NVML_THROTTLE_REASON_HW_THERMAL     0x0000000000000040ULL
#define NVML_THROTTLE_REASON_HW_POWER_BRAKE 0x0000000000000080ULL

/* "Hard error" return codes — trigger immediate GPU unavailable marking */
#define NVML_IS_HARD_ERROR(ret) \
    ((ret) == NVML_ERROR_GPU_IS_LOST || (ret) == NVML_ERROR_DRIVER_NOT_LOADED)

/* =========================================================================
 * Function pointer vtable
 *
 * All device handles are void* — they hold nvmlDevice_t internally (itself
 * an opaque pointer). Cast is identity on any platform we target.
 *
 * Populated by nvml_load(). NULL entries indicate optional functions that
 * were not found in this driver version. Callers must check before calling.
 * ========================================================================= */

typedef struct {
    /* Library lifecycle */
    int          (*Init)(void);               /* nvmlInit_v2 */
    int          (*Shutdown)(void);           /* nvmlShutdown */
    const char * (*ErrorString)(int result);  /* nvmlErrorString */

    /* Device enumeration */
    int (*DeviceGetCount)(unsigned int *count);
    int (*DeviceGetHandleByIndex)(unsigned int index, void **device);

    /* Identity — called once at startup */
    int (*DeviceGetSerial)(void *dev, char *serial, unsigned int len);
    int (*DeviceGetUUID)(void *dev, char *uuid, unsigned int len);
    int (*DeviceGetName)(void *dev, char *name, unsigned int len);
    int (*SystemGetDriverVersion)(char *version, unsigned int len);

    /* Startup — static properties (stored in gpu_ctx_t, not re-polled) */
    int (*DeviceGetMaxPcieLinkGeneration)(void *dev, unsigned int *gen);
    int (*DeviceGetMaxPcieLinkWidth)(void *dev, unsigned int *width);

    /* Per-poll: thermal */
    int (*DeviceGetTemperature)(void *dev, int sensor, unsigned int *temp);
    int (*DeviceGetFieldValues)(void *dev, int valuesCount, nvml_field_value_t *values);

    /* Per-poll: power */
    int (*DeviceGetPowerUsage)(void *dev, unsigned int *milliwatts);
    int (*DeviceGetEnforcedPowerLimit)(void *dev, unsigned int *milliwatts);

    /* Per-poll: ECC */
    int (*DeviceGetTotalEccErrors)(void *dev, int error_type, int counter_type,
                                   unsigned long long *count);
    int (*DeviceGetRetiredPages)(void *dev, int cause,     /* OPTIONAL */
                                 unsigned int *page_count,
                                 unsigned long long *addresses);
    int (*DeviceGetRemappedRows)(void *dev,                /* OPTIONAL — Ampere+ */
                                 unsigned int *corr_rows,
                                 unsigned int *uncorr_rows,
                                 unsigned int *is_pending,
                                 unsigned int *failure_occurred);

    /* Per-poll: memory capacity */
    int (*DeviceGetMemoryInfo)(void *dev, nvml_memory_t *mem);

    /* Per-poll: clocks */
    int (*DeviceGetClockInfo)(void *dev, int type, unsigned int *mhz);

    /* Per-poll: utilization */
    int (*DeviceGetUtilizationRates)(void *dev, nvml_utilization_t *util);

    /* Per-poll: throttle reasons */
    int (*DeviceGetCurrentClocksThrottleReasons)(void *dev,
                                                 unsigned long long *reasons);

    /* Per-poll: performance state */
    int (*DeviceGetPerformanceState)(void *dev, int *pstate);

    /* Per-poll: fan (OPTIONAL — absent on liquid-cooled SXM) */
    int (*DeviceGetFanSpeed)(void *dev, unsigned int *speed_pct);

    /* Per-poll: PCIe current link state */
    int (*DeviceGetCurrPcieLinkGeneration)(void *dev, unsigned int *gen);
    int (*DeviceGetCurrPcieLinkWidth)(void *dev, unsigned int *width);
    int (*DeviceGetPcieReplayCounter)(void *dev, unsigned int *value); /* OPTIONAL */

    /* Per-poll: violation times (OPTIONAL — DCGM preferred) */
    int (*DeviceGetViolationStatus)(void *dev, int policy,
                                    nvml_violation_time_t *vt);
} nvml_vtable_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/*
 * Load libnvidia-ml.so.1 (falls back to libnvidia-ml.so) and populate vt.
 * Required symbols missing → returns -1 and logs the missing symbol.
 * Optional symbols missing → logged at DEBUG level, entry left NULL.
 *
 * On success: *dl_handle is set; caller must pass it to nvml_unload() later.
 * Returns 0 on success, -1 on failure.
 */
int  nvml_load(nvml_vtable_t *vt, void **dl_handle);

/*
 * dlclose the handle obtained from nvml_load.
 * Safe to call with NULL.
 */
void nvml_unload(void *dl_handle);

#endif /* GPU_HEALTH_NVML_H */
