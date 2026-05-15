#ifndef GPU_HEALTH_FILESD_H
#define GPU_HEALTH_FILESD_H

#include "types.h"

/*
 * Write a Prometheus file_sd JSON target file so this node appears
 * automatically in scrape discovery.  Writes atomically via a tmp file +
 * rename.  No-op if cfg->file_sd_path is empty.
 *
 * Format: [{"targets":["hostname:port"],"labels":{"job":"gpu-health"}}]
 */
void filesd_write(const gpu_config_t *cfg);

/* Remove the file_sd file on clean shutdown.  No-op if path is empty. */
void filesd_remove(const gpu_config_t *cfg);

#endif /* GPU_HEALTH_FILESD_H */
