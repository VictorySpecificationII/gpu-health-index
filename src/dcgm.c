#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

#include "dcgm.h"
#include "util.h"

/* -------------------------------------------------------------------------
 * Field subscription list
 *
 * All fields watched in dcgm_setup and queried in dcgm_poll.
 * Order here determines index in the values[] array returned by
 * GetLatestValues — dcgm_poll switches on POLL_FIELDS[i], not on the
 * fieldId echoed back in the result struct.
 * ------------------------------------------------------------------------- */

static const unsigned short POLL_FIELDS[DCGM_NUM_POLL_FIELDS] = {
    DCGM_FI_DEV_POWER_USAGE,
    DCGM_FI_DEV_TOTAL_ENERGY_CONSUMPTION,
    DCGM_FI_DEV_MEM_COPY_UTIL,
    DCGM_FI_DEV_POWER_VIOLATION,
    DCGM_FI_DEV_THERMAL_VIOLATION,
    DCGM_FI_DEV_NVLINK_CRC_FLIT_ERROR_COUNT_TOTAL,
    DCGM_FI_DEV_NVLINK_REPLAY_ERROR_COUNT_TOTAL,
    DCGM_FI_DEV_NVLINK_RECOVERY_ERROR_COUNT_TOTAL,
    DCGM_FI_DEV_XID_ERRORS,
    DCGM_FI_DEV_LAST_XID,
    DCGM_FI_DEV_PCIE_REPLAY_COUNTER,
    DCGM_FI_DEV_ROW_REMAP_FAILURE,
};

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static void *sym(void *dl, const char *primary, const char *fallback)
{
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
 * dcgm_load
 * ------------------------------------------------------------------------- */

int dcgm_load(dcgm_vtable_t *vt, void **dl_handle)
{
    memset(vt, 0, sizeof(*vt));
    *dl_handle = NULL;

    /* Try versioned name first; graceful failure is expected when DCGM is
     * not installed — log at INFO not ERROR. */
    void *dl = dlopen("libdcgm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        log_debug("dcgm: libdcgm.so.1 not found, trying libdcgm.so");
        dl = dlopen("libdcgm.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!dl) {
        log_info("dcgm: library not available — DCGM metrics will be absent (%s)",
                 dlerror());
        return -1;
    }

    log_debug("dcgm: library loaded");

#define LOAD_REQ(member, primary, fallback)                                    \
    do {                                                                       \
        *(void **)(&vt->member) = sym(dl, primary, fallback);                  \
        if (!vt->member) {                                                     \
            log_error("dcgm: required symbol '%s' not found", primary);        \
            dlclose(dl);                                                       \
            return -1;                                                         \
        }                                                                      \
    } while (0)

    LOAD_REQ(Init,             "dcgmInit",                                NULL);
    LOAD_REQ(Shutdown,         "dcgmShutdown",                            NULL);
    LOAD_REQ(Connect,          "dcgmConnect",                             NULL);
    LOAD_REQ(Disconnect,       "dcgmDisconnect",                          NULL);
    LOAD_REQ(GroupCreate,      "dcgmGroupCreate",                         NULL);
    LOAD_REQ(GroupAddDevice,   "dcgmGroupAddDevice",                      NULL);
    LOAD_REQ(FieldGroupCreate, "dcgmFieldGroupCreate",                    NULL);
    LOAD_REQ(WatchFields,      "dcgmWatchFields",                         NULL);
    LOAD_REQ(GetLatestValues,  "dcgmGetLatestValues_v2",
                               "dcgmGetLatestValues");
    LOAD_REQ(ErrorString,      "dcgmErrorString",                         NULL);

#undef LOAD_REQ

    *dl_handle = dl;
    log_info("dcgm: all required symbols resolved");
    return 0;
}

/* -------------------------------------------------------------------------
 * dcgm_unload
 * ------------------------------------------------------------------------- */

void dcgm_unload(void *dl_handle)
{
    if (dl_handle)
        dlclose(dl_handle);
}

/* -------------------------------------------------------------------------
 * dcgm_setup
 * ------------------------------------------------------------------------- */

int dcgm_setup(dcgm_vtable_t *vt, long *handle,
               unsigned int *gpu_ids, int num_gpus)
{
    int ret;

    if ((ret = vt->Init()) != DCGM_ST_OK) {
        log_error("dcgm: dcgmInit failed: %s", vt->ErrorString(ret));
        return -1;
    }

    /* NULL address → connect to localhost DCGM daemon on default port */
    if ((ret = vt->Connect(NULL, handle)) != DCGM_ST_OK) {
        log_error("dcgm: dcgmConnect failed: %s", vt->ErrorString(ret));
        vt->Shutdown();
        return -1;
    }

    log_debug("dcgm: connected to daemon");

    /* Create an empty device group and populate it */
    long group_id = 0;
    if ((ret = vt->GroupCreate(*handle, DCGM_GROUP_EMPTY,
                               "gpu_health", &group_id)) != DCGM_ST_OK) {
        log_error("dcgm: GroupCreate failed: %s", vt->ErrorString(ret));
        goto fail_disconnect;
    }

    for (int i = 0; i < num_gpus; i++) {
        if ((ret = vt->GroupAddDevice(*handle, group_id,
                                      gpu_ids[i])) != DCGM_ST_OK) {
            log_error("dcgm: GroupAddDevice gpu=%u failed: %s",
                      gpu_ids[i], vt->ErrorString(ret));
            goto fail_disconnect;
        }
    }

    /* Create field group with all subscribed fields */
    long field_group_id = 0;
    if ((ret = vt->FieldGroupCreate(*handle, (unsigned short *)POLL_FIELDS,
                                    DCGM_NUM_POLL_FIELDS,
                                    "gpu_health_fields",
                                    &field_group_id)) != DCGM_ST_OK) {
        log_error("dcgm: FieldGroupCreate failed: %s", vt->ErrorString(ret));
        goto fail_disconnect;
    }

    /* Watch at 1 Hz; retain 5 minutes of history; keep 2 samples in ring */
    if ((ret = vt->WatchFields(*handle, group_id, field_group_id,
                               1000000L /* µs */, 300.0 /* s */,
                               2)) != DCGM_ST_OK) {
        log_error("dcgm: WatchFields failed: %s", vt->ErrorString(ret));
        goto fail_disconnect;
    }

    log_info("dcgm: setup complete — %d GPU(s), %d fields watched",
             num_gpus, DCGM_NUM_POLL_FIELDS);
    return 0;

fail_disconnect:
    vt->Disconnect(*handle);
    vt->Shutdown();
    return -1;
}

/* -------------------------------------------------------------------------
 * dcgm_poll
 * ------------------------------------------------------------------------- */

int dcgm_poll(dcgm_vtable_t *vt, long handle, int gpu_id, dcgm_fields_t *out)
{
    /* Pre-fill all fields with unavailable sentinels */
    out->power_w              = DCGM_FIELD_UNAVAILABLE_DBL;
    out->energy_j             = DCGM_FIELD_UNAVAILABLE_DBL;
    out->mem_bw_util_pct      = DCGM_FIELD_UNAVAILABLE_DBL;
    out->power_violation_us   = DCGM_FIELD_UNAVAILABLE_U64;
    out->thermal_violation_us = DCGM_FIELD_UNAVAILABLE_U64;
    out->nvlink_replay        = DCGM_FIELD_UNAVAILABLE_U64;
    out->nvlink_recovery      = DCGM_FIELD_UNAVAILABLE_U64;
    out->nvlink_crc           = DCGM_FIELD_UNAVAILABLE_U64;
    out->xid_count            = DCGM_FIELD_UNAVAILABLE_U64;
    out->xid_last_code        = DCGM_FIELD_UNAVAILABLE_U32;
    out->pcie_replay          = DCGM_FIELD_UNAVAILABLE_U64;
    out->row_remap_failures   = DCGM_FIELD_UNAVAILABLE_U32;

    dcgm_field_value_t values[DCGM_NUM_POLL_FIELDS];
    memset(values, 0, sizeof(values));

    int ret = vt->GetLatestValues(handle, gpu_id,
                                  (unsigned short *)POLL_FIELDS,
                                  DCGM_NUM_POLL_FIELDS, values);
    if (ret != DCGM_ST_OK) {
        log_error("dcgm: GetLatestValues gpu=%d failed: %s",
                  gpu_id, vt->ErrorString(ret));
        return -1;
    }

    for (int i = 0; i < DCGM_NUM_POLL_FIELDS; i++) {
        /* Field-level error — skip, leave sentinel */
        if (values[i].status != DCGM_ST_OK)
            continue;

        /*
         * Switch on the field ID from our own subscription array rather
         * than on values[i].fieldId — GetLatestValues preserves order so
         * POLL_FIELDS[i] is authoritative.
         */
        switch (POLL_FIELDS[i]) {

        case DCGM_FI_DEV_POWER_USAGE:
            /* double field, W */
            if (values[i].value.dbl < DCGM_FP64_BLANK)
                out->power_w = values[i].value.dbl;
            break;

        case DCGM_FI_DEV_TOTAL_ENERGY_CONSUMPTION:
            /* int64 field, mJ — convert to J */
            if (values[i].value.i64 < DCGM_INT64_BLANK)
                out->energy_j = (double)values[i].value.i64 / 1000.0;
            break;

        case DCGM_FI_DEV_MEM_COPY_UTIL:
            /* int64 field, percent as integer */
            if (values[i].value.i64 < DCGM_INT64_BLANK)
                out->mem_bw_util_pct = (double)values[i].value.i64;
            break;

        case DCGM_FI_DEV_POWER_VIOLATION:
            if (values[i].value.i64 < DCGM_INT64_BLANK)
                out->power_violation_us = (uint64_t)values[i].value.i64;
            break;

        case DCGM_FI_DEV_THERMAL_VIOLATION:
            if (values[i].value.i64 < DCGM_INT64_BLANK)
                out->thermal_violation_us = (uint64_t)values[i].value.i64;
            break;

        case DCGM_FI_DEV_NVLINK_CRC_FLIT_ERROR_COUNT_TOTAL:
            if (values[i].value.i64 < DCGM_INT64_BLANK)
                out->nvlink_crc = (uint64_t)values[i].value.i64;
            break;

        case DCGM_FI_DEV_NVLINK_REPLAY_ERROR_COUNT_TOTAL:
            if (values[i].value.i64 < DCGM_INT64_BLANK)
                out->nvlink_replay = (uint64_t)values[i].value.i64;
            break;

        case DCGM_FI_DEV_NVLINK_RECOVERY_ERROR_COUNT_TOTAL:
            if (values[i].value.i64 < DCGM_INT64_BLANK)
                out->nvlink_recovery = (uint64_t)values[i].value.i64;
            break;

        case DCGM_FI_DEV_XID_ERRORS:
            if (values[i].value.i64 < DCGM_INT64_BLANK)
                out->xid_count = (uint64_t)values[i].value.i64;
            break;

        case DCGM_FI_DEV_LAST_XID: {
            int64_t v = values[i].value.i64;
            if (v >= 0 && v < DCGM_INT64_BLANK)
                out->xid_last_code = (v <= (int64_t)UINT32_MAX)
                                     ? (uint32_t)v
                                     : DCGM_FIELD_UNAVAILABLE_U32;
            break;
        }

        case DCGM_FI_DEV_PCIE_REPLAY_COUNTER:
            if (values[i].value.i64 < DCGM_INT64_BLANK)
                out->pcie_replay = (uint64_t)values[i].value.i64;
            break;

        case DCGM_FI_DEV_ROW_REMAP_FAILURE: {
            int64_t v = values[i].value.i64;
            if (v >= 0 && v < DCGM_INT64_BLANK)
                out->row_remap_failures = (v <= (int64_t)UINT32_MAX)
                                          ? (uint32_t)v
                                          : DCGM_FIELD_UNAVAILABLE_U32;
            break;
        }

        default:
            break;
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * dcgm_teardown
 * ------------------------------------------------------------------------- */

void dcgm_teardown(dcgm_vtable_t *vt, long handle)
{
    /* Groups and field groups are cleaned up automatically on disconnect */
    vt->Disconnect(handle);
    vt->Shutdown();
    log_info("dcgm: disconnected");
}
