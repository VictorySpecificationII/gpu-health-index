#include <dlfcn.h>
#include <string.h>
#include <stdio.h>

#include "nvml.h"
#include "util.h"

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/*
 * Try to dlsym `primary`; if not found, try `fallback` (may be NULL).
 * Returns the symbol address or NULL.
 */
static void *sym(void *dl, const char *primary, const char *fallback)
{
    /* Clear any prior dlerror state */
    dlerror();

    void *p = dlsym(dl, primary);
    if (p)
        return p;

    if (fallback) {
        dlerror();
        p = dlsym(dl, fallback);
    }

    return p;
}

/* -------------------------------------------------------------------------
 * nvml_load
 * ------------------------------------------------------------------------- */

int nvml_load(nvml_vtable_t *vt, void **dl_handle)
{
    memset(vt, 0, sizeof(*vt));
    *dl_handle = NULL;

    /* Try versioned name first — standard on all modern Linux systems */
    void *dl = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        log_debug("nvml: libnvidia-ml.so.1 not found, trying libnvidia-ml.so");
        dl = dlopen("libnvidia-ml.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!dl) {
        log_error("nvml: failed to load NVML library: %s", dlerror());
        return -1;
    }

    log_debug("nvml: library loaded");

    /*
     * Required symbols — failure to find any of these is fatal.
     * We use versioned names where they exist (_v2 suffix), falling back to
     * unversioned for drivers that export only the legacy name.
     */
#define LOAD_REQ(member, primary, fallback)                                    \
    do {                                                                       \
        *(void **)(&vt->member) = sym(dl, primary, fallback);                  \
        if (!vt->member) {                                                     \
            log_error("nvml: required symbol '%s' not found in NVML library",  \
                      primary);                                                \
            dlclose(dl);                                                       \
            return -1;                                                         \
        }                                                                      \
    } while (0)

    LOAD_REQ(Init,                               "nvmlInit_v2",
                                                 "nvmlInit");
    LOAD_REQ(Shutdown,                           "nvmlShutdown",             NULL);
    LOAD_REQ(ErrorString,                        "nvmlErrorString",          NULL);
    LOAD_REQ(DeviceGetCount,                     "nvmlDeviceGetCount",       NULL);
    LOAD_REQ(DeviceGetHandleByIndex,             "nvmlDeviceGetHandleByIndex_v2",
                                                 "nvmlDeviceGetHandleByIndex");
    LOAD_REQ(DeviceGetSerial,                    "nvmlDeviceGetSerial",      NULL);
    LOAD_REQ(DeviceGetUUID,                      "nvmlDeviceGetUUID",        NULL);
    LOAD_REQ(DeviceGetName,                      "nvmlDeviceGetName",        NULL);
    LOAD_REQ(SystemGetDriverVersion,             "nvmlSystemGetDriverVersion", NULL);
    LOAD_REQ(DeviceGetMaxPcieLinkGeneration,     "nvmlDeviceGetMaxPcieLinkGeneration", NULL);
    LOAD_REQ(DeviceGetMaxPcieLinkWidth,          "nvmlDeviceGetMaxPcieLinkWidth", NULL);
    LOAD_REQ(DeviceGetTemperature,               "nvmlDeviceGetTemperature", NULL);
    LOAD_REQ(DeviceGetPowerUsage,                "nvmlDeviceGetPowerUsage",  NULL);
    LOAD_REQ(DeviceGetEnforcedPowerLimit,        "nvmlDeviceGetEnforcedPowerLimit", NULL);
    LOAD_REQ(DeviceGetTotalEccErrors,            "nvmlDeviceGetTotalEccErrors", NULL);
    LOAD_REQ(DeviceGetMemoryInfo,                "nvmlDeviceGetMemoryInfo_v2",
                                                 "nvmlDeviceGetMemoryInfo");
    LOAD_REQ(DeviceGetClockInfo,                 "nvmlDeviceGetClockInfo",   NULL);
    LOAD_REQ(DeviceGetUtilizationRates,          "nvmlDeviceGetUtilizationRates", NULL);
    LOAD_REQ(DeviceGetCurrentClocksThrottleReasons,
                                                 "nvmlDeviceGetCurrentClocksThrottleReasons", NULL);
    LOAD_REQ(DeviceGetPerformanceState,          "nvmlDeviceGetPerformanceState", NULL);
    LOAD_REQ(DeviceGetCurrPcieLinkGeneration,    "nvmlDeviceGetCurrPcieLinkGeneration", NULL);
    LOAD_REQ(DeviceGetCurrPcieLinkWidth,         "nvmlDeviceGetCurrPcieLinkWidth", NULL);

#undef LOAD_REQ

    /*
     * Optional symbols — absent on older drivers or non-applicable hardware.
     * Caller must check for NULL before invoking.
     */
#define LOAD_OPT(member, primary, fallback)                                    \
    do {                                                                       \
        *(void **)(&vt->member) = sym(dl, primary, fallback);                  \
        if (!vt->member)                                                       \
            log_debug("nvml: optional symbol '%s' unavailable", primary);     \
    } while (0)

    LOAD_OPT(DeviceGetRetiredPages,    "nvmlDeviceGetRetiredPages",     NULL);
    LOAD_OPT(DeviceGetRemappedRows,    "nvmlDeviceGetRemappedRows",     NULL);
    LOAD_OPT(DeviceGetFanSpeed,        "nvmlDeviceGetFanSpeed",         NULL);
    LOAD_OPT(DeviceGetPcieReplayCounter, "nvmlDeviceGetPcieReplayCounter", NULL);
    LOAD_OPT(DeviceGetViolationStatus, "nvmlDeviceGetViolationStatus",  NULL);

#undef LOAD_OPT

    *dl_handle = dl;
    log_info("nvml: all required symbols resolved");
    return 0;
}

/* -------------------------------------------------------------------------
 * nvml_unload
 * ------------------------------------------------------------------------- */

void nvml_unload(void *dl_handle)
{
    if (dl_handle)
        dlclose(dl_handle);
}
