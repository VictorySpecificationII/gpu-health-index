#ifndef GPU_HEALTH_PROCPRIV_H
#define GPU_HEALTH_PROCPRIV_H

/*
 * procpriv.h — capability drop and seccomp-BPF filter installation.
 *
 * Called from main.c in the child and parent branches immediately after fork().
 * Both functions are fatal on PR_SET_NO_NEW_PRIVS failure (child) or log and
 * continue (parent).  Child seccomp failure is fatal (_exit(1)).
 */

/*
 * Called in the child process immediately after fork(), before http_child_run().
 * Sets PR_SET_NO_NEW_PRIVS, drops all capabilities, installs seccomp allowlist.
 */
void procpriv_child_setup(void);

/*
 * Called in the parent process immediately after fork().
 * Sets PR_SET_NO_NEW_PRIVS, drops all capabilities.
 */
void procpriv_parent_setup(void);

#endif /* GPU_HEALTH_PROCPRIV_H */
