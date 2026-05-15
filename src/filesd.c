#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "filesd.h"
#include "util.h"

void filesd_write(const gpu_config_t *cfg)
{
    if (!cfg->file_sd_path[0])
        return;

    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        log_warn("filesd: gethostname failed: %s — skipping file_sd write",
                 strerror(errno));
        return;
    }
    hostname[sizeof(hostname) - 1] = '\0';

    /* Atomic write: write to .tmp then rename into place */
    char tmp[sizeof(cfg->file_sd_path) + 4];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg->file_sd_path);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        log_warn("filesd: cannot open %s for writing: %s",
                 tmp, strerror(errno));
        return;
    }

    fprintf(f, "[{\"targets\":[\"%s:%d\"],\"labels\":{\"job\":\"gpu-health\"}}]\n",
            hostname, cfg->listen_port);
    fflush(f);
    fclose(f);

    if (rename(tmp, cfg->file_sd_path) != 0) {
        log_warn("filesd: rename %s → %s failed: %s",
                 tmp, cfg->file_sd_path, strerror(errno));
        unlink(tmp);
        return;
    }

    log_info("filesd: wrote %s (target %s:%d)",
             cfg->file_sd_path, hostname, cfg->listen_port);
}

void filesd_remove(const gpu_config_t *cfg)
{
    if (!cfg->file_sd_path[0])
        return;

    if (unlink(cfg->file_sd_path) != 0 && errno != ENOENT)
        log_warn("filesd: unlink %s failed: %s",
                 cfg->file_sd_path, strerror(errno));
    else
        log_info("filesd: removed %s", cfg->file_sd_path);
}
