/*
 * procpriv.c — capability drop and seccomp filter (Phase 1 stubs).
 *
 * Full implementation deferred to Phase 2.  Phase 1 sets PR_SET_NO_NEW_PRIVS
 * in both the child and parent to prevent privilege re-escalation via exec.
 *
 * Phase 2 will add:
 *   Child:  capset() to all-zeros, seccomp(SECCOMP_SET_MODE_FILTER, ...)
 *           with a strict whitelist (accept4, read, write, close, select,
 *           socket, bind, listen, sendto, recvfrom, sigaction, exit_group).
 *   Parent: capset() retaining only what NVML polling requires.
 *           Typically no capabilities are needed after the NVML library fd
 *           is already open; remove all and test.
 */

#include <sys/prctl.h>
#include <errno.h>
#include <string.h>

#include "procpriv.h"
#include "util.h"

void procpriv_child_setup(void) {
    /*
     * PR_SET_NO_NEW_PRIVS: once set, execve() cannot gain privileges (setuid
     * bits, file capabilities).  Irreversible.  Required by seccomp FILTER
     * mode without CAP_SYS_ADMIN.
     */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        log_warn("procpriv: child: PR_SET_NO_NEW_PRIVS failed: %s "
                 "(continuing without)", strerror(errno));
    } else {
        log_debug("procpriv: child: PR_SET_NO_NEW_PRIVS set");
    }

    /* Phase 2: capset() all-zeros, then seccomp whitelist filter */
    log_debug("procpriv: child: capability drop and seccomp not implemented "
              "in this build (Phase 2)");
}

void procpriv_parent_setup(void) {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        log_warn("procpriv: parent: PR_SET_NO_NEW_PRIVS failed: %s "
                 "(continuing without)", strerror(errno));
    } else {
        log_debug("procpriv: parent: PR_SET_NO_NEW_PRIVS set");
    }

    /* Phase 2: capset() to minimal set required for NVML poll calls */
    log_debug("procpriv: parent: capability reduction not implemented "
              "in this build (Phase 2)");
}
