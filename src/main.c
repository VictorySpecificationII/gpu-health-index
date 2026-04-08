/*
 * main.c — GPU health exporter entry point.
 *
 * Startup sequence (DESIGN.md §5.5):
 *  1. Parse args, load and validate config
 *  2. Validate runtime directories
 *  3. dlopen NVML → Init()
 *  4. dlopen DCGM → attempt connection (soft fail)
 *  5. Enumerate GPUs; init contexts, load baselines/probes, start collectors
 *  6. Create socketpair; write gpu_ipc_init_t
 *  7. fork() → child: close NVML, drop privs, http_child_run()
 *              parent: close child_fd, drop privs
 *  8. Wait for first poll on all GPUs (timeout = nvml_timeout_ms × 2)
 *  9. Log any GPUs that timed out
 * 10. sd_notify READY=1 if NOTIFY_SOCKET is set
 * 11. Steady-state monitor loop: child watchdog + inotify baseline reload
 * 12. On SIGTERM/SIGINT: stop collectors, kill child, teardown, exit
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "collector.h"
#include "config.h"
#include "dcgm.h"
#include "http.h"
#include "nvml.h"
#include "procpriv.h"
#include "state.h"
#include "util.h"

#define GPU_HEALTH_VERSION "0.1.0"

/* --------------------------------------------------------------------------
 * Signal handling
 * -------------------------------------------------------------------------- */

static volatile int g_running = 1;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

/* --------------------------------------------------------------------------
 * sd_notify helper — no libsystemd dependency.
 * Sends "READY=1\n" to NOTIFY_SOCKET if the env var is set.
 * Handles both abstract-namespace sockets (leading '@') and path sockets.
 * -------------------------------------------------------------------------- */

static void sd_notify_ready(void) {
    const char *sock_path = getenv("NOTIFY_SOCKET");
    if (!sock_path || sock_path[0] == '\0')
        return;

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        log_warn("main: sd_notify: socket: %s", strerror(errno));
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (sock_path[0] == '@') {
        /* Abstract namespace: replace '@' with NUL */
        addr.sun_path[0] = '\0';
        strncpy(addr.sun_path + 1, sock_path + 1, sizeof(addr.sun_path) - 2);
    } else {
        strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    }

    const char msg[] = "READY=1\n";
    if (sendto(fd, msg, sizeof(msg) - 1, MSG_NOSIGNAL,
               (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_warn("main: sd_notify: sendto: %s", strerror(errno));
    } else {
        log_info("main: sd_notify READY=1 sent");
    }
    close(fd);
}

/* --------------------------------------------------------------------------
 * IPC init write helper
 * Writes gpu_ipc_init_t to fd.  Returns 0 on success, -1 on error.
 * -------------------------------------------------------------------------- */

static int write_ipc_init(int fd, int num_gpus) {
    gpu_ipc_init_t init;
    memset(&init, 0, sizeof(init));
    init.num_gpus = (int32_t)num_gpus;

    const char *p = (const char *)&init;
    size_t rem = sizeof(init);
    while (rem > 0) {
        ssize_t n = write(fd, p, rem);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            log_error("main: write_ipc_init: %s", strerror(errno));
            return -1;
        }
        p   += n;
        rem -= (size_t)n;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * HTTP child spawn
 *
 * Creates a new socketpair, writes the init message, forks, and runs the
 * HTTP child.  On return the parent has exp->parent_fd updated and
 * exp->child_pid set.  Returns 0 on success, -1 on error.
 *
 * Called once at startup and again on unexpected child exit (respawn).
 *
 * Thread safety: the poll threads write to exp->parent_fd.  We atomically
 * replace parent_fd after the new socket is ready; the worst case is one
 * snapshot_send() call failing with EBADF during the brief swap window,
 * which snapshot_send() handles by logging and returning -1.
 * -------------------------------------------------------------------------- */

static int spawn_http_child(exporter_t *exp) {
    /* Create a new socketpair for this child instance. */
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        log_error("main: socketpair: %s", strerror(errno));
        return -1;
    }
    /* fds[0] = parent end, fds[1] = child end */

    /* Write the IPC init message so the child sees it immediately on startup. */
    if (write_ipc_init(fds[0], exp->num_gpus) < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_error("main: fork: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        /* ----------------------------------------------------------------
         * Child process
         * ---------------------------------------------------------------- */

        /* Close the parent end — child only reads from fds[1]. */
        close(fds[0]);

        /* Close the previous parent_fd if this is a respawn. */
        if (exp->parent_fd >= 0)
            close(exp->parent_fd);


        /*
         * Do NOT call NVML/DCGM teardown in the forked child.
         *
         * These libraries were initialized in the parent before fork().
         * Their internal state is not guaranteed to be fork-safe, and
         * nvmlShutdown() may segfault in the child.
         *
         * The HTTP child must never use NVML/DCGM; ownership and teardown
         * remain entirely in the parent.
         */
        memset(&exp->nvml, 0, sizeof(exp->nvml));
        exp->nvml_dl = NULL;

        memset(&exp->dcgm, 0, sizeof(exp->dcgm));
        exp->dcgm_dl = NULL;
        exp->dcgm_handle = 0;
        exp->dcgm_available = 0;

        /* Drop capabilities and install seccomp (Phase 1: PR_SET_NO_NEW_PRIVS). */
        procpriv_child_setup();

        /* Enter the HTTP serve loop — does not return. */
        http_child_run(fds[1], &exp->cfg);

        /* http_child_run() calls exit() internally; this is unreachable. */
        _exit(1);
    }

    /* ----------------------------------------------------------------
     * Parent process
     * ---------------------------------------------------------------- */

    /* Atomically swap parent_fd before closing the old one. */
    int old_parent_fd = exp->parent_fd;
    exp->parent_fd    = fds[0];  /* poll threads pick this up on next write */
    exp->child_pid    = pid;

    close(fds[1]);              /* parent does not read from the child end  */
    if (old_parent_fd >= 0)
        close(old_parent_fd);   /* close after swap — brief EBADF window is OK */

    log_info("main: HTTP child spawned (pid %d)", (int)pid);
    return 0;
}

/* --------------------------------------------------------------------------
 * Graceful shutdown
 * Stop all poll threads, send SIGTERM to child, wait, teardown NVML/DCGM.
 * -------------------------------------------------------------------------- */

static void shutdown_all(exporter_t *exp) {
    log_info("main: shutting down");

    /* Stop all poll threads. */
    for (int i = 0; i < exp->num_gpus; i++)
        exp->gpus[i].stop = 1;
    for (int i = 0; i < exp->num_gpus; i++)
        collector_stop(&exp->gpus[i]);

    /* Terminate HTTP child. */
    if (exp->child_pid > 0) {
        kill(exp->child_pid, SIGTERM);
        /* Wait up to 3 seconds for clean exit. */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };
        for (int t = 0; t < 30; t++) {
            int status;
            if (waitpid(exp->child_pid, &status, WNOHANG) == exp->child_pid)
                break;
            nanosleep(&ts, NULL);
        }
        /* Force kill if still running. */
        waitpid(exp->child_pid, NULL, WNOHANG);
    }

    if (exp->parent_fd >= 0) {
        close(exp->parent_fd);
        exp->parent_fd = -1;
    }

    if (exp->baseline_inotify_fd >= 0) {
        close(exp->baseline_inotify_fd);
        exp->baseline_inotify_fd = -1;
    }

    /* Teardown NVML and DCGM. */
    if (exp->nvml.Shutdown)
        exp->nvml.Shutdown();
    nvml_unload(exp->nvml_dl);

    if (exp->dcgm_available) {
        dcgm_teardown(&exp->dcgm, exp->dcgm_handle);
        dcgm_unload(exp->dcgm_dl);
    }

    free(exp->gpus);
}

/* --------------------------------------------------------------------------
 * Usage
 * -------------------------------------------------------------------------- */

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [OPTIONS]\n"
            "\n"
            "Options:\n"
            "  -c <file>   Config file path "
            "(default: /etc/gpu-health/gpu-health.conf)\n"
            "  -h          Print this help and exit\n"
            "  -v          Print version and exit\n",
            prog);
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *config_path = "/etc/gpu-health/gpu-health.conf";

    /* ------------------------------------------------------------------ */
    /* 0. Arg parsing                                                      */
    /* ------------------------------------------------------------------ */
    int opt;
    while ((opt = getopt(argc, argv, "c:hv")) != -1) {
        switch (opt) {
        case 'c':
            config_path = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        case 'v':
            printf("gpu-health-exporter %s\n", GPU_HEALTH_VERSION);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* ------------------------------------------------------------------ */
    /* 1. Config load + validate                                           */
    /* ------------------------------------------------------------------ */
    log_init();

    exporter_t exp;
    memset(&exp, 0, sizeof(exp));
    exp.parent_fd            = -1;
    exp.child_fd             = -1;
    exp.child_pid            = -1;
    exp.baseline_inotify_fd  = -1;

    /* config_load: try the file path; if not found, uses defaults + env. */
    if (config_load(config_path, &exp.cfg) < 0) {
        log_error("main: config load failed — exiting");
        return 1;
    }
    if (config_validate(&exp.cfg) < 0) {
        log_error("main: config validation failed — exiting");
        return 1;
    }

    log_info("gpu-health-exporter %s starting", GPU_HEALTH_VERSION);

    /* ------------------------------------------------------------------ */
    /* 2. Validate runtime directories                                     */
    /* ------------------------------------------------------------------ */

    /* state_dir must exist and be writable — the exporter writes state files. */
    if (access(exp.cfg.state_dir, W_OK | X_OK) < 0) {
        log_error("main: state_dir '%s' is not writable: %s",
                  exp.cfg.state_dir, strerror(errno));
        log_error("main: create it with: mkdir -p %s", exp.cfg.state_dir);
        return 1;
    }

    /* baseline_dir: soft warn if not accessible — GPU can run without baselines. */
    if (access(exp.cfg.baseline_dir, R_OK | X_OK) < 0) {
        log_warn("main: baseline_dir '%s' is not readable: %s "
                 "— baselines will be unavailable",
                 exp.cfg.baseline_dir, strerror(errno));
    }

    /* ------------------------------------------------------------------ */
    /* 3. Load NVML                                                        */
    /* ------------------------------------------------------------------ */
    if (nvml_load(&exp.nvml, &exp.nvml_dl) < 0) {
        log_error("main: failed to load NVML — is the NVIDIA driver installed?");
        return 1;
    }

    if (exp.nvml.Init() != 0) {
        log_error("main: nvmlInit_v2() failed");
        nvml_unload(exp.nvml_dl);
        return 1;
    }
    log_info("main: NVML initialised");

    /* ------------------------------------------------------------------ */
    /* 4. Load DCGM (optional — soft fail)                                */
    /* ------------------------------------------------------------------ */
    exp.dcgm_available = 0;
    if (dcgm_load(&exp.dcgm, &exp.dcgm_dl) == 0) {
        /*
         * dcgm_setup is called after GPU enumeration so we have the gpu_ids
         * array.  Initialise here to get the library ready.
         */
        if (exp.dcgm.Init && exp.dcgm.Init() == 0) {
            log_info("main: DCGM library loaded — connection deferred "
                     "until GPU enumeration");
            exp.dcgm_available = -1;  /* tentative: loaded but not yet connected */
        } else {
            log_warn("main: DCGM Init() failed — running without DCGM");
            dcgm_unload(exp.dcgm_dl);
            exp.dcgm_dl = NULL;
            memset(&exp.dcgm, 0, sizeof(exp.dcgm));
        }
    } else {
        log_info("main: DCGM library not found — running NVML-only mode");
    }

    /* ------------------------------------------------------------------ */
    /* 5. Enumerate GPUs                                                   */
    /* ------------------------------------------------------------------ */
    unsigned int gpu_count = 0;
    if (exp.nvml.DeviceGetCount(&gpu_count) != 0 || gpu_count == 0) {
        log_error("main: nvmlDeviceGetCount() returned 0 GPUs — nothing to monitor");
        exp.nvml.Shutdown();
        nvml_unload(exp.nvml_dl);
        return 1;
    }

    exp.num_gpus = (int)gpu_count;
    exp.gpus     = calloc((size_t)exp.num_gpus, sizeof(gpu_ctx_t));
    if (!exp.gpus) {
        log_error("main: calloc for GPU context array failed");
        exp.nvml.Shutdown();
        nvml_unload(exp.nvml_dl);
        return 1;
    }

    log_info("main: found %d GPU(s)", exp.num_gpus);

    /* Build gpu_ids for dcgm_setup and retrieve per-GPU static properties. */
    unsigned int *gpu_ids = calloc((size_t)exp.num_gpus, sizeof(unsigned int));
    if (!gpu_ids) {
        log_error("main: calloc for gpu_ids failed");
        free(exp.gpus);
        exp.nvml.Shutdown();
        nvml_unload(exp.nvml_dl);
        return 1;
    }

    /* Retrieve driver version once — it's the same for all GPUs. */
    char driver_version[64] = "(unknown)";
    if (exp.nvml.SystemGetDriverVersion) {
        exp.nvml.SystemGetDriverVersion(driver_version, sizeof(driver_version));
    }
    log_info("main: driver version: %s", driver_version);

    for (int i = 0; i < exp.num_gpus; i++) {
        gpu_ctx_t *ctx = &exp.gpus[i];
        gpu_ids[i]     = (unsigned int)i;

        ctx->gpu_index    = i;
        ctx->gpu_present  = 1;
        ctx->gpu_available = 1;

        /* Device handle */
        if (exp.nvml.DeviceGetHandleByIndex((unsigned int)i,
                                             &ctx->nvml_handle) != 0) {
            log_error("main: gpu[%d]: DeviceGetHandleByIndex failed", i);
            /* Continue — collector will detect and handle on first poll. */
        }

        /* Serial → primary identity label */
        if (!exp.nvml.DeviceGetSerial ||
            exp.nvml.DeviceGetSerial(ctx->nvml_handle,
                                     ctx->serial,
                                     sizeof(ctx->serial)) != 0) {
            /* Fall back to UUID as serial */
            if (exp.nvml.DeviceGetUUID)
                exp.nvml.DeviceGetUUID(ctx->nvml_handle,
                                       ctx->serial, sizeof(ctx->serial));
            ctx->identity_source = GPU_IDENTITY_UUID;
            log_warn("main: gpu[%d]: serial unavailable — using UUID as identity",
                     i);
        } else {
            ctx->identity_source = GPU_IDENTITY_SERIAL;
        }

        /* UUID (always try, even if we already have serial) */
        if (exp.nvml.DeviceGetUUID)
            exp.nvml.DeviceGetUUID(ctx->nvml_handle,
                                   ctx->uuid, sizeof(ctx->uuid));

        /* Model name */
        if (exp.nvml.DeviceGetName)
            exp.nvml.DeviceGetName(ctx->nvml_handle,
                                   ctx->gpu_model, sizeof(ctx->gpu_model));

        safe_strncpy(ctx->driver_version, driver_version,
                     sizeof(ctx->driver_version));

        /* Static PCIe capability */
        unsigned int u = 0;
        if (exp.nvml.DeviceGetMaxPcieLinkGeneration &&
            exp.nvml.DeviceGetMaxPcieLinkGeneration(ctx->nvml_handle, &u) == 0)
            ctx->pcie_link_gen_max = (int)u;
        u = 0;
        if (exp.nvml.DeviceGetMaxPcieLinkWidth &&
            exp.nvml.DeviceGetMaxPcieLinkWidth(ctx->nvml_handle, &u) == 0)
            ctx->pcie_link_width_max = (int)u;

        /* Initialise DCGM-sourced state fields to NaN — avoid emitting 0 for
           unavailable data before the first poll cycle runs. */
        ctx->state.board_power_w   = 0.0;
        ctx->state.energy_j        = 0.0;
        ctx->state.mem_bw_util_pct = 0.0;
        ctx->state.fan_speed_pct   = -1;

        /* Mutexes */
        pthread_mutex_init(&ctx->ring_mutex,     NULL);
        pthread_mutex_init(&ctx->state_mutex,    NULL);
        pthread_mutex_init(&ctx->snapshot_mutex, NULL);
        pthread_mutex_init(&ctx->files_mutex,    NULL);

        /* Load baseline */
        baseline_load(exp.cfg.baseline_dir, ctx->serial, &ctx->baseline);
        if (ctx->baseline.available && ctx->baseline.valid) {
            ctx->baseline.driver_mismatch =
                (strncmp(ctx->baseline.driver_version,
                         ctx->driver_version,
                         sizeof(ctx->driver_version)) != 0) ? 1 : 0;
            if (ctx->baseline.driver_mismatch)
                log_warn("main: gpu[%d] (%s): baseline driver mismatch "
                         "(%s vs %s)",
                         i, ctx->serial,
                         ctx->baseline.driver_version, ctx->driver_version);
        }

        /* Load probe state */
        probe_load(exp.cfg.state_dir, ctx->serial,
                   exp.cfg.probe_ttl_s, &ctx->probe);

        log_info("main: gpu[%d]: serial=%s model=%s "
                 "pcie_gen_max=%d pcie_width_max=%d "
                 "baseline=%s probe=%s",
                 i, ctx->serial, ctx->gpu_model,
                 ctx->pcie_link_gen_max, ctx->pcie_link_width_max,
                 ctx->baseline.available ?
                     (ctx->baseline.valid ? "valid" : "invalid") :
                     "none",
                 ctx->probe.available ?
                     (ctx->probe.stale ? "stale" : "current") :
                     "none");
    }

    /* Set up inotify on baseline_dir for hot-reload. */
    exp.baseline_inotify_fd = baseline_inotify_init(exp.cfg.baseline_dir);
    if (exp.baseline_inotify_fd < 0)
        log_warn("main: inotify unavailable — baseline hot-reload disabled");

    /* Connect DCGM now that we have gpu_ids. */
    if (exp.dcgm_available == -1) {
        if (dcgm_setup(&exp.dcgm, &exp.dcgm_handle,
                       gpu_ids, exp.num_gpus) == 0) {
            exp.dcgm_available = 1;
            log_info("main: DCGM connected and watching %d GPU(s)",
                     exp.num_gpus);
        } else {
            log_warn("main: DCGM connection failed — running NVML-only mode");
            exp.dcgm_available = 0;
            dcgm_unload(exp.dcgm_dl);
            exp.dcgm_dl = NULL;
            memset(&exp.dcgm, 0, sizeof(exp.dcgm));
        }
    }

    free(gpu_ids);

    /* ------------------------------------------------------------------ */
    /* Spawn poll threads (before fork — threads run in parent only)      */
    /* ------------------------------------------------------------------ */
    for (int i = 0; i < exp.num_gpus; i++) {
        if (collector_start(&exp.gpus[i], &exp) < 0) {
            log_error("main: gpu[%d]: failed to start collector thread", i);
            /* Mark not ready so startup wait doesn't hang. */
            exp.gpus[i].ready = 1;
        }
    }

    /* ------------------------------------------------------------------ */
    /* Set up signal handlers before fork so both parent and child        */
    /* inherit SA_RESETHAND semantics if needed.                          */
    /* ------------------------------------------------------------------ */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* ------------------------------------------------------------------ */
    /* 6 + 7. Socketpair, write init, fork HTTP child                     */
    /* ------------------------------------------------------------------ */
    if (spawn_http_child(&exp) < 0) {
        log_error("main: failed to spawn HTTP child — exiting");
        shutdown_all(&exp);
        return 1;
    }

    /* Parent: drop to minimum privileges. */
    procpriv_parent_setup();

    /* ------------------------------------------------------------------ */
    /* 8. Wait for first poll on all GPUs                                 */
    /* ------------------------------------------------------------------ */
    {
        uint64_t deadline_ms = time_now_ms() +
                               (uint64_t)exp.cfg.nvml_timeout_ms * 2;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L }; /* 100 ms */

        log_info("main: waiting for first poll on all %d GPU(s) "
                 "(timeout %d ms)", exp.num_gpus,
                 exp.cfg.nvml_timeout_ms * 2);

        while (time_now_ms() < deadline_ms && g_running) {
            int all = 1;
            for (int i = 0; i < exp.num_gpus; i++) {
                if (!exp.gpus[i].ready) { all = 0; break; }
            }
            if (all) break;
            nanosleep(&ts, NULL);
        }
    }

    /* ------------------------------------------------------------------ */
    /* 9. Log any GPUs that did not complete first poll                   */
    /* ------------------------------------------------------------------ */
    for (int i = 0; i < exp.num_gpus; i++) {
        if (!exp.gpus[i].ready)
            log_warn("main: gpu[%d] (%s): did not complete first poll "
                     "within startup timeout", i, exp.gpus[i].serial);
    }

    /* ------------------------------------------------------------------ */
    /* 10. sd_notify READY=1                                              */
    /* ------------------------------------------------------------------ */
    sd_notify_ready();

    /* ------------------------------------------------------------------ */
    /* 11. Steady-state monitor loop                                      */
    /*                                                                    */
    /* Responsibilities:                                                   */
    /*   - Watch for HTTP child exit → respawn                            */
    /*   - inotify baseline reload is handled inside the poll threads;    */
    /*     nothing to do here beyond keeping the loop alive.              */
    /* ------------------------------------------------------------------ */
    log_info("main: entering steady-state monitor loop");

    while (g_running) {
        /* Check if the HTTP child is still alive. */
        int status;
        pid_t dead = waitpid(exp.child_pid, &status, WNOHANG);
        if (dead == exp.child_pid) {
            if (WIFEXITED(status))
                log_warn("main: HTTP child (pid %d) exited with status %d",
                         (int)exp.child_pid, WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                log_warn("main: HTTP child (pid %d) killed by signal %d",
                         (int)exp.child_pid, WTERMSIG(status));

            if (g_running) {
                log_info("main: respawning HTTP child");
                if (spawn_http_child(&exp) < 0) {
                    log_error("main: failed to respawn HTTP child — exiting");
                    g_running = 0;
                }
            }
        }

        /* 1-second tick.  EINTR from SIGTERM breaks us out gracefully. */
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
    }

    /* ------------------------------------------------------------------ */
    /* 12. Graceful shutdown                                              */
    /* ------------------------------------------------------------------ */
    shutdown_all(&exp);

    log_info("main: exited cleanly");
    return 0;
}
