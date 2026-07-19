
#include "pcv_privdrop.h"
#include "pcv_log.h"

#include <sys/prctl.h>
#include <errno.h>
#include <string.h>

#define PD_LOG_DOM "pcv_privdrop"

#ifdef HAVE_LIBCAP
#include <sys/capability.h>

gboolean
pcv_privdrop_capabilities(void)
{

    cap_value_t keep[] = {
        CAP_NET_ADMIN,
        CAP_NET_BIND_SERVICE,
        CAP_SYS_ADMIN,
        CAP_SETUID,
        CAP_DAC_OVERRIDE,
    };

    const int nkeep = (int)(sizeof(keep) / sizeof(keep[0]));

    cap_t caps = cap_init();
    if (!caps) {
        PCV_LOG_WARN(PD_LOG_DOM, "cap_init failed: %s", strerror(errno));
        return FALSE;
    }

    if (cap_set_flag(caps, CAP_PERMITTED,   nkeep, keep, CAP_SET) != 0 ||
        cap_set_flag(caps, CAP_EFFECTIVE,   nkeep, keep, CAP_SET) != 0 ||
        cap_set_flag(caps, CAP_INHERITABLE, nkeep, keep, CAP_SET) != 0) {
        PCV_LOG_WARN(PD_LOG_DOM, "cap_set_flag failed: %s", strerror(errno));
        cap_free(caps);
        return FALSE;
    }

    if (cap_set_proc(caps) != 0) {
        PCV_LOG_WARN(PD_LOG_DOM, "cap_set_proc failed: %s", strerror(errno));
        cap_free(caps);
        return FALSE;
    }

    cap_free(caps);

    {
        cap_t verify_caps = cap_get_proc();
        if (verify_caps) {
            cap_flag_value_t val;

            if (cap_get_flag(verify_caps, CAP_SYS_MODULE, CAP_EFFECTIVE, &val) == 0 && val == CAP_SET) {
                PCV_LOG_WARN(PD_LOG_DOM, "CAP_SYS_MODULE still set after drop — security degraded");
            }
            cap_free(verify_caps);
        }
    }

    gint dropped = 0, skipped = 0;
    for (int c = 0; c <= CAP_LAST_CAP; c++) {

        gboolean in_keep = FALSE;
        for (int i = 0; i < nkeep; i++) {
            if (keep[i] == (cap_value_t)c) { in_keep = TRUE; break; }
        }
        if (!in_keep) {
            if (prctl(PR_CAPBSET_DROP, c, 0, 0, 0) == 0)
                dropped++;
            else if (errno != EINVAL)
                skipped++;
        }
    }

    if (skipped > 0)
        PCV_LOG_DEBUG(PD_LOG_DOM,
                      "PR_CAPBSET_DROP: %d dropped, %d skipped (EPERM — sudo env)",
                      dropped, skipped);
    else
        PCV_LOG_DEBUG(PD_LOG_DOM, "PR_CAPBSET_DROP: %d dropped", dropped);

    PCV_LOG_INFO(PD_LOG_DOM,
                 "Capabilities restricted to: NET_ADMIN SYS_ADMIN SETUID DAC_OVERRIDE");
    return TRUE;
}

#else

gboolean
pcv_privdrop_capabilities(void)
{
    PCV_LOG_WARN(PD_LOG_DOM,
                 "libcap not available — capability restriction skipped "
                 "(install libcap2-dev for hardened builds)");
    return FALSE;
}

#endif

gboolean
pcv_privdrop_no_new_privs(void)
{
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        PCV_LOG_WARN(PD_LOG_DOM,
                     "PR_SET_NO_NEW_PRIVS failed: %s", strerror(errno));
        return FALSE;
    }
    PCV_LOG_INFO(PD_LOG_DOM, "PR_SET_NO_NEW_PRIVS=1: privilege escalation via exec blocked");
    return TRUE;
}

#ifdef HAVE_SECCOMP
#include <seccomp.h>

#define SECCOMP_DEFAULT_ACTION  SCMP_ACT_ERRNO(EPERM)

static const int ALLOWED_SYSCALLS[] = {

    SCMP_SYS(read),       SCMP_SYS(write),      SCMP_SYS(close),
    SCMP_SYS(close_range),
    SCMP_SYS(ioctl),      SCMP_SYS(readv),      SCMP_SYS(writev),
    SCMP_SYS(pread64),    SCMP_SYS(pwrite64),
    SCMP_SYS(sendfile),

    SCMP_SYS(epoll_create),  SCMP_SYS(epoll_create1),
    SCMP_SYS(epoll_ctl),     SCMP_SYS(epoll_wait),
    SCMP_SYS(epoll_pwait),   SCMP_SYS(epoll_pwait2),
    SCMP_SYS(poll),          SCMP_SYS(ppoll),
    SCMP_SYS(select),        SCMP_SYS(pselect6),

#ifdef __NR_io_uring_setup
    SCMP_SYS(io_uring_setup),
    SCMP_SYS(io_uring_enter),
    SCMP_SYS(io_uring_register),
#endif

    SCMP_SYS(socket),      SCMP_SYS(socketpair),
    SCMP_SYS(bind),        SCMP_SYS(connect),
    SCMP_SYS(accept),      SCMP_SYS(accept4),     SCMP_SYS(listen),
    SCMP_SYS(sendmsg),     SCMP_SYS(recvmsg),
    SCMP_SYS(sendmmsg),    SCMP_SYS(recvmmsg),
    SCMP_SYS(sendto),      SCMP_SYS(recvfrom),
    SCMP_SYS(getsockopt),  SCMP_SYS(setsockopt),
    SCMP_SYS(getsockname), SCMP_SYS(getpeername),
    SCMP_SYS(shutdown),

    SCMP_SYS(clone),       SCMP_SYS(clone3),
    SCMP_SYS(fork),        SCMP_SYS(vfork),
    SCMP_SYS(execve),      SCMP_SYS(execveat),
    SCMP_SYS(wait4),       SCMP_SYS(waitid),
    SCMP_SYS(exit),        SCMP_SYS(exit_group),
    SCMP_SYS(getpid),      SCMP_SYS(getppid),
    SCMP_SYS(gettid),      SCMP_SYS(set_tid_address),
    SCMP_SYS(pidfd_open),
    SCMP_SYS(pidfd_send_signal),

    SCMP_SYS(getuid),      SCMP_SYS(geteuid),
    SCMP_SYS(getgid),      SCMP_SYS(getegid),
    SCMP_SYS(getgroups),   SCMP_SYS(setgroups),
    SCMP_SYS(setuid),      SCMP_SYS(setgid),
    SCMP_SYS(setreuid),    SCMP_SYS(setregid),
    SCMP_SYS(setresuid),   SCMP_SYS(setresgid),
    SCMP_SYS(getresuid),   SCMP_SYS(getresgid),

    SCMP_SYS(sched_yield),
    SCMP_SYS(sched_getaffinity),  SCMP_SYS(sched_setaffinity),
    SCMP_SYS(sched_getparam),     SCMP_SYS(sched_setparam),
    SCMP_SYS(sched_getscheduler), SCMP_SYS(sched_setscheduler),
    SCMP_SYS(getpriority),        SCMP_SYS(setpriority),

    SCMP_SYS(getrlimit),   SCMP_SYS(setrlimit),   SCMP_SYS(prlimit64),
    SCMP_SYS(getrusage),

    SCMP_SYS(capget),      SCMP_SYS(capset),

    SCMP_SYS(mmap),        SCMP_SYS(mmap2),
    SCMP_SYS(mprotect),    SCMP_SYS(munmap),
    SCMP_SYS(brk),         SCMP_SYS(madvise),
    SCMP_SYS(mremap),      SCMP_SYS(mincore),
    SCMP_SYS(msync),       SCMP_SYS(mlock),       SCMP_SYS(munlock),
    SCMP_SYS(mlockall),    SCMP_SYS(munlockall),
    SCMP_SYS(shmget),      SCMP_SYS(shmat),       SCMP_SYS(shmdt),
    SCMP_SYS(shmctl),
    SCMP_SYS(memfd_create),

    SCMP_SYS(open),        SCMP_SYS(openat),      SCMP_SYS(openat2),
    SCMP_SYS(creat),
    SCMP_SYS(stat),        SCMP_SYS(fstat),       SCMP_SYS(lstat),
    SCMP_SYS(newfstatat),  SCMP_SYS(statx),
    SCMP_SYS(statfs),      SCMP_SYS(fstatfs),
    SCMP_SYS(access),      SCMP_SYS(faccessat),   SCMP_SYS(faccessat2),
    SCMP_SYS(dup),         SCMP_SYS(dup2),        SCMP_SYS(dup3),
    SCMP_SYS(pipe),        SCMP_SYS(pipe2),
    SCMP_SYS(unlink),      SCMP_SYS(unlinkat),
    SCMP_SYS(rename),      SCMP_SYS(renameat),    SCMP_SYS(renameat2),
    SCMP_SYS(mkdir),       SCMP_SYS(mkdirat),
    SCMP_SYS(rmdir),
    SCMP_SYS(chmod),       SCMP_SYS(fchmod),      SCMP_SYS(fchmodat),
    SCMP_SYS(chown),       SCMP_SYS(lchown),      SCMP_SYS(fchown),
    SCMP_SYS(fchownat),
    SCMP_SYS(lseek),
    SCMP_SYS(getdents),    SCMP_SYS(getdents64),
    SCMP_SYS(getcwd),      SCMP_SYS(chdir),       SCMP_SYS(fchdir),
    SCMP_SYS(symlink),     SCMP_SYS(symlinkat),
    SCMP_SYS(readlink),    SCMP_SYS(readlinkat),
    SCMP_SYS(link),        SCMP_SYS(linkat),
    SCMP_SYS(mount),       SCMP_SYS(umount2),
    SCMP_SYS(getxattr),    SCMP_SYS(lgetxattr),   SCMP_SYS(fgetxattr),
    SCMP_SYS(setxattr),    SCMP_SYS(lsetxattr),   SCMP_SYS(fsetxattr),
    SCMP_SYS(listxattr),   SCMP_SYS(llistxattr),

    SCMP_SYS(rt_sigaction),   SCMP_SYS(rt_sigprocmask),
    SCMP_SYS(rt_sigreturn),   SCMP_SYS(rt_sigsuspend),
    SCMP_SYS(rt_sigpending),  SCMP_SYS(rt_sigtimedwait),
    SCMP_SYS(signalfd),       SCMP_SYS(signalfd4),
    SCMP_SYS(kill),           SCMP_SYS(tgkill),
    SCMP_SYS(sigaltstack),

    SCMP_SYS(futex),          SCMP_SYS(futex_time64),
    SCMP_SYS(futex_waitv),
    SCMP_SYS(set_robust_list),SCMP_SYS(get_robust_list),

    SCMP_SYS(clock_gettime),  SCMP_SYS(clock_getres),
    SCMP_SYS(clock_settime),  SCMP_SYS(clock_nanosleep),
    SCMP_SYS(nanosleep),
    SCMP_SYS(gettimeofday),   SCMP_SYS(settimeofday),
    SCMP_SYS(time),           SCMP_SYS(adjtimex),

    SCMP_SYS(prctl),          SCMP_SYS(arch_prctl),
    SCMP_SYS(fcntl),          SCMP_SYS(fcntl64),
    SCMP_SYS(flock),
    SCMP_SYS(fsync),          SCMP_SYS(fdatasync),
    SCMP_SYS(truncate),       SCMP_SYS(ftruncate),
    SCMP_SYS(umask),          SCMP_SYS(uname),
    SCMP_SYS(sysinfo),        SCMP_SYS(times),
    SCMP_SYS(getrandom),
    SCMP_SYS(eventfd),        SCMP_SYS(eventfd2),
    SCMP_SYS(timerfd_create), SCMP_SYS(timerfd_settime),
    SCMP_SYS(timerfd_gettime),
    SCMP_SYS(inotify_init),   SCMP_SYS(inotify_init1),
    SCMP_SYS(inotify_add_watch), SCMP_SYS(inotify_rm_watch),
    SCMP_SYS(splice),         SCMP_SYS(tee),
    SCMP_SYS(copy_file_range),

    SCMP_SYS(rseq),
    SCMP_SYS(set_mempolicy),
    SCMP_SYS(get_mempolicy),
    SCMP_SYS(mbind),
    SCMP_SYS(process_vm_readv),  SCMP_SYS(process_vm_writev),

    SCMP_SYS(getsid),
    SCMP_SYS(setns),
    SCMP_SYS(unshare),
    SCMP_SYS(pivot_root),
    SCMP_SYS(iopl),           SCMP_SYS(ioperm),

    SCMP_SYS(clone3),
    SCMP_SYS(mount),          SCMP_SYS(umount2),
    SCMP_SYS(sethostname),    SCMP_SYS(setdomainname),
    SCMP_SYS(keyctl),         SCMP_SYS(request_key),
    SCMP_SYS(add_key),
    SCMP_SYS(personality),
    SCMP_SYS(capset),         SCMP_SYS(capget),
    SCMP_SYS(seccomp),
};

gboolean
pcv_privdrop_seccomp(void)
{

    scmp_filter_ctx ctx = seccomp_init(SECCOMP_DEFAULT_ACTION);
    if (!ctx) {
        PCV_LOG_WARN(PD_LOG_DOM, "seccomp_init failed");
        return FALSE;
    }

    int n = (int)(sizeof(ALLOWED_SYSCALLS) / sizeof(ALLOWED_SYSCALLS[0]));
    for (int i = 0; i < n; i++) {
        if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW,
                             ALLOWED_SYSCALLS[i], 0) != 0) {

        }
    }

    if (seccomp_load(ctx) != 0) {
        PCV_LOG_WARN(PD_LOG_DOM, "seccomp_load failed: %s", strerror(errno));
        seccomp_release(ctx);
        return FALSE;
    }

    seccomp_release(ctx);
    PCV_LOG_INFO(PD_LOG_DOM,
                 "seccomp-bpf applied: %d syscalls allowed, others → EPERM", n);
    return TRUE;
}

#else

gboolean
pcv_privdrop_seccomp(void)
{
    PCV_LOG_WARN(PD_LOG_DOM,
                 "libseccomp not available — seccomp filter skipped "
                 "(install libseccomp-dev for hardened builds)");
    return FALSE;
}

#endif

void
pcv_privdrop_apply_all(void)
{
    PCV_LOG_INFO(PD_LOG_DOM, "Applying privilege restrictions...");

    gboolean cap_ok  = pcv_privdrop_capabilities();

    gboolean nnp_ok  = TRUE;

    gboolean sec_ok  = FALSE;
    PCV_LOG_INFO(PD_LOG_DOM,
                 "seccomp skipped: BPF filters inherit to child processes "
                 "(lxc-start/systemd), container isolation via lxc.seccomp.profile");

    (void)nnp_ok; (void)sec_ok;
    PCV_LOG_INFO(PD_LOG_DOM,
                 "Privilege drop complete: cap=%s nnp=disabled(LXC-AppArmor) "
                 "seccomp=disabled(LXC-inherit) — host MAC=capabilities+AppArmor(ADR-0026)",
                 cap_ok ? "OK" : "skipped");
}
