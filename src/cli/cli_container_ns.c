   
                           
                                                                                                                                
  
                           
                                                                           
                                                                                                           
                                                                                                                  
                                                                                      
                                                                                                                 
                                                                                                    
                                                                                              
                                                                                                                        
                                                                                                                       
                                                                                                        
                                                                                                               
                                                                                                   
                                                       
                                                                                                        
                                                                                    
                                                                                                                           
                                                                                                                    
                                                                                                                     
                                                                                            
                                                                                                     
                                                                                                                                              
                                                                               
  
                                                                              
                                                                                               
   
#include "cli_container_guard.h"
#include "cli_rpc.h"
#include "modules/lxc/lxc_mountinfo.h"

#include <glib.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>                                   
#include <string.h>
#include <unistd.h>
#include <linux/magic.h>                                                  
#include <linux/openat2.h>                                   
#include <sys/mount.h>                                
#include <sys/stat.h>                                 
#include <sys/statfs.h>                    
#include <sys/syscall.h>                       
#include <sys/wait.h>                      

#define NSD_BUF_MAX     ((gsize)4 * 1024 * 1024)                                           
#define NSD_LINE_MAX    16384                                                        
#define NSD_ATTEMPTS    64                                                                                               
#define NSD_SKIP_MAX    (NSD_LINE_MAX + NSD_ATTEMPTS)                                                       
#define NSD_USAGE "Usage: pcvctl container volume-ns-detach <init_pid> <cgroup> <maj:min> <fs-root> <container_path> " \
                  "[<maj:min> <fs-root> <container_path>]... (daemon internal)\n"

                                                        
                                           
typedef struct {
    int                       procfd;                                         
    gsize                     n;                             
    const PcvVolIdent *const *idents;                                             
    const char *const        *dests;                                         
    const gboolean           *target;                                                  
    char                     *buf;                              
    PcvMountinfoLine         *lines;                         
    guint64                  *skip;                                                                               
    gsize                     n_skip;
} NsdCtx;

                                                                                                         
                                                                            
                                                                                              
                                                                 

                                                                    
                                
static gsize
_nsd_put(char *out, gsize at, const char *s)
{
    gsize n = strlen(s);
    memcpy(out + at, s, n);
    return at + n;
}

                                                                                                           
                                               
static void
_nsd_line(const char *word, guint64 id, int err)
{
    char out[96];                                                                           
    gsize w = _nsd_put(out, 0, word);
    out[w++] = ' ';
    w += pcv_mountinfo_fmt_u64(id, out + w, sizeof out - w);
    if (err > 0) {
        out[w++] = ' ';
        w += pcv_mountinfo_fmt_u64((guint64)err, out + w, sizeof out - w);
    }
    out[w++] = '\n';
    for (gsize off = 0; off < w;) {
        ssize_t r = write(1, out + off, w - off);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        off += (gsize)r;
    }
}

                                                                                                         
                                                                                   
                                                         
static int
_nsd_read(NsdCtx *c, gsize *n_out)
{
    int fd = openat(c->procfd, "thread-self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno;
    gsize len = 0;
    int e = 0;
    for (;;) {
        if (len > NSD_BUF_MAX) {
            e = E2BIG;
            break;
        }
        ssize_t r = read(fd, c->buf + len, NSD_BUF_MAX + 1 - len);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            e = errno;
            break;
        }
        if (r == 0)
            break;
        len += (gsize)r;
    }
    close(fd);
    if (e != 0)
        return e;
    c->buf[len] = '\0';                                                                         
    gboolean truncated = FALSE;
    *n_out = pcv_mountinfo_parse(c->buf, c->lines, NSD_LINE_MAX, &truncated);
    return truncated ? E2BIG : 0;
}

                                                                     
static gboolean
_nsd_skipped(const NsdCtx *c, guint64 id)
{
    for (gsize k = 0; k < c->n_skip; k++)
        if (c->skip[k] == id)
            return TRUE;
    return FALSE;
}

                                                                                                             
                                                                      
static gboolean
_nsd_is_target(const NsdCtx *c, const PcvMountinfoLine *m)
{
    gint a = pcv_vol_attribute(c->idents, c->dests, c->n, m->dev_major, m->dev_minor, m->root, m->mount_point);
    return a >= 0 && c->target[a];
}

                                                                                          
                                        
static gsize
_nsd_count(const NsdCtx *c, gsize n)
{
    gsize k = 0;
    for (gsize i = 0; i < n; i++)
        if (_nsd_is_target(c, &c->lines[i]))
            k++;
    return k;
}

                                                                                                       
                                                                                                  
                                                                                                
                                                                                          
                                                                             
static gssize
_nsd_pick(const NsdCtx *c, gsize n)
{
    for (gsize i = n; i-- > 0;) {
        const PcvMountinfoLine *m = &c->lines[i];
        if (!_nsd_skipped(c, m->mnt_id) && _nsd_is_target(c, m))
            return (gssize)i;
    }
    return -1;
}

                                                                                                        
                                                                                                                   
                                                            
static int
_nsd_detach(const NsdCtx *c, int rootfd, const PcvMountinfoLine *m)
{
    const char *rel = m->mount_point;
    while (*rel == '/')
        rel++;
    if (*rel == '\0')
        return EBUSY;                                                      
    struct open_how how;
    memset(&how, 0, sizeof how);
    how.flags = O_PATH | O_CLOEXEC;
    how.resolve = RESOLVE_NO_SYMLINKS | RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS;
    int fd = (int)syscall(SYS_openat2, rootfd, rel, &how, sizeof how);
    if (fd < 0)
        return errno;
    struct statx stx;
    memset(&stx, 0, sizeof stx);
    int e = 0;
    if (statx(fd, "", AT_EMPTY_PATH, STATX_MNT_ID, &stx) < 0)
        e = errno;
    else if (!(stx.stx_mask & STATX_MNT_ID) || stx.stx_mnt_id != m->mnt_id)
        e = ESTALE;                                                                            
    else if (fchdir(c->procfd) < 0)
        e = errno;
    else {
        char path[48];                                                                 
        gsize w = _nsd_put(path, 0, "thread-self/fd/");
        if (pcv_mountinfo_fmt_u64((guint64)fd, path + w, sizeof path - w) == 0)
            e = ENAMETOOLONG;
        else if (umount2(path, MNT_DETACH) < 0)
            e = errno;
    }
    close(fd);
    return e;
}

                                                                                                 
                                                                                                 
                                                                                       
                                                                                
                                                                
static int
_nsd_in_ns(NsdCtx *c)
{
    int rootfd = openat(AT_FDCWD, "/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (rootfd < 0) {
        _nsd_line("failed", 0, errno);
        return 1;
    }
    int failed = 0;
    guint limit = 0;                                                                                     
    for (guint attempt = 0; limit == 0 || attempt < limit; attempt++) {
        gsize n = 0;
        int e = _nsd_read(c, &n);
        if (e != 0) {
            _nsd_line("failed", 0, e);
            failed = 1;
            break;
        }
        if (limit == 0)                                                                                
            limit = (guint)_nsd_count(c, n) + NSD_ATTEMPTS;
        gssize i = _nsd_pick(c, n);
        if (i < 0)
            break;                                                            
        guint64 id = c->lines[i].mnt_id;
        e = _nsd_detach(c, rootfd, &c->lines[i]);
        if (e == 0) {
            _nsd_line("detached", id, 0);
            c->n_skip = 0;
        } else {
            if (c->n_skip < NSD_SKIP_MAX)                                                                   
                c->skip[c->n_skip++] = id;
            if (e == EINVAL) {
                _nsd_line("locked", id, 0);
            } else {
                _nsd_line("failed", id, e);
                failed = 1;
            }
        }
    }
    close(rootfd);
    return failed;
}

                                                                                                                

                                                                                                       
                                                                                                              
                                        
                                              
static gboolean
_nsd_parse(int argc, char *argv[], guint64 *pid_out, const char **cgroup_out, NsdCtx *c, PcvVolIdent **ids_out)
{
    if (argc < 7 || (argc - 4) % 3 != 0 || !argv[2] || !g_ascii_isdigit(argv[2][0]) ||
        !g_ascii_string_to_unsigned(argv[2], 10, 1, G_MAXINT, pid_out, NULL))
        return FALSE;
    const gsize cl = argv[3] ? strlen(argv[3]) : 0;
    if (cl < 2 || argv[3][0] != '/' || argv[3][cl - 1] == '/')
        return FALSE;                                                                        
    *cgroup_out = argv[3];
    gsize n = (gsize)(argc - 4) / 3;
    PcvVolIdent *ids = g_new0(PcvVolIdent, n);
    const PcvVolIdent **idp = g_new0(const PcvVolIdent *, n);
    const char **dests = g_new0(const char *, n);
    gboolean *target = g_new0(gboolean, n);
    gboolean ok = TRUE;
    for (gsize i = 0; ok && i < n; i++) {
        char **t = &argv[4 + 3 * i];
        ok = pcv_mountinfo_parse_dev(t[0], &ids[i].major, &ids[i].minor) && t[1][0] == '/';
        ids[i].root = t[1];
        idp[i] = &ids[i];
        dests[i] = t[2];
        target[i] = pcv_vol_dest_equal(t[2], argv[6]);                                              
    }
    c->n = n;
    c->idents = idp;
    c->dests = dests;
    c->target = target;
    *ids_out = ids;
    return ok;
}

                                                                                                        
                                                       
                                         
static int
_nsd_read_small(int dirfd, const char *name, gchar **out)
{
    *out = NULL;
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno;
    GString *s = g_string_new(NULL);
    char chunk[4096];
    int e = 0;
    for (;;) {
        ssize_t r = read(fd, chunk, sizeof chunk);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            e = r < 0 ? errno : 0;
            break;
        }
        g_string_append_len(s, chunk, r);
        if (s->len > 64 * 1024) {
            e = E2BIG;
            break;
        }
    }
    close(fd);
    if (e != 0) {
        g_string_free(s, TRUE);
        return e;
    }
    *out = g_string_free(s, FALSE);
    return 0;
}

                                                                                                              
                                                                                            
                                                                                                          
                                                                                          
                                                                                   
                                                    
                                                                                                    
                                                                                  
                             
static int
_nsd_pin_target(int procfd, guint64 pid, const char *cgroup)
{
    char pid_rel[24];
    g_snprintf(pid_rel, sizeof pid_rel, "%" G_GUINT64_FORMAT, pid);
    int piddir = openat(procfd, pid_rel, O_PATH | O_DIRECTORY | O_CLOEXEC);                                   
    if (piddir < 0) {
        g_printerr("pcv volume-ns-detach: cannot open /proc/%s: %s\n", pid_rel, g_strerror(errno));
        return -1;
    }
    g_autofree gchar *cg_text = NULL;
    struct stat a, b;
    int nsfd = -1;
    int e = _nsd_read_small(piddir, "cgroup", &cg_text);
    if (e != 0)
        g_printerr("pcv volume-ns-detach: cannot read /proc/%s/cgroup: %s; refusing to detach\n", pid_rel, g_strerror(e));
    else if (!pcv_proc_cgroup_v2_under(cg_text, cgroup))
        g_printerr("pcv volume-ns-detach: process %s is not in the container cgroup %s; refusing to detach\n", pid_rel, cgroup);
    else if ((nsfd = openat(piddir, "ns/mnt", O_RDONLY | O_CLOEXEC)) < 0)
        g_printerr("pcv volume-ns-detach: cannot open /proc/%s/ns/mnt: %s\n", pid_rel, g_strerror(errno));
    else if (fstat(nsfd, &a) < 0 || fstatat(procfd, "thread-self/ns/mnt", &b, 0) < 0) {
        g_printerr("pcv volume-ns-detach: cannot compare mount namespaces: %s; refusing to detach\n", g_strerror(errno));
        close(nsfd);
        nsfd = -1;
    } else if (a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
        g_printerr("pcv volume-ns-detach: process %s is in this process's own mount namespace (the host); refusing to detach\n", pid_rel);
        close(nsfd);
        nsfd = -1;
    }
    close(piddir);
    return nsfd;
}

                                                                  
static gboolean
_nsd_wait(pid_t child, int *status)
{
    for (;;) {
        if (waitpid(child, status, 0) == child)
            return TRUE;
        if (errno != EINTR)
            return FALSE;
    }
}

                                                                                                                     
                                                                                                       
                                                                            
void
cmd_container_volume_ns_detach(int argc, char *argv[])
{
    NsdCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.procfd = -1;
    PcvVolIdent *ids = NULL;
    guint64 pid = 0;
    const char *cgroup = NULL;
    int nsfd = -1;
    if (!_nsd_parse(argc, argv, &pid, &cgroup, &ctx, &ids)) {
        g_printerr(NSD_USAGE);                                                        
        goto out;
    }
    ctx.procfd = openat(AT_FDCWD, "/proc", O_PATH | O_DIRECTORY | O_CLOEXEC);
    struct statfs sfs;
    if (ctx.procfd < 0 || fstatfs(ctx.procfd, &sfs) < 0 || sfs.f_type != PROC_SUPER_MAGIC) {
        g_printerr("pcv volume-ns-detach: cannot open the host /proc as procfs: %s\n", g_strerror(ctx.procfd < 0 ? errno : ENOTSUP));
        pcv_cli_command_mark_runtime_failure();
        goto out;
    }
    nsfd = _nsd_pin_target(ctx.procfd, pid, cgroup);                                                             
    if (nsfd < 0) {
        pcv_cli_command_mark_runtime_failure();
        goto out;
    }
    ctx.buf = g_malloc(NSD_BUF_MAX + 1);
    ctx.lines = g_new(PcvMountinfoLine, NSD_LINE_MAX);
    ctx.skip = g_new(guint64, NSD_SKIP_MAX);
    pid_t child = fork();
    if (child == 0) {
        int code = 1;
        if (setns(nsfd, CLONE_NEWNS) == 0)
            code = _nsd_in_ns(&ctx);
        else
            _nsd_line("failed", 0, errno);
        _exit(code);
    }
    int status = 0;
    if (child < 0)
        g_printerr("pcv volume-ns-detach: cannot fork: %s\n", g_strerror(errno));
    if (child > 0 && _nsd_wait(child, &status) && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        pcv_cli_command_mark_success();
    else
        pcv_cli_command_mark_runtime_failure();
out:
    if (nsfd >= 0)
        close(nsfd);
    if (ctx.procfd >= 0)
        close(ctx.procfd);
    g_free(ctx.buf);
    g_free(ctx.lines);
    g_free(ctx.skip);
    g_free(ids);
    g_free((gpointer)ctx.idents);
    g_free((gpointer)ctx.dests);
    g_free((gpointer)ctx.target);
}
