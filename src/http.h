#ifndef GPU_HEALTH_HTTP_H
#define GPU_HEALTH_HTTP_H

/*
 * http.h — HTTP child process: /metrics /ready /live.
 *
 * http_child_run is the sole entry point. It must be called in the child
 * process after fork(), after capability drops and seccomp installation
 * (handled by procpriv.c, called from main.c before this).
 *
 * The function reads a gpu_ipc_init_t from ipc_fd to learn num_gpus, spawns
 * an IPC receiver thread to drain gpu_ipc_msg_t messages from the parent,
 * binds the listen port, and runs the HTTP accept loop until SIGTERM or IPC
 * EOF.
 */

#include "types.h"

/*
 * Entry point for the HTTP child process.
 *
 * ipc_fd: the child end of the socketpair (parent_fd must already be closed).
 * cfg:    configuration; only listen_addr, listen_port, and poll_interval_s
 *         are used by the HTTP child.
 *
 * Does not return.  Calls exit(1) on fatal errors (bind failure, bad init
 * message).  Calls exit(0) on clean SIGTERM or IPC EOF.
 */
void http_child_run(int ipc_fd, const gpu_config_t *cfg);

#endif /* GPU_HEALTH_HTTP_H */
