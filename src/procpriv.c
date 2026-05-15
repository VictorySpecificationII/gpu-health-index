/*
 * procpriv.c — capability drop and seccomp-BPF filter.
 *
 * Child:  drop all capabilities, then install a seccomp allowlist.
 *         Any syscall not on the list kills the process immediately
 *         (SECCOMP_RET_KILL_PROCESS).
 *
 * Parent: drop all capabilities.  NVML/DCGM polling uses already-open
 *         file descriptors — no capabilities are required after nvmlInit().
 *
 * No libcap dependency.  All capability manipulation is via the raw
 * capset(2) syscall with <linux/capability.h> types.
 *
 * Architecture: x86_64 only.  The BPF filter kills the process on any
 * other audit arch.
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

/*
 * Linux UAPI headers use __u8/__u16 etc. which are not C11-standard names.
 * Suppress -Wpedantic for these includes only.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#pragma GCC diagnostic pop

#include "procpriv.h"
#include "util.h"

/* --------------------------------------------------------------------------
 * SECCOMP_RET_KILL_PROCESS was added in Linux 4.14.
 * Fall back to SECCOMP_RET_KILL (kills the offending thread only) on older
 * kernel headers — the effect is equivalent for a single-thread context.
 * -------------------------------------------------------------------------- */
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS SECCOMP_RET_KILL
#endif

/* --------------------------------------------------------------------------
 * Capability drop — zero all sets, no libcap dependency.
 * -------------------------------------------------------------------------- */

static int drop_all_caps(void) {
    struct __user_cap_header_struct hdr = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid     = 0,
    };
    struct __user_cap_data_struct data[2];
    memset(data, 0, sizeof(data));

    if (syscall(SYS_capset, &hdr, data) < 0) {
        log_warn("procpriv: capset: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Seccomp BPF — HTTP child allowlist.
 *
 * The child needs a narrow set of syscalls.  Entries marked (*) are
 * conservative inclusions for libc/pthread internals; they may be removed
 * after production validation with strace confirms they are unused.
 *
 * Excluded intentionally: execve, execveat, open, openat, fork, vfork,
 * ptrace, kill, tgkill, tkill, unlink, chmod, chown, and all others not
 * listed below.
 * -------------------------------------------------------------------------- */

/*
 * Each ALLOW entry expands to two BPF instructions:
 *   - a JEQ test against the syscall number
 *   - a ALLOW return if the test passed
 * If the test fails, execution falls through to the next ALLOW entry.
 */
#define ALLOW(nr)                                                   \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)(nr), 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

static int install_child_seccomp(void) {
    struct sock_filter filter[] = {
        /* ---- Architecture guard ---------------------------------------- */
        BPF_STMT(BPF_LD  | BPF_W | BPF_ABS,
                 (unsigned int)offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),

        /* ---- Load syscall number --------------------------------------- */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 (unsigned int)offsetof(struct seccomp_data, nr)),

        /* ---- I/O ------------------------------------------------------- */
        ALLOW(__NR_read),
        ALLOW(__NR_write),
        ALLOW(__NR_writev),          /* (*) possible logging path           */
        ALLOW(__NR_close),

        /* ---- Network --------------------------------------------------- */
        ALLOW(__NR_socket),
        ALLOW(__NR_bind),
        ALLOW(__NR_listen),
        ALLOW(__NR_accept),
        ALLOW(__NR_accept4),
        ALLOW(__NR_select),
        ALLOW(__NR_pselect6),        /* (*) some libcs emit pselect6        */
        ALLOW(__NR_setsockopt),
        ALLOW(__NR_getsockopt),      /* (*) conservative                    */
        ALLOW(__NR_sendto),          /* (*) some libcs use sendto on sockets */
        ALLOW(__NR_recvfrom),        /* (*) symmetric conservative inclusion */

        /* ---- Process / thread exit ------------------------------------- */
        ALLOW(__NR_exit),
        ALLOW(__NR_exit_group),

        /* ---- Threading ------------------------------------------------- */
        ALLOW(__NR_futex),
        ALLOW(__NR_clone),
        ALLOW(__NR_set_robust_list), /* (*) pthread_create                  */
        ALLOW(__NR_gettid),          /* (*) pthread internal                */
        ALLOW(__NR_getpid),          /* (*) conservative                    */

        /* ---- Memory management ----------------------------------------- */
        ALLOW(__NR_mmap),
        ALLOW(__NR_munmap),
        ALLOW(__NR_mprotect),        /* (*) pthread stack guard page        */
        ALLOW(__NR_madvise),         /* (*) malloc MADV_DONTNEED on free    */
        ALLOW(__NR_brk),

        /* ---- Signals --------------------------------------------------- */
        ALLOW(__NR_rt_sigaction),
        ALLOW(__NR_rt_sigprocmask),  /* (*) pthread signal mask management  */
        ALLOW(__NR_rt_sigreturn),

        /* ---- Time ------------------------------------------------------ */
        ALLOW(__NR_clock_gettime),
        ALLOW(__NR_gettimeofday),    /* (*) conservative                    */

        /* ---- Entropy (glibc heap randomisation since 2.25) ------------- */
        ALLOW(__NR_getrandom),

#ifdef __NR_clone3
        /* clone3 is used by glibc >= 2.35 for pthread_create             */
        ALLOW(__NR_clone3),
#endif

#ifdef __NR_rseq
        /* rseq is registered per-thread by glibc >= 2.35                 */
        ALLOW(__NR_rseq),
#endif

        /* ---- Default: kill the process --------------------------------- */
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
    };

    struct sock_fprog prog = {
        .len    = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) < 0) {
        log_error("procpriv: child: seccomp filter install failed: %s",
                  strerror(errno));
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Public interface
 * -------------------------------------------------------------------------- */

void procpriv_child_setup(void) {
    /*
     * Order matters:
     *   1. PR_SET_NO_NEW_PRIVS — required before SECCOMP_MODE_FILTER without
     *      CAP_SYS_ADMIN.  Irreversible.
     *   2. capset() to all-zeros — drop before installing the filter so the
     *      filter itself runs without capabilities.
     *   3. seccomp filter — installed last; must allow every syscall the
     *      child will make from this point forward.
     */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        log_warn("procpriv: child: PR_SET_NO_NEW_PRIVS: %s (continuing)",
                 strerror(errno));
    }

    if (drop_all_caps() == 0)
        log_debug("procpriv: child: all capabilities dropped");

    if (install_child_seccomp() < 0) {
        log_error("procpriv: child: seccomp install failed — exiting");
        _exit(1);
    }

    log_info("procpriv: child: seccomp filter active");
}

void procpriv_parent_setup(void) {
    /*
     * NVML and DCGM handles are already open.  Polling is ioctls on those
     * fds — the permission check happened at nvmlInit().  No capabilities
     * are required for the poll loop.  Drop everything.
     *
     * If NVML needs to reconnect after a driver reset, it may fail to
     * re-open /dev/nvidia* without CAP_DAC_OVERRIDE.  That failure will
     * surface as consecutive poll errors and trigger the existing
     * hard-error handling path.
     */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        log_warn("procpriv: parent: PR_SET_NO_NEW_PRIVS: %s (continuing)",
                 strerror(errno));
    }

    if (drop_all_caps() == 0)
        log_debug("procpriv: parent: all capabilities dropped");
    else
        log_warn("procpriv: parent: capability drop failed — "
                 "continuing with PR_SET_NO_NEW_PRIVS only");
}
