#ifndef GPU_HEALTH_DCGM_H
#define GPU_HEALTH_DCGM_H

/*
 * Self-contained DCGM interface — does NOT include NVIDIA's dcgm_agent.h.
 *
 * All types, constants, and struct layouts are mirrored from the stable
 * DCGM ABI. The library is loaded at runtime via dlopen; there is no
 * hard link dependency on libdcgm.so.
 *
 * Usage:
 *   void *dl;
 *   dcgm_vtable_t vt;
 *   long handle;
 *   if (dcgm_load(&vt, &dl) != 0) { ... }   // dcgm_available = 0
 *   if (dcgm_setup(&vt, &handle, gpu_ids, num_gpus) != 0) { ... }  // dcgm_available = 0
 *   ...per-poll: dcgm_poll(&vt, handle, gpu_id, &fields);
 *   dcgm_teardown(&vt, handle);
 *   dcgm_unload(dl);
 */

#include <stdint.h>

/* =========================================================================
 * DCGM return codes (dcgmReturn_t)
 * ========================================================================= */

#define DCGM_ST_OK            0
#define DCGM_ST_BADPARAM     (-1)
#define DCGM_ST_NOT_SUPPORTED (-6)
#define DCGM_ST_NO_DATA      (-14)
#define DCGM_ST_STALE_DATA   (-15)

/* =========================================================================
 * DCGM field type tags (stored in dcgm_field_value_t.fieldType)
 * ========================================================================= */

#define DCGM_FT_DOUBLE   'd'   /* ASCII 100 — value in .value.dbl  */
#define DCGM_FT_INT64    'l'   /* ASCII 108 — value in .value.i64  */

/* =========================================================================
 * DCGM group type constants
 * ========================================================================= */

#define DCGM_GROUP_EMPTY   1   /* empty group — add devices via GroupAddDevice */

/* =========================================================================
 * DCGM blank sentinel values
 *
 * When a field cannot be read, DCGM writes one of these into value.dbl or
 * value.i64.  Check against these before trusting the field value.
 * ========================================================================= */

#define DCGM_FP64_BLANK   ((double)140737488355328LL)      /* 2^47 */
#define DCGM_INT64_BLANK  ((int64_t)0x7FFFFFFFFFFFFFF0LL)  /* near INT64_MAX */

/* =========================================================================
 * DCGM field IDs
 *
 * Mirrored from dcgm_fields.h (stable ABI).  These numeric values are
 * passed to FieldGroupCreate and GetLatestValues and must match exactly.
 * Verified against DCGM open-source repository.
 * ========================================================================= */

#define DCGM_FI_DEV_POWER_USAGE                          155
#define DCGM_FI_DEV_TOTAL_ENERGY_CONSUMPTION             156  /* mJ since driver load */
#define DCGM_FI_DEV_PCIE_REPLAY_COUNTER                  202
#define DCGM_FI_DEV_MEM_COPY_UTIL                        204  /* %, returned as INT64 */
#define DCGM_FI_DEV_XID_ERRORS                           230  /* cumulative XID count */
#define DCGM_FI_DEV_LAST_XID                             237  /* last XID code; DCGM 2.x+ */
#define DCGM_FI_DEV_POWER_VIOLATION                      240  /* µs throttled, cumulative */
#define DCGM_FI_DEV_THERMAL_VIOLATION                    241  /* µs throttled, cumulative */
#define DCGM_FI_DEV_NVLINK_CRC_FLIT_ERROR_COUNT_TOTAL   409
#define DCGM_FI_DEV_NVLINK_REPLAY_ERROR_COUNT_TOTAL     429
#define DCGM_FI_DEV_NVLINK_RECOVERY_ERROR_COUNT_TOTAL   439
#define DCGM_FI_DEV_ROW_REMAP_FAILURE                   395  /* Ampere+ */

/* Number of fields subscribed to in dcgm_setup and polled in dcgm_poll */
#define DCGM_NUM_POLL_FIELDS  12

/* =========================================================================
 * Mirrored struct types — layout must match DCGM ABI exactly.
 * ========================================================================= */

/*
 * dcgm_field_value_t mirrors dcgmFieldValue_v1 from dcgm_structs.h.
 *
 * Padding:  version(4) + fieldId(2) + fieldType(2) + status(4) = 12 bytes,
 *           then 4 bytes implicit padding before ts(8) to satisfy alignment,
 *           giving ts at offset 16, union at offset 24.
 * Total size: 24 + 4096 = 4120 bytes.
 */
#define DCGM_MAX_STR_LENGTH   256
#define DCGM_MAX_BLOB_LENGTH  4096

typedef struct {
    unsigned int   version;    /* dcgmFieldValue_v1 version tag */
    unsigned short fieldId;    /* which field this value belongs to */
    unsigned short fieldType;  /* DCGM_FT_DOUBLE or DCGM_FT_INT64 */
    int            status;     /* DCGM_ST_OK on success */
    int64_t        ts;         /* timestamp, µs since epoch */
    union {
        int64_t  i64;
        double   dbl;
        char     str[DCGM_MAX_STR_LENGTH];
        char     blob[DCGM_MAX_BLOB_LENGTH];
    } value;
} dcgm_field_value_t;

/* =========================================================================
 * dcgm_fields_t — per-GPU polled output
 *
 * All fields are pre-filled with unavailable sentinels by dcgm_poll().
 * A field retains its sentinel if DCGM returns an error or a blank value.
 * ========================================================================= */

#define DCGM_FIELD_UNAVAILABLE_DBL  (-1.0)
#define DCGM_FIELD_UNAVAILABLE_U64  (UINT64_MAX)
#define DCGM_FIELD_UNAVAILABLE_U32  (UINT32_MAX)

typedef struct {
    double   power_w;               /* DCGM_FI_DEV_POWER_USAGE (155), W          */
    double   energy_j;              /* DCGM_FI_DEV_TOTAL_ENERGY_CONSUMPTION (156),
                                       returned as mJ INT64, converted to J       */
    double   mem_bw_util_pct;       /* DCGM_FI_DEV_MEM_COPY_UTIL (204), %        */
    uint64_t power_violation_us;    /* DCGM_FI_DEV_POWER_VIOLATION (240), µs     */
    uint64_t thermal_violation_us;  /* DCGM_FI_DEV_THERMAL_VIOLATION (241), µs   */
    uint64_t nvlink_replay;         /* DCGM_FI_DEV_NVLINK_REPLAY_ERROR_COUNT_TOTAL (429) */
    uint64_t nvlink_recovery;       /* DCGM_FI_DEV_NVLINK_RECOVERY_ERROR_COUNT_TOTAL (439) */
    uint64_t nvlink_crc;            /* DCGM_FI_DEV_NVLINK_CRC_FLIT_ERROR_COUNT_TOTAL (409) */
    uint64_t xid_count;             /* DCGM_FI_DEV_XID_ERRORS (230)              */
    uint32_t xid_last_code;         /* DCGM_FI_DEV_LAST_XID (237)                */
    uint64_t pcie_replay;           /* DCGM_FI_DEV_PCIE_REPLAY_COUNTER (202)     */
    uint32_t row_remap_failures;    /* DCGM_FI_DEV_ROW_REMAP_FAILURE (395)       */
} dcgm_fields_t;

/* =========================================================================
 * dcgm_vtable_t — function pointer table
 *
 * All DCGM handle and group IDs are opaque longs here to avoid any
 * dependency on dcgm_agent.h.  Populated by dcgm_load().
 *
 * NOTE: FieldGroupCreate parameter order matches the SPEC definition
 * (field_ids, count, name, &id).  The real dcgmFieldGroupCreate has
 * (count, field_ids, name, &id) — verify ABI when testing against a live
 * DCGM daemon.
 * ========================================================================= */

typedef struct {
    /* Library lifecycle */
    int  (*Init)(void);                                 /* dcgmInit      */
    void (*Shutdown)(void);                             /* dcgmShutdown  */

    /* Daemon connection */
    int  (*Connect)(const char *addr, long *handle);    /* dcgmConnect   */
    void (*Disconnect)(long handle);                    /* dcgmDisconnect */

    /* Startup: group and field group setup */
    int  (*GroupCreate)(long handle, int type, const char *name, long *group_id);
    int  (*GroupAddDevice)(long handle, long group_id, unsigned int gpu_id);
    int  (*FieldGroupCreate)(long handle, unsigned short *field_ids, int count,
                             const char *name, long *field_group_id);
    int  (*WatchFields)(long handle, long group_id, long field_group_id,
                        long update_freq_us, double max_keep_age_s,
                        int max_keep_samples);

    /* Per-poll: latest field values for one GPU */
    int  (*GetLatestValues)(long handle, int gpu_id,
                            unsigned short *fields, int count,
                            void /* dcgm_field_value_t */ *values);

    const char *(*ErrorString)(int result);             /* dcgmErrorString */
} dcgm_vtable_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/*
 * Load libdcgm.so.1 (falls back to libdcgm.so) and populate vt.
 * DCGM is optional — returns -1 if the library is not present.
 * On success: *dl_handle is set; caller must pass it to dcgm_unload() later.
 */
int  dcgm_load(dcgm_vtable_t *vt, void **dl_handle);

/* dlclose the handle obtained from dcgm_load. Safe to call with NULL. */
void dcgm_unload(void *dl_handle);

/*
 * Connect to the local DCGM daemon, create a device group containing all
 * gpu_ids[0..num_gpus-1], create a field group with all watched fields,
 * and install a 1 Hz watch.
 *
 * On success: *handle is set to the DCGM connection handle.
 * On failure: returns -1 (caller should set dcgm_available = 0).
 */
int  dcgm_setup(dcgm_vtable_t *vt, long *handle,
                unsigned int *gpu_ids, int num_gpus);

/*
 * Fetch the latest field values for gpu_id into out.
 * All fields in out are pre-filled with unavailable sentinels before the
 * DCGM call; individual fields retain their sentinel on field-level errors.
 * Returns 0 on success, -1 if GetLatestValues itself fails.
 */
int  dcgm_poll(dcgm_vtable_t *vt, long handle, int gpu_id, dcgm_fields_t *out);

/* Disconnect from the DCGM daemon and call Shutdown. */
void dcgm_teardown(dcgm_vtable_t *vt, long handle);

#endif /* GPU_HEALTH_DCGM_H */
