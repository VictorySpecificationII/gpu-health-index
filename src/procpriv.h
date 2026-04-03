#ifndef GPU_HEALTH_PROCPRIV_H
#define GPU_HEALTH_PROCPRIV_H

/*
 * procpriv.h — capability drop and seccomp filter installation.
 *
 * Called from main.c in the child and parent branches after fork().
 *
 * Phase 1: stubs that install PR_SET_NO_NEW_PRIVS and log.
 *          Full libcap capability drop and seccomp filter are Phase 2.
 *
 * Build with WITH_CAPS=1 (future) to enable full capability management.
 */

/*
 * Called in the child process immediately after fork(), before
 * http_child_run().
 *
 * Intent: drop all capabilities, then install a seccomp whitelist
 *         permitting only: accept4, read, write, close, select, socket,
 *         bind, listen, sendto, recvfrom, sigaction, exit_group.
 *
 * Phase 1: sets PR_SET_NO_NEW_PRIVS only.
 */
void procpriv_child_setup(void);

/*
 * Called in the parent process immediately after fork().
 *
 * Intent: drop capabilities not needed for NVML polling (retain none;
 *         NVML access is via the already-opened file descriptors).
 *
 * Phase 1: sets PR_SET_NO_NEW_PRIVS only.
 */
void procpriv_parent_setup(void);

#endif /* GPU_HEALTH_PROCPRIV_H */
