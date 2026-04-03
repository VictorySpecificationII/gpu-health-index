#ifndef GPU_HEALTH_SNAPSHOT_H
#define GPU_HEALTH_SNAPSHOT_H

/*
 * snapshot.h — mutex-protected snapshot assembly and socketpair IPC.
 *
 * snapshot_update: assembles gpu_snapshot_t from scorer output + current
 *   state + identity.  Acquires ctx->snapshot_mutex internally.
 *
 * snapshot_send/recv: atomic fixed-size read/write of gpu_ipc_msg_t over
 *   the socketpair.  One message per GPU per poll cycle.
 */

#include "types.h"
#include "collector.h"

/*
 * Assemble a fresh gpu_snapshot_t from the poll thread's current output.
 * Acquires ctx->snapshot_mutex for the write; the poll thread must have
 * already updated ctx->ring, ctx->state, ctx->baseline, and ctx->probe
 * before calling this.
 *
 * dcgm_available: 1 if DCGM is connected, 0 otherwise.
 */
void snapshot_update(gpu_ctx_t *ctx,
                     const gpu_score_result_t *score,
                     int dcgm_available);

/*
 * Write one gpu_ipc_msg_t to fd.
 * Acquires ctx->snapshot_mutex to copy the snapshot atomically.
 * Returns 0 on success, -1 on error (errno set).
 */
int snapshot_send(int fd, gpu_ctx_t *ctx);

/*
 * Read one gpu_ipc_msg_t from fd into msg.
 * Blocks until a full message is available.
 * Returns 0 on success, -1 on error or EOF.
 */
int snapshot_recv(int fd, gpu_ipc_msg_t *msg);

#endif /* GPU_HEALTH_SNAPSHOT_H */
