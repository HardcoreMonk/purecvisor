   
                       
                                                    
  
                           
                                                   
                                                    
                                        
  
                                                  
                                                     
                                                   
                                                   
                                               
                                          
  
                                                             
                                                                          
                                           
                                                                  
                                                                   
                                                                        
                                                              
                                                         
                                                 
                                                       
                                                               
                                                                           
  
                                                  
                                              
                             
  
            
                 
                                              
                                                   
                                  
  
              
                               
                                                                                     
                                                                            
                                                                    
  
                                
                                                               
                                                         
                                                
  
                                           
                                                               
                                                         
                                                                         
                                                                          
                                                     
                                                                          
                                                                            
  
            
                                                             
                                                                
                                         
  
                                    
                                                                     
                                                                     
                                                          
  
              
                                                   
                                    
                                              
                                                
  
         
                                                              
                                                                   
                                 
                                                                        
   

#include "pcv_privdrop.h"
#include "pcv_log.h"

#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <linux/securebits.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

                                                                        
#define PD_LOG_DOM "pcv_privdrop"

                                                                   
                                                                 
#define PCV_CAP_SCAN_MAX 63
#define PCV_CAP_BIT(cap) (G_GUINT64_CONSTANT(1) << (cap))

                                                               
static const guint64 PCV_DAEMON_EFFECTIVE_MASK =
    PCV_CAP_BIT(CAP_DAC_OVERRIDE) |
    PCV_CAP_BIT(CAP_SETUID) |
    PCV_CAP_BIT(CAP_NET_BIND_SERVICE) |
    PCV_CAP_BIT(CAP_NET_ADMIN) |
    PCV_CAP_BIT(CAP_SYS_ADMIN);

                                                                  
                                                             
                                            
static const guint64 PCV_SPAWN_CEILING_MASK =
    ((PCV_CAP_BIT(CAP_LAST_CAP + 1) - 1) &
     ~PCV_CAP_BIT(CAP_SYS_MODULE) &
     ~PCV_CAP_BIT(CAP_SYS_RAWIO) &
     ~PCV_CAP_BIT(CAP_SYS_TIME) &
     ~PCV_CAP_BIT(CAP_MAC_OVERRIDE) &
     ~PCV_CAP_BIT(CAP_MAC_ADMIN));

G_STATIC_ASSERT(CAP_LAST_CAP < PCV_CAP_SCAN_MAX);

                                                           
static gint g_child_profiles_enabled = 0;

                                                           
                                                         
guint64
pcv_privdrop_daemon_effective_mask(void)
{
    return PCV_DAEMON_EFFECTIVE_MASK;
}

guint64
pcv_privdrop_spawn_ceiling_mask(void)
{
    return PCV_SPAWN_CEILING_MASK;
}

guint64
pcv_privdrop_child_profile_mask(PcvChildCapabilityProfile profile)
{
    switch (profile) {
    case PCV_CHILD_CAP_STORAGE:
        return PCV_DAEMON_EFFECTIVE_MASK |
               PCV_CAP_BIT(CAP_CHOWN) | PCV_CAP_BIT(CAP_FOWNER) |
               PCV_CAP_BIT(CAP_FSETID);
    case PCV_CHILD_CAP_SIGNAL:
        return PCV_DAEMON_EFFECTIVE_MASK | PCV_CAP_BIT(CAP_KILL);
    case PCV_CHILD_CAP_DHCP:
        return PCV_DAEMON_EFFECTIVE_MASK |
               PCV_CAP_BIT(CAP_CHOWN) | PCV_CAP_BIT(CAP_SETGID) |
               PCV_CAP_BIT(CAP_SETPCAP) | PCV_CAP_BIT(CAP_NET_RAW);
    case PCV_CHILD_CAP_RUNTIME:
        return PCV_SPAWN_CEILING_MASK;
    case PCV_CHILD_CAP_BASE:
    default:
        return PCV_DAEMON_EFFECTIVE_MASK;
    }
}

gboolean
pcv_privdrop_child_profiles_enabled(void)
{
    return g_atomic_int_get(&g_child_profiles_enabled) != 0;
}

                                                         
                            
  
                                                      
                                         
                             
  
      
                                                
                                            
                                                           

#ifdef HAVE_LIBCAP
#include <sys/capability.h>

                                                                 
static gboolean
_apply_cap_masks(guint64 permitted, guint64 effective, guint64 inheritable)
{
    cap_t caps = cap_init();
    if (!caps)
        return FALSE;

    cap_value_t p[64], e[64], i[64];
    int np = 0, ne = 0, ni = 0;
    for (int c = 0; c <= CAP_LAST_CAP; c++) {
        guint64 bit = PCV_CAP_BIT(c);
        if (permitted & bit)   p[np++] = (cap_value_t)c;
        if (effective & bit)   e[ne++] = (cap_value_t)c;
        if (inheritable & bit) i[ni++] = (cap_value_t)c;
    }

    gboolean ok =
        (np == 0 || cap_set_flag(caps, CAP_PERMITTED, np, p, CAP_SET) == 0) &&
        (ne == 0 || cap_set_flag(caps, CAP_EFFECTIVE, ne, e, CAP_SET) == 0) &&
        (ni == 0 || cap_set_flag(caps, CAP_INHERITABLE, ni, i, CAP_SET) == 0) &&
        cap_set_proc(caps) == 0;
    cap_free(caps);
    return ok;
}

                                       
static gboolean
_read_cap_masks(guint64 *permitted, guint64 *effective, guint64 *inheritable)
{
    cap_t caps = cap_get_proc();
    if (!caps)
        return FALSE;

    *permitted = *effective = *inheritable = 0;
    for (int c = 0; c <= CAP_LAST_CAP; c++) {
        cap_flag_value_t value = CAP_CLEAR;
        if (cap_get_flag(caps, (cap_value_t)c, CAP_PERMITTED, &value) != 0)
            goto fail;
        if (value == CAP_SET) *permitted |= PCV_CAP_BIT(c);
        if (cap_get_flag(caps, (cap_value_t)c, CAP_EFFECTIVE, &value) != 0)
            goto fail;
        if (value == CAP_SET) *effective |= PCV_CAP_BIT(c);
        if (cap_get_flag(caps, (cap_value_t)c, CAP_INHERITABLE, &value) != 0)
            goto fail;
        if (value == CAP_SET) *inheritable |= PCV_CAP_BIT(c);
    }
    cap_free(caps);
    return TRUE;

fail:
    cap_free(caps);
    return FALSE;
}

                                                               
static int
_raw_capset(guint64 permitted, guint64 effective, guint64 inheritable)
{
    struct __user_cap_header_struct header = {
        .version = _LINUX_CAPABILITY_VERSION_3,
        .pid = 0,
    };
    struct __user_cap_data_struct data[2] = {
        {
            .effective = (guint32)effective,
            .permitted = (guint32)permitted,
            .inheritable = (guint32)inheritable,
        },
        {
            .effective = (guint32)(effective >> 32),
            .permitted = (guint32)(permitted >> 32),
            .inheritable = (guint32)(inheritable >> 32),
        },
    };
    return (int)syscall(SYS_capset, &header, data);
}

   
                                                                     
  
                                                                      
                                           
   
void
pcv_privdrop_child_setup(gpointer user_data)
{
    PcvChildCapabilityProfile profile =
        (PcvChildCapabilityProfile)GPOINTER_TO_INT(user_data);
    if (profile < PCV_CHILD_CAP_BASE || profile >= PCV_CHILD_CAP_N_PROFILES)
        _exit(126);

    guint64 desired = pcv_privdrop_child_profile_mask(profile);
    guint64 setup = desired | PCV_CAP_BIT(CAP_SETPCAP);

    if (_raw_capset(setup, setup, desired) != 0)
        _exit(126);

    for (int c = 0; c <= PCV_CAP_SCAN_MAX; c++) {
        if (desired & PCV_CAP_BIT(c))
            continue;
        if (syscall(SYS_prctl, PR_CAPBSET_DROP, c, 0, 0, 0) != 0 && errno != EINVAL)
            _exit(126);
    }

                                                                       
                                                                  
    if (profile != PCV_CHILD_CAP_RUNTIME &&
        syscall(SYS_prctl, PR_SET_SECUREBITS,
                SECBIT_NOROOT | SECBIT_NOROOT_LOCKED, 0, 0, 0) != 0)
        _exit(126);

    if (_raw_capset(desired, desired, desired) != 0)
        _exit(126);
    if (syscall(SYS_prctl, PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) != 0)
        _exit(126);
    for (int c = 0; c <= CAP_LAST_CAP; c++) {
        if ((desired & PCV_CAP_BIT(c)) &&
            syscall(SYS_prctl, PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, c, 0, 0) != 0)
            _exit(126);
    }
}

   
                                                       
  
                                                     
                                                        
                                     
                                                                     
                                                   
  
                           
                                                 
                                        
                                                      
  
                  
                                             
                                                      
                                           
                                          
                                                
                                                 
                                                 
                                              
  
                 
                                                                  
                                                              
                                                                        
                                                  
                                                              
                                             
  
                     
                                                                    
                                                                     
                                                                      
                                                                        
                                                               
   
gboolean
pcv_privdrop_capabilities(void)
{
    g_atomic_int_set(&g_child_profiles_enabled, 0);

                                                                    
                                                                            
    guint64 setup_effective = PCV_DAEMON_EFFECTIVE_MASK | PCV_CAP_BIT(CAP_SETPCAP);
    if (!_apply_cap_masks(PCV_SPAWN_CEILING_MASK, setup_effective,
                          PCV_DAEMON_EFFECTIVE_MASK)) {
        PCV_LOG_WARN(PD_LOG_DOM, "capability setup stage failed: %s", strerror(errno));
        return FALSE;
    }

    gint dropped = 0;
    for (int c = 0; c <= PCV_CAP_SCAN_MAX; c++) {
        if (PCV_SPAWN_CEILING_MASK & PCV_CAP_BIT(c))
            continue;
        if (prctl(PR_CAPBSET_DROP, c, 0, 0, 0) == 0) {
            dropped++;
            continue;
        }
        if (errno == EINVAL)
            continue;                                      

        int saved_errno = errno;
        (void)_apply_cap_masks(PCV_DAEMON_EFFECTIVE_MASK,
                              PCV_DAEMON_EFFECTIVE_MASK,
                              PCV_DAEMON_EFFECTIVE_MASK);
        PCV_LOG_WARN(PD_LOG_DOM,
                     "PR_CAPBSET_DROP(%d) failed: %s — child profiles disabled",
                     c, strerror(saved_errno));
        return FALSE;
    }

    if (!_apply_cap_masks(PCV_SPAWN_CEILING_MASK,
                          PCV_DAEMON_EFFECTIVE_MASK,
                          PCV_DAEMON_EFFECTIVE_MASK)) {
        PCV_LOG_WARN(PD_LOG_DOM, "final capability set failed: %s", strerror(errno));
        (void)_apply_cap_masks(PCV_DAEMON_EFFECTIVE_MASK,
                              PCV_DAEMON_EFFECTIVE_MASK,
                              PCV_DAEMON_EFFECTIVE_MASK);
        return FALSE;
    }

    guint64 permitted = 0, effective = 0, inheritable = 0, bounding = 0;
    if (!_read_cap_masks(&permitted, &effective, &inheritable)) {
        PCV_LOG_WARN(PD_LOG_DOM, "capability readback failed: %s", strerror(errno));
        return FALSE;
    }
    for (int c = 0; c <= CAP_LAST_CAP; c++) {
        int value = prctl(PR_CAPBSET_READ, c, 0, 0, 0);
        if (value == 1)
            bounding |= PCV_CAP_BIT(c);
        else if (value < 0 && errno != EINVAL) {
            PCV_LOG_WARN(PD_LOG_DOM, "bounding readback(%d) failed: %s", c, strerror(errno));
            return FALSE;
        }
    }

    if (permitted != PCV_SPAWN_CEILING_MASK ||
        effective != PCV_DAEMON_EFFECTIVE_MASK ||
        inheritable != PCV_DAEMON_EFFECTIVE_MASK ||
        bounding != PCV_SPAWN_CEILING_MASK) {
        PCV_LOG_WARN(PD_LOG_DOM,
                     "capability readback mismatch: P=%016llx E=%016llx I=%016llx B=%016llx",
                     (unsigned long long)permitted, (unsigned long long)effective,
                     (unsigned long long)inheritable, (unsigned long long)bounding);
        return FALSE;
    }

    g_atomic_int_set(&g_child_profiles_enabled, 1);
    PCV_LOG_INFO(PD_LOG_DOM,
                 "PRIVDROP-1 closed: daemon effective=keep-5, spawn ceiling=%016llx, "
                 "bounding dropped=%d, child profiles enabled",
                 (unsigned long long)PCV_SPAWN_CEILING_MASK, dropped);
    return TRUE;
}

#else                                           

   
                      
                           
                                    
                                                     
                                      
   
gboolean
pcv_privdrop_capabilities(void)
{
    g_atomic_int_set(&g_child_profiles_enabled, 0);
    PCV_LOG_WARN(PD_LOG_DOM,
                 "libcap not available — capability restriction skipped "
                 "(install libcap2-dev for hardened builds)");
    return FALSE;
}

void
pcv_privdrop_child_setup(gpointer user_data)
{
    (void)user_data;
    _exit(126);
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

                                                         
                   
  
                                                          
                                      
                    
                                                           

                                                                     
                                                       
                                                     
                                    
                                                 
                                        
void
pcv_privdrop_disable_coredumps(void)
{
    struct rlimit rl = { 0, 0 };
    if (setrlimit(RLIMIT_CORE, &rl) != 0)
        PCV_LOG_WARN(PD_LOG_DOM,
                     "RLIMIT_CORE=0 설정 실패(coredump 잔존 가능): %s", strerror(errno));
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0)
        PCV_LOG_WARN(PD_LOG_DOM,
                     "PR_SET_DUMPABLE=0 설정 실패: %s", strerror(errno));
    PCV_LOG_INFO(PD_LOG_DOM,
                 "coredump 비활성화 완료(RLIMIT_CORE=0 + PR_SET_DUMPABLE=0)");
}

                                                                              
                                                                     
                                                                        
                                                   
                                         
void
pcv_privdrop_enable_coredumps(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_CORE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;                                               
        if (setrlimit(RLIMIT_CORE, &rl) != 0)
            PCV_LOG_WARN(PD_LOG_DOM,
                         "RLIMIT_CORE soft 상향 실패(디버깅 coredump 불가 가능): %s", strerror(errno));
    } else {
        PCV_LOG_WARN(PD_LOG_DOM, "getrlimit(RLIMIT_CORE) 실패: %s", strerror(errno));
    }
    PCV_LOG_INFO(PD_LOG_DOM,
                 "allow_core_dumps=true — coredump soft 한도 상향(디버깅 모드, 시크릿 노출 위험)");
}
