/*
 * test_dcgm.c — unit tests for dcgm.c
 *
 * No real DCGM daemon or libdcgm.so required.  All vtable function pointers
 * are replaced with deterministic fakes, exercising dcgm_setup, dcgm_poll,
 * and dcgm_teardown independently of hardware.
 */

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#include "dcgm.h"
#include "test_harness.h"

/* =========================================================================
 * Fake vtable state
 * ========================================================================= */

/* Call counters */
static int fake_init_calls;
static int fake_shutdown_calls;
static int fake_connect_calls;
static int fake_disconnect_calls;
static int fake_groupcreate_calls;
static int fake_groupadddevice_calls;
static int fake_fieldgroupcreate_calls;
static int fake_watchfields_calls;
static int fake_getlatestvalues_calls;

/* Configured return codes for error-path tests */
static int fake_init_ret;
static int fake_connect_ret;
static int fake_groupcreate_ret;
static int fake_groupadddevice_ret;
static int fake_fieldgroupcreate_ret;
static int fake_watchfields_ret;
static int fake_getlatestvalues_ret;

/* Per-field values that fake_GetLatestValues writes into the output.
 * val is a union so setting .i64 and .dbl don't overwrite each other. */
typedef union { int64_t i64; double dbl; } fake_val_u;
typedef struct {
    int        status;
    fake_val_u val;
} fake_fval_t;

static fake_fval_t fake_fvals[DCGM_NUM_POLL_FIELDS];

static void reset_fakes(void)
{
    fake_init_calls            = 0;
    fake_shutdown_calls        = 0;
    fake_connect_calls         = 0;
    fake_disconnect_calls      = 0;
    fake_groupcreate_calls     = 0;
    fake_groupadddevice_calls  = 0;
    fake_fieldgroupcreate_calls = 0;
    fake_watchfields_calls     = 0;
    fake_getlatestvalues_calls = 0;

    fake_init_ret              = DCGM_ST_OK;
    fake_connect_ret           = DCGM_ST_OK;
    fake_groupcreate_ret       = DCGM_ST_OK;
    fake_groupadddevice_ret    = DCGM_ST_OK;
    fake_fieldgroupcreate_ret  = DCGM_ST_OK;
    fake_watchfields_ret       = DCGM_ST_OK;
    fake_getlatestvalues_ret   = DCGM_ST_OK;

    memset(fake_fvals, 0, sizeof(fake_fvals));
    for (int i = 0; i < DCGM_NUM_POLL_FIELDS; i++) {
        fake_fvals[i].status  = DCGM_ST_OK;
        fake_fvals[i].val.i64 = 0;
    }
}

/* =========================================================================
 * Fake function implementations
 * ========================================================================= */

static int fake_Init(void)         { fake_init_calls++;     return fake_init_ret; }
static void fake_Shutdown(void)    { fake_shutdown_calls++; }

static int fake_Connect(const char *addr, long *handle)
{
    (void)addr;
    fake_connect_calls++;
    *handle = 42L;
    return fake_connect_ret;
}

static void fake_Disconnect(long handle)
{
    (void)handle;
    fake_disconnect_calls++;
}

static int fake_GroupCreate(long handle, int type, const char *name, long *id)
{
    (void)handle; (void)type; (void)name;
    fake_groupcreate_calls++;
    *id = 1L;
    return fake_groupcreate_ret;
}

static int fake_GroupAddDevice(long handle, long group_id, unsigned int gpu_id)
{
    (void)handle; (void)group_id; (void)gpu_id;
    fake_groupadddevice_calls++;
    return fake_groupadddevice_ret;
}

static int fake_FieldGroupCreate(long handle, unsigned short *field_ids,
                                 int count, const char *name, long *id)
{
    (void)handle; (void)field_ids; (void)count; (void)name;
    fake_fieldgroupcreate_calls++;
    *id = 2L;
    return fake_fieldgroupcreate_ret;
}

static int fake_WatchFields(long handle, long gid, long fgid,
                             long freq_us, double age_s, int n_samples)
{
    (void)handle; (void)gid; (void)fgid;
    (void)freq_us; (void)age_s; (void)n_samples;
    fake_watchfields_calls++;
    return fake_watchfields_ret;
}

static int fake_GetLatestValues(long handle, int gpu_id,
                                unsigned short *fields, int count,
                                void *values_void)
{
    (void)handle; (void)gpu_id; (void)fields;
    fake_getlatestvalues_calls++;

    if (fake_getlatestvalues_ret != DCGM_ST_OK)
        return fake_getlatestvalues_ret;

    dcgm_field_value_t *values = (dcgm_field_value_t *)values_void;
    for (int i = 0; i < count && i < DCGM_NUM_POLL_FIELDS; i++) {
        values[i].fieldId   = fields[i];
        values[i].status    = fake_fvals[i].status;
        /* Copy all 8 bytes of the union at once — avoids overwrite */
        values[i].value.i64 = fake_fvals[i].val.i64;
    }
    return DCGM_ST_OK;
}

static const char *fake_ErrorString(int result)
{
    (void)result;
    return "fake_error";
}

static dcgm_vtable_t make_vt(void)
{
    dcgm_vtable_t vt;
    memset(&vt, 0, sizeof(vt));
    vt.Init             = fake_Init;
    vt.Shutdown         = fake_Shutdown;
    vt.Connect          = fake_Connect;
    vt.Disconnect       = fake_Disconnect;
    vt.GroupCreate      = fake_GroupCreate;
    vt.GroupAddDevice   = fake_GroupAddDevice;
    vt.FieldGroupCreate = fake_FieldGroupCreate;
    vt.WatchFields      = fake_WatchFields;
    vt.GetLatestValues  = fake_GetLatestValues;
    vt.ErrorString      = fake_ErrorString;
    return vt;
}

/* =========================================================================
 * POLL_FIELDS index aliases — must match the order in dcgm.c
 * ========================================================================= */

enum {
    FI_POWER_W      = 0,
    FI_ENERGY_MJ    = 1,
    FI_MEM_BW       = 2,
    FI_PWR_VIO      = 3,
    FI_THERM_VIO    = 4,
    FI_NVL_CRC      = 5,
    FI_NVL_REPLAY   = 6,
    FI_NVL_RECOV    = 7,
    FI_XID_COUNT    = 8,
    FI_XID_LAST     = 9,
    FI_PCIE_REPLAY  = 10,
    FI_ROW_REMAP    = 11,
};

/* =========================================================================
 * Tests: struct layout
 * ========================================================================= */

static void test_dcgm_num_poll_fields_is_12(void)
{
    ASSERT_EQ_INT(12, DCGM_NUM_POLL_FIELDS);
}

static void test_dcgm_field_value_size(void)
{
    /*
     * dcgm_field_value_t must be 4120 bytes to match DCGM ABI:
     *   uint(4) + ushort(2) + ushort(2) + int(4) + pad(4) + i64(8) + blob(4096) = 4120
     */
    ASSERT_EQ_INT(4120, (int)sizeof(dcgm_field_value_t));
}

static void test_field_unavailable_sentinels(void)
{
    ASSERT(DCGM_FIELD_UNAVAILABLE_DBL == -1.0);
    ASSERT(DCGM_FIELD_UNAVAILABLE_U64 == UINT64_MAX);
    ASSERT(DCGM_FIELD_UNAVAILABLE_U32 == UINT32_MAX);
}

/* =========================================================================
 * Tests: dcgm_load / dcgm_unload
 * ========================================================================= */

static void test_load_graceful_failure_without_library(void)
{
    dcgm_vtable_t vt;
    void *dl = NULL;
    int ret = dcgm_load(&vt, &dl);
    /* -1 when library absent, 0 if somehow installed — both are acceptable */
    ASSERT(ret == -1 || ret == 0);
    if (ret == 0)
        dcgm_unload(dl);
}

static void test_unload_null_safe(void)
{
    dcgm_unload(NULL);  /* must not crash */
    ASSERT(1);
}

/* =========================================================================
 * Tests: dcgm_setup call sequence
 * ========================================================================= */

static void test_setup_happy_path_two_gpus(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    long handle = 0;
    unsigned int gpu_ids[] = {0, 1};

    int ret = dcgm_setup(&vt, &handle, gpu_ids, 2);

    ASSERT_EQ_INT(0, ret);
    ASSERT_EQ_INT(42, (int)handle);
    ASSERT_EQ_INT(1, fake_init_calls);
    ASSERT_EQ_INT(1, fake_connect_calls);
    ASSERT_EQ_INT(1, fake_groupcreate_calls);
    ASSERT_EQ_INT(2, fake_groupadddevice_calls);
    ASSERT_EQ_INT(1, fake_fieldgroupcreate_calls);
    ASSERT_EQ_INT(1, fake_watchfields_calls);
    ASSERT_EQ_INT(0, fake_disconnect_calls);
    ASSERT_EQ_INT(0, fake_shutdown_calls);
}

static void test_setup_single_gpu(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    long handle = 0;
    unsigned int gpu_ids[] = {0};

    int ret = dcgm_setup(&vt, &handle, gpu_ids, 1);

    ASSERT_EQ_INT(0, ret);
    ASSERT_EQ_INT(1, fake_groupadddevice_calls);
}

static void test_setup_init_failure_aborts_early(void)
{
    reset_fakes();
    fake_init_ret = DCGM_ST_BADPARAM;
    dcgm_vtable_t vt = make_vt();
    long handle = 0;
    unsigned int gpu_ids[] = {0};

    int ret = dcgm_setup(&vt, &handle, gpu_ids, 1);

    ASSERT_EQ_INT(-1, ret);
    ASSERT_EQ_INT(1,  fake_init_calls);
    ASSERT_EQ_INT(0,  fake_connect_calls);
    ASSERT_EQ_INT(0,  fake_disconnect_calls);
    ASSERT_EQ_INT(0,  fake_shutdown_calls);
}

static void test_setup_connect_failure_calls_shutdown(void)
{
    reset_fakes();
    fake_connect_ret = DCGM_ST_BADPARAM;
    dcgm_vtable_t vt = make_vt();
    long handle = 0;
    unsigned int gpu_ids[] = {0};

    int ret = dcgm_setup(&vt, &handle, gpu_ids, 1);

    ASSERT_EQ_INT(-1, ret);
    ASSERT_EQ_INT(1,  fake_connect_calls);
    ASSERT_EQ_INT(0,  fake_groupcreate_calls);
    ASSERT_EQ_INT(0,  fake_disconnect_calls);
    ASSERT_EQ_INT(1,  fake_shutdown_calls);
}

static void test_setup_groupcreate_failure_disconnects(void)
{
    reset_fakes();
    fake_groupcreate_ret = DCGM_ST_BADPARAM;
    dcgm_vtable_t vt = make_vt();
    long handle = 0;
    unsigned int gpu_ids[] = {0};

    int ret = dcgm_setup(&vt, &handle, gpu_ids, 1);

    ASSERT_EQ_INT(-1, ret);
    ASSERT_EQ_INT(1,  fake_disconnect_calls);
    ASSERT_EQ_INT(1,  fake_shutdown_calls);
}

static void test_setup_adddevice_failure_disconnects(void)
{
    reset_fakes();
    fake_groupadddevice_ret = DCGM_ST_BADPARAM;
    dcgm_vtable_t vt = make_vt();
    long handle = 0;
    unsigned int gpu_ids[] = {0, 1};

    int ret = dcgm_setup(&vt, &handle, gpu_ids, 2);

    ASSERT_EQ_INT(-1, ret);
    ASSERT_EQ_INT(1,  fake_disconnect_calls);
    ASSERT_EQ_INT(1,  fake_shutdown_calls);
}

static void test_setup_fieldgroupcreate_failure_disconnects(void)
{
    reset_fakes();
    fake_fieldgroupcreate_ret = DCGM_ST_BADPARAM;
    dcgm_vtable_t vt = make_vt();
    long handle = 0;
    unsigned int gpu_ids[] = {0};

    int ret = dcgm_setup(&vt, &handle, gpu_ids, 1);

    ASSERT_EQ_INT(-1, ret);
    ASSERT_EQ_INT(1,  fake_disconnect_calls);
    ASSERT_EQ_INT(1,  fake_shutdown_calls);
}

static void test_setup_watchfields_failure_disconnects(void)
{
    reset_fakes();
    fake_watchfields_ret = DCGM_ST_BADPARAM;
    dcgm_vtable_t vt = make_vt();
    long handle = 0;
    unsigned int gpu_ids[] = {0};

    int ret = dcgm_setup(&vt, &handle, gpu_ids, 1);

    ASSERT_EQ_INT(-1, ret);
    ASSERT_EQ_INT(1,  fake_disconnect_calls);
    ASSERT_EQ_INT(1,  fake_shutdown_calls);
}

/* =========================================================================
 * Tests: dcgm_poll field extraction
 * ========================================================================= */

static void test_poll_zero_values_extracted(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    dcgm_fields_t out;

    /* All zero with ST_OK — should extract 0 into all fields */
    for (int i = 0; i < DCGM_NUM_POLL_FIELDS; i++) {
        fake_fvals[i].status = DCGM_ST_OK;
        fake_fvals[i].val.i64 = 0;
        fake_fvals[i].val.dbl = 0.0;
    }

    int ret = dcgm_poll(&vt, 42L, 0, &out);

    ASSERT_EQ_INT(0, ret);
    ASSERT(fabs(out.power_w)         < 1e-9);
    ASSERT(fabs(out.energy_j)        < 1e-9);
    ASSERT(fabs(out.mem_bw_util_pct) < 1e-9);
    ASSERT(out.power_violation_us   == 0);
    ASSERT(out.thermal_violation_us == 0);
    ASSERT(out.nvlink_crc           == 0);
    ASSERT(out.nvlink_replay        == 0);
    ASSERT(out.nvlink_recovery      == 0);
    ASSERT(out.xid_count            == 0);
    ASSERT(out.xid_last_code        == 0);
    ASSERT(out.pcie_replay          == 0);
    ASSERT(out.row_remap_failures   == 0);
}

static void test_poll_power_w_extracted(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    dcgm_fields_t out;

    fake_fvals[FI_POWER_W].val.dbl = 350.5;

    dcgm_poll(&vt, 42L, 0, &out);
    ASSERT(fabs(out.power_w - 350.5) < 1e-9);
}

static void test_poll_energy_mj_converts_to_j(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    dcgm_fields_t out;

    fake_fvals[FI_ENERGY_MJ].val.i64 = 5000LL;  /* 5000 mJ → 5.0 J */

    dcgm_poll(&vt, 42L, 0, &out);
    ASSERT(fabs(out.energy_j - 5.0) < 1e-9);
}

static void test_poll_mem_bw_util_int_to_double(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    dcgm_fields_t out;

    fake_fvals[FI_MEM_BW].val.i64 = 87LL;

    dcgm_poll(&vt, 42L, 0, &out);
    ASSERT(fabs(out.mem_bw_util_pct - 87.0) < 1e-9);
}

static void test_poll_violation_counters(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    dcgm_fields_t out;

    fake_fvals[FI_PWR_VIO].val.i64 = 12345LL;
    fake_fvals[FI_THERM_VIO].val.i64 = 67890LL;

    dcgm_poll(&vt, 42L, 0, &out);
    ASSERT(out.power_violation_us   == 12345ULL);
    ASSERT(out.thermal_violation_us == 67890ULL);
}

static void test_poll_nvlink_counters(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    dcgm_fields_t out;

    fake_fvals[FI_NVL_CRC].val.i64 = 1LL;
    fake_fvals[FI_NVL_REPLAY].val.i64 = 2LL;
    fake_fvals[FI_NVL_RECOV].val.i64 = 3LL;

    dcgm_poll(&vt, 42L, 0, &out);
    ASSERT(out.nvlink_crc      == 1ULL);
    ASSERT(out.nvlink_replay   == 2ULL);
    ASSERT(out.nvlink_recovery == 3ULL);
}

static void test_poll_xid_count_and_last_code(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    dcgm_fields_t out;

    fake_fvals[FI_XID_COUNT].val.i64 = 42LL;
    fake_fvals[FI_XID_LAST].val.i64 = 79LL;

    dcgm_poll(&vt, 42L, 0, &out);
    ASSERT(out.xid_count     == 42ULL);
    ASSERT(out.xid_last_code == 79U);
}

static void test_poll_pcie_replay_and_row_remap(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    dcgm_fields_t out;

    fake_fvals[FI_PCIE_REPLAY].val.i64 = 7LL;
    fake_fvals[FI_ROW_REMAP].val.i64 = 1LL;

    dcgm_poll(&vt, 42L, 0, &out);
    ASSERT(out.pcie_replay        == 7ULL);
    ASSERT(out.row_remap_failures == 1U);
}

static void test_poll_field_status_error_leaves_sentinel(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    dcgm_fields_t out;

    /* All fields good except power_w which is marked NO_DATA */
    for (int i = 0; i < DCGM_NUM_POLL_FIELDS; i++) {
        fake_fvals[i].status = DCGM_ST_OK;
        fake_fvals[i].val.dbl = 100.0;
        fake_fvals[i].val.i64 = 100LL;
    }
    fake_fvals[FI_POWER_W].status = DCGM_ST_NO_DATA;

    dcgm_poll(&vt, 42L, 0, &out);
    ASSERT(out.power_w == DCGM_FIELD_UNAVAILABLE_DBL);
    /* energy_j: 100 mJ → 0.1 J */
    ASSERT(fabs(out.energy_j - 0.1) < 1e-9);
}

static void test_poll_fp64_blank_leaves_sentinel(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    dcgm_fields_t out;

    fake_fvals[FI_POWER_W].status = DCGM_ST_OK;
    fake_fvals[FI_POWER_W].val.dbl = DCGM_FP64_BLANK;

    dcgm_poll(&vt, 42L, 0, &out);
    ASSERT(out.power_w == DCGM_FIELD_UNAVAILABLE_DBL);
}

static void test_poll_int64_blank_leaves_sentinel(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    dcgm_fields_t out;

    fake_fvals[FI_XID_COUNT].status = DCGM_ST_OK;
    fake_fvals[FI_XID_COUNT].val.i64 = DCGM_INT64_BLANK;

    dcgm_poll(&vt, 42L, 0, &out);
    ASSERT(out.xid_count == DCGM_FIELD_UNAVAILABLE_U64);
}

static void test_poll_getlatestvalues_error_returns_minus1(void)
{
    reset_fakes();
    fake_getlatestvalues_ret = DCGM_ST_NO_DATA;
    dcgm_vtable_t vt = make_vt();
    dcgm_fields_t out;

    int ret = dcgm_poll(&vt, 42L, 0, &out);

    ASSERT_EQ_INT(-1, ret);
    /* Sentinels must still be set even on early return */
    ASSERT(out.power_w   == DCGM_FIELD_UNAVAILABLE_DBL);
    ASSERT(out.xid_count == DCGM_FIELD_UNAVAILABLE_U64);
}

static void test_poll_all_blanked_gives_all_sentinels(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    dcgm_fields_t out;

    for (int i = 0; i < DCGM_NUM_POLL_FIELDS; i++) {
        fake_fvals[i].status = DCGM_ST_OK;
        fake_fvals[i].val.dbl = DCGM_FP64_BLANK;
        fake_fvals[i].val.i64 = DCGM_INT64_BLANK;
    }

    int ret = dcgm_poll(&vt, 42L, 0, &out);

    ASSERT_EQ_INT(0, ret);
    ASSERT(out.power_w              == DCGM_FIELD_UNAVAILABLE_DBL);
    ASSERT(out.energy_j             == DCGM_FIELD_UNAVAILABLE_DBL);
    ASSERT(out.mem_bw_util_pct      == DCGM_FIELD_UNAVAILABLE_DBL);
    ASSERT(out.power_violation_us   == DCGM_FIELD_UNAVAILABLE_U64);
    ASSERT(out.thermal_violation_us == DCGM_FIELD_UNAVAILABLE_U64);
    ASSERT(out.nvlink_crc           == DCGM_FIELD_UNAVAILABLE_U64);
    ASSERT(out.nvlink_replay        == DCGM_FIELD_UNAVAILABLE_U64);
    ASSERT(out.nvlink_recovery      == DCGM_FIELD_UNAVAILABLE_U64);
    ASSERT(out.xid_count            == DCGM_FIELD_UNAVAILABLE_U64);
    ASSERT(out.xid_last_code        == DCGM_FIELD_UNAVAILABLE_U32);
    ASSERT(out.pcie_replay          == DCGM_FIELD_UNAVAILABLE_U64);
    ASSERT(out.row_remap_failures   == DCGM_FIELD_UNAVAILABLE_U32);
}

static void test_poll_xid_last_negative_gives_sentinel(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();
    dcgm_fields_t out;

    fake_fvals[FI_XID_LAST].status = DCGM_ST_OK;
    fake_fvals[FI_XID_LAST].val.i64 = -1LL;

    dcgm_poll(&vt, 42L, 0, &out);
    ASSERT(out.xid_last_code == DCGM_FIELD_UNAVAILABLE_U32);
}

/* =========================================================================
 * Tests: dcgm_teardown
 * ========================================================================= */

static void test_teardown_calls_disconnect_and_shutdown(void)
{
    reset_fakes();
    dcgm_vtable_t vt = make_vt();

    dcgm_teardown(&vt, 42L);

    ASSERT_EQ_INT(1, fake_disconnect_calls);
    ASSERT_EQ_INT(1, fake_shutdown_calls);
    ASSERT_EQ_INT(0, fake_init_calls);
    ASSERT_EQ_INT(0, fake_connect_calls);
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void)
{
    RUN_TEST(test_dcgm_num_poll_fields_is_12);
    RUN_TEST(test_dcgm_field_value_size);
    RUN_TEST(test_field_unavailable_sentinels);

    RUN_TEST(test_load_graceful_failure_without_library);
    RUN_TEST(test_unload_null_safe);

    RUN_TEST(test_setup_happy_path_two_gpus);
    RUN_TEST(test_setup_single_gpu);
    RUN_TEST(test_setup_init_failure_aborts_early);
    RUN_TEST(test_setup_connect_failure_calls_shutdown);
    RUN_TEST(test_setup_groupcreate_failure_disconnects);
    RUN_TEST(test_setup_adddevice_failure_disconnects);
    RUN_TEST(test_setup_fieldgroupcreate_failure_disconnects);
    RUN_TEST(test_setup_watchfields_failure_disconnects);

    RUN_TEST(test_poll_zero_values_extracted);
    RUN_TEST(test_poll_power_w_extracted);
    RUN_TEST(test_poll_energy_mj_converts_to_j);
    RUN_TEST(test_poll_mem_bw_util_int_to_double);
    RUN_TEST(test_poll_violation_counters);
    RUN_TEST(test_poll_nvlink_counters);
    RUN_TEST(test_poll_xid_count_and_last_code);
    RUN_TEST(test_poll_pcie_replay_and_row_remap);
    RUN_TEST(test_poll_field_status_error_leaves_sentinel);
    RUN_TEST(test_poll_fp64_blank_leaves_sentinel);
    RUN_TEST(test_poll_int64_blank_leaves_sentinel);
    RUN_TEST(test_poll_getlatestvalues_error_returns_minus1);
    RUN_TEST(test_poll_all_blanked_gives_all_sentinels);
    RUN_TEST(test_poll_xid_last_negative_gives_sentinel);

    RUN_TEST(test_teardown_calls_disconnect_and_shutdown);

    return TEST_RESULT();
}
