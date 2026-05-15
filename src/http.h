#ifndef GPU_HEALTH_HTTP_H
#define GPU_HEALTH_HTTP_H

/*
 * http.h — HTTP child process: /metrics /ready /live.
 *
 * Call order in the child process after fork():
 *   1. http_child_tls_init()   [WITH_TLS=1 only] — loads cert/key before seccomp
 *   2. procpriv_child_setup()  — capability drop + seccomp filter
 *   3. http_child_run()        — blocks until SIGTERM or IPC EOF
 */

#include "types.h"

/*
 * http_child_tls_init — load TLS cert/key and seed mbedTLS PRNG.
 *
 * Must be called before procpriv_child_setup() because it opens files.
 * No-op (returns 0) if tls_cert_path is empty.
 * Only compiled when WITH_TLS=1.
 */
#ifdef WITH_TLS
int http_child_tls_init(const gpu_config_t *cfg);
#endif

/*
 * http_child_run — entry point for the HTTP child process.
 *
 * ipc_fd: the child end of the socketpair (parent_fd must already be closed).
 * cfg:    configuration; only listen_addr, listen_port, poll_interval_s,
 *         and (if WITH_TLS) tls_cert_path / tls_key_path are used.
 *
 * Does not return.  Calls exit(1) on fatal errors, exit(0) on clean shutdown.
 */
void http_child_run(int ipc_fd, const gpu_config_t *cfg);

#endif /* GPU_HEALTH_HTTP_H */
