                                                                             
                                     
                                                                                
                                                                     
  
                           
                                                                                             
                                                                                
                                                                                      
                                                                                                     
                                                                                                        
                                                                                   
                                                                                   
                                                                                    
                                        
  
                                                                            
                                                                               
                                                                                

#include "lxc_volume_mount.h"
#include "lxc_userns.h"
#include "lxc_volume_guard.h"
#include "lxc_driver.h"
#include "lxc_unit.h"                                                                   

#include "lxc_mountinfo.h"

#include <glib.h>
#include <gio/gio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>                                             
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>                                                                                           
#include <sys/stat.h>                                        
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"                                                       

#define LXC_LOG_DOM "lxc_driver"

                                                                                                               
                                                                                                    
                                                                                             
                                                                                       
                                                               

                                                                                                             
                                                                                        
                                            
                                                                    
static gchar *
_volume_config_rewrite(const gchar *text, gconstpointer data, GError **error)
{
    g_autofree gchar *with_entry = NULL;
    if (data) {
        const PcvLxcConfigEdit add = { "lxc.mount.entry", data, TRUE };
        if (!(with_entry = pcv_lxc_userns_config_apply_edits(text, &add, 1, error)))
            return NULL;
    }
    gchar *hooked = pcv_lxc_config_with_guard_hook(with_entry ? with_entry : text);
    return hooked ? hooked : g_steal_pointer(&with_entry);
}

                                                                             
                                                                   
gboolean
pcv_lxc_volume_config_save(const gchar *lxc_path, const gchar *name, const gchar *mount_value, GError **error)
{
    return pcv_lxc_userns_config_rewrite(lxc_path, name, _volume_config_rewrite, mount_value, error);
}

                                                                                                                                     
                                                                                            
                                                                             
                                                                                                            
                                                              
void
pcv_lxc_volume_hook_ensure(const gchar *lxc_path, const gchar *name)
{
    g_autofree gchar *path = g_build_filename(lxc_path, name, "config", NULL);
    g_autofree gchar *text = NULL, *hooked = NULL;
    g_autoptr(GMutexLocker) cfg_lock = g_mutex_locker_new(pcv_lxc_config_mutex());
    if (!g_file_get_contents(path, &text, NULL, NULL) || !(hooked = pcv_lxc_config_with_guard_hook(text)))
        return;                                              
    GError *err = NULL;
    if (!pcv_lxc_volume_guard_hook_installed(PCV_LXC_VOLUME_GUARD_HOOK))
        PCV_LOG_WARN("container", "volume guard hook not installed: %s — starts outside purecvisor are not blocked for '%s'",
                     PCV_LXC_VOLUME_GUARD_HOOK, name);
    else if (!pcv_lxc_volume_config_save(lxc_path, name, NULL, &err))
        PCV_LOG_WARN("container", "cannot add the volume guard hook to '%s' (starting with the daemon re-check only): %s",
                     name, err->message);
    g_clear_error(&err);
}

                                                                                                         
                                                                                                       
                                                                                                       
                                                                         
                                                                                                      
                                                                                       
                                                                                                      
                                                                                                      
                                                                                                  
                                                                                                
                                                                                        
                                                                                        
                                          
                                                                               
                                   

                                                                                                   
                                                                                                                      
               

                                                 
                                                                                          
GQuark
pcv_lxc_volume_error_quark(void)
{
    return g_quark_from_static_string("pcv-lxc-volume-error-quark");
}

                                                                                  
static gchar *
_real_read_mountinfo(GError **error)
{
    gchar *text = NULL;
    return g_file_get_contents("/proc/self/mountinfo", &text, NULL, error) ? text : NULL;
}

                                                                                                      
                                   
static int
_real_open_dir(const gchar *path, GError **error)
{
    int fd = open(path, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(e), g_strerror(e));
    }
    return fd;
}

                                                                                                    
static gchar *
_real_fd_path(int fd)
{
    g_autofree gchar *link = g_strdup_printf("/proc/self/fd/%d", fd);
    return g_file_read_link(link, NULL);
}

                                                                                                         
                                        
static gboolean
_real_mnt_id(int fd, guint64 *id_out)
{
    struct statx stx;
    if (statx(fd, "", AT_EMPTY_PATH, STATX_MNT_ID, &stx) < 0 || !(stx.stx_mask & STATX_MNT_ID))
        return FALSE;
    *id_out = stx.stx_mnt_id;
    return TRUE;
}

                                                                                                 
                                                   
static gboolean
_real_make_private(int fd, GError **error)
{
    struct mount_attr attr = { .propagation = MS_PRIVATE };
    if (mount_setattr(fd, "", AT_EMPTY_PATH | AT_RECURSIVE, &attr, sizeof attr) < 0) {
        int e = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "mount_setattr private: %s", g_strerror(e));
        return FALSE;
    }
    return TRUE;
}

                                                                                            
static gboolean
_real_umount_fd(int fd, GError **error)
{
    g_autofree gchar *path = g_strdup_printf("/proc/self/fd/%d", fd);
    if (umount2(path, MNT_DETACH) < 0) {
        int e = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "umount: %s", g_strerror(e));
        return FALSE;
    }
    return TRUE;
}

                                                                                         
                                                    
static gboolean
_real_remove_at(int dir_fd, const gchar *name, GError **error)
{
    int rc = unlinkat(dir_fd, name, 0);
    if (rc < 0 && errno == EISDIR)
        rc = unlinkat(dir_fd, name, AT_REMOVEDIR);
    if (rc == 0 || errno == ENOENT)
        return TRUE;
    int e = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "remove %s: %s", name, g_strerror(e));
    return FALSE;
}

                                                                                            
static GPtrArray *
_real_list_at(int dir_fd, GError **error)
{
    int fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
    if (!d) {
        int e = errno;
        if (fd >= 0)
            close(fd);
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "list directory: %s", g_strerror(e));
        return NULL;
    }
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    struct dirent *de;
    int e = 0;
    for (;;) {
        errno = 0;
        if (!(de = readdir(d))) {
            e = errno;
            break;
        }
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
            g_ptr_array_add(out, g_strdup(de->d_name));
    }
    closedir(d);
    if (e != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "list directory: %s", g_strerror(e));
        g_ptr_array_unref(out);
        return NULL;
    }
    return out;
}

                                                                                                
                                                                                           
                                                                                          
                                                                                                                 
                                                                                                       
                                                                               
                                                                                
                                       

                                                                                      
static PcvLxcStorageChildFn G_vol_child_test;

                                                                                             
static int
_stage_mismatch(GError **error, const gchar *what)
{
    g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_STAGING_MISMATCH, what);
    return -1;
}

                                                                                                      
                                        
static gboolean
_stage_owner_ok(const struct stat *st, guint mode, gint64 gid)
{
    const gid_t want = gid > 0 ? (gid_t)gid : getegid();
    return st->st_uid == geteuid() && st->st_gid == want && (guint)(st->st_mode & 07777) == mode;
}

                                                                                                               
                                                                                                               
                                                                                              
                                                              
static int
_real_prepare_dir(int parent_fd, const gchar *name, guint mode, gint64 gid, GError **error)
{
    const gboolean made = mkdirat(parent_fd, name, 0700) == 0;
    if (!made && errno != EEXIST) {
        int e = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "create %s: %s", name, g_strerror(e));
        return -1;
    }
    int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        if (e == ELOOP || e == ENOTDIR)                                                          
            return _stage_mismatch(error, "volume staging directory has unexpected owner or mode");
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "open %s: %s", name, g_strerror(e));
        return -1;
    }
    if (made && gid > 0) {                                                             
        g_autofree gchar *parent = _real_fd_path(parent_fd);
        g_autofree gchar *path = parent ? g_build_filename(parent, name, NULL) : NULL;
        g_autofree gchar *owner = g_strdup_printf("0:%" G_GINT64_FORMAT, gid);
        const gchar *argv[] = { "chown", owner, "--", path, NULL };
        g_autofree gchar *err_out = NULL;
        GError *local = NULL;
        const PcvLxcStorageChildFn child = g_atomic_pointer_get(&G_vol_child_test);
        gboolean ran = path && (child ? child(argv, &err_out, &local)
                                      : pcv_spawn_sync_profile(argv, PCV_CHILD_CAP_STORAGE, NULL, &err_out, &local));
        if (!ran) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "chown failed: %s",
                        !path ? "cannot resolve the staging directory"
                              : local ? local->message : (err_out && *err_out ? g_strstrip(err_out) : "unknown"));
            g_clear_error(&local);
            close(fd);
            return -1;
        }
    }
    if (made && fchmod(fd, mode) < 0) {                                            
        int e = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "chmod %s: %s", name, g_strerror(e));
        close(fd);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISDIR(st.st_mode) || !_stage_owner_ok(&st, mode, gid)) {
        close(fd);
        return _stage_mismatch(error, "volume staging directory has unexpected owner or mode");
    }
    return fd;
}

                                                                                                        
                                                                                   
                                                              
static int
_real_prepare_slot(int dir_fd, const gchar *name, gboolean is_dir, GError **error)
{
    int rc = 0;
    if (is_dir) {
        rc = mkdirat(dir_fd, name, 0700);
    } else {
        int w = openat(dir_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (w < 0)
            rc = -1;
        else
            close(w);
    }
    int fd = rc == 0 || errno == EEXIST ? openat(dir_fd, name, O_PATH | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (fd < 0) {
        int e = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "create %s: %s", name, g_strerror(e));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || (st.st_mode & S_IFMT) != (is_dir ? S_IFDIR : S_IFREG) ||
        !_stage_owner_ok(&st, is_dir ? 0700 : 0600, 0)) {
        close(fd);
        return _stage_mismatch(error, "volume staging slot has unexpected type, owner or mode");
    }
    return fd;
}

static const PcvLxcMountOps G_vol_real_ops = {
    .read_mountinfo = _real_read_mountinfo,
    .open_dir       = _real_open_dir,
    .fd_path        = _real_fd_path,
    .walk           = pcv_lxc_rootfs_walk_dir,
    .open_leaf      = pcv_lxc_rootfs_open_leaf,
    .mnt_id         = _real_mnt_id,
    .make_private   = _real_make_private,
    .umount_fd      = _real_umount_fd,
    .remove_at      = _real_remove_at,
    .list_at        = _real_list_at,
    .source_open    = pcv_lxc_volume_source_open,
    .prepare_dir    = _real_prepare_dir,
    .prepare_slot   = _real_prepare_slot,
    .mount_clone    = pcv_lxc_userns_mount_clone,
};

                                                      
static const PcvLxcMountOps *G_vol_ops_test;
                                                                                                    
static GMutex G_vol_roots_mu;
static gchar *G_vol_volstage_test, *G_vol_runtime_test;

                                                                
static const PcvLxcMountOps *
_mount_ops(void)
{
    const PcvLxcMountOps *ops = g_atomic_pointer_get(&G_vol_ops_test);
    return ops ? ops : &G_vol_real_ops;
}

                                                                                           
static gchar *
_volstage_root(void)
{
    g_mutex_lock(&G_vol_roots_mu);
    gchar *root = g_strdup(G_vol_volstage_test ? G_vol_volstage_test : PCV_LXC_VOLSTAGE_ROOT);
    g_mutex_unlock(&G_vol_roots_mu);
    return root;
}

                                                                                     
static gchar *
_runtime_root(void)
{
    g_mutex_lock(&G_vol_roots_mu);
    gchar *root = g_strdup(G_vol_runtime_test ? G_vol_runtime_test : PCV_LXC_RUNTIME_CONFIG_DIR);
    g_mutex_unlock(&G_vol_roots_mu);
    return root;
}

                                                                        
                                          
                                                            
void
pcv_lxc_volume_set_ops_for_test(const PcvLxcMountOps *ops)
{
    g_atomic_pointer_set(&G_vol_ops_test, ops);
}

                                                                                 
                                      
                                                            
void
pcv_lxc_volume_set_roots_for_test(const gchar *volstage_root, const gchar *runtime_root)
{
    g_mutex_lock(&G_vol_roots_mu);
    g_free(G_vol_volstage_test);
    G_vol_volstage_test = g_strdup(volstage_root);
    g_free(G_vol_runtime_test);
    G_vol_runtime_test = g_strdup(runtime_root);
    g_mutex_unlock(&G_vol_roots_mu);
}

                                                                                           
                                     
                                                            
void
pcv_lxc_volume_set_storage_child_for_test(PcvLxcStorageChildFn fn)
{
    g_atomic_pointer_set(&G_vol_child_test, fn);
}

                                                        
                                          
                                                            
const PcvLxcMountOps *
pcv_lxc_volume_real_ops_for_test(void)
{
    return &G_vol_real_ops;
}

                                                          
                                                         
const PcvLxcMountOps *
pcv_lxc_volume_mount_ops(void)
{
    return _mount_ops();
}

                                                                                               
                                                       
                                                                
static gboolean
_residue_prefix_open(const PcvLxcMountOps *ops, const gchar *path, int *fd_out, gchar **prefix_out, GError **error)
{
    GError *e = NULL;
    *fd_out = -1;
    int fd = ops->open_dir(path, &e);
    if (fd < 0) {
        gboolean missing = g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
        if (!missing)
            g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "cannot open %s: %s",
                        path, e ? e->message : "unknown error");
        g_clear_error(&e);
        return missing;
    }
    gchar *prefix = ops->fd_path(fd);
    if (!prefix || prefix[0] != '/') {
        g_free(prefix);
        close(fd);
        g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "cannot resolve %s", path);
        return FALSE;
    }
    *fd_out = fd;
    *prefix_out = prefix;
    return TRUE;
}

                                                                                     
                                          
static gboolean
_residue_read(const PcvLxcMountOps *ops, gchar **text, PcvMountinfoLine **lines, gsize *n, GError **error)
{
    GError *e = NULL;
    if (!(*text = ops->read_mountinfo(&e))) {
        g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "cannot read the host mount table: %s",
                    e ? e->message : "unknown error");
        g_clear_error(&e);
        return FALSE;
    }
    gsize cap = 1;
    for (const gchar *p = *text; *p; p++)
        cap += *p == '\n';                                              
    *lines = g_new(PcvMountinfoLine, cap);
    *n = pcv_mountinfo_parse(*text, *lines, cap, NULL);
    return TRUE;
}

                                                                                        
                                                          
typedef gint (*VolUnmountPick)(const PcvMountinfoLine *m, const gchar *rel, gconstpointer data);

                                                             
typedef struct {
    guint    done;                
    guint    left;                                       
    gboolean refused;                                             
    gchar   *last_why;                                               
    gchar   *stack_why;                                                                         
} VolUnmountResult;

                                                                 
static void
_result_clear(VolUnmountResult *r)
{
    g_free(r->last_why);
    g_free(r->stack_why);
}

                                                                                           
typedef enum { VOL_STEP_PRIVATE, VOL_STEP_UMOUNT } VolStep;

                                                                        
static gboolean
_ids_has(const GArray *ids, guint64 id)
{
    for (guint k = 0; k < ids->len; k++)
        if (g_array_index(ids, guint64, k) == id)
            return TRUE;
    return FALSE;
}

                                                                                                     
                                                                                                   
                                                                       
                                                        
static gboolean
_unmount_step(const PcvLxcMountOps *ops, int pfd, gsize plen, const PcvMountinfoLine *m, VolStep step, const gchar *what,
              const gchar *name, VolUnmountResult *res)
{
    GError *e = NULL;
    guint64 now_id = 0;
    const gchar *why = NULL;
    int fd = ops->open_leaf(pfd, m->mount_point + plen, &e);                                            
    if (fd < 0)
        why = e ? e->message : "cannot open the mount point";
    else if (!ops->mnt_id(fd, &now_id))
        why = "cannot read the mount id";
    else if (now_id != m->mnt_id)
        why = "the path is now on a different mount";                                          
    else if (step == VOL_STEP_PRIVATE && !ops->make_private(fd, &e))                                            
        why = e ? e->message : "cannot make the mount private";
    else if (step == VOL_STEP_UMOUNT && !ops->umount_fd(fd, &e))                                      
        why = e ? e->message : "umount failed";
    if (fd >= 0)
        close(fd);
    if (!why && step == VOL_STEP_UMOUNT)
        PCV_LOG_INFO(LXC_LOG_DOM, "container '%s': detached %s %s (mount id %" G_GUINT64_FORMAT ")", name, what, m->mount_point,
                     m->mnt_id);
    else if (why) {
        g_free(res->last_why);
        res->last_why = g_strdup(why);
        PCV_LOG_WARN(LXC_LOG_DOM, "container '%s': %s %s (mount id %" G_GUINT64_FORMAT ") not %s: %s", name, what, m->mount_point,
                     m->mnt_id, step == VOL_STEP_PRIVATE ? "made private" : "detached", why);
    }
    g_clear_error(&e);
    return why == NULL;
}

                                                                                                
                                                                      
static void
_stack_refuse(VolUnmountResult *res, GArray *held, const PcvMountinfoLine *v, const gchar *what, const gchar *name)
{
    g_array_append_vals(held, &v->mnt_id, 1);
    if (!res->stack_why)
        res->stack_why = g_strdup_printf(PCV_LXC_VOLUME_STACK_SHARED_FMT, v->mount_point);
    g_free(res->last_why);
    res->last_why = g_strdup(res->stack_why);
    PCV_LOG_WARN(LXC_LOG_DOM, "container '%s': %s %s (mount id %" G_GUINT64_FORMAT ") not detached: %s", name, what, v->mount_point,
                 v->mnt_id, res->stack_why);
}

                                                                                                   
                                                                                                   
                                                                                        
                                                                                       
                                                                                                              
                                                                                           
                                                                                                  
                                                                                          
                                                                                    
                                                                               
                                                       
                                                                                                 
                                                                                                     
                                                                                           
                                                                                                          
                                                            
                                                                                 
static gboolean
_unmount_matching(const PcvLxcMountOps *ops, int pfd, const gchar *prefix, VolUnmountPick pick, gconstpointer data,
                  const gchar *what, const gchar *name, VolUnmountResult *res, GError **error)
{
    const gsize plen = strlen(prefix) > 1 ? strlen(prefix) : 0;                                     
    g_autoptr(GArray) skip = g_array_new(FALSE, FALSE, sizeof(guint64));                                 
    g_autoptr(GArray) done_top = g_array_new(FALSE, FALSE, sizeof(guint64));                                              
    g_autoptr(GArray) held = g_array_new(FALSE, FALSE, sizeof(guint64));                                    
    guint attempts = 0, limit = 0;                                                                         
    for (;;) {
        g_autofree gchar *text = NULL;
        g_autofree PcvMountinfoLine *lines = NULL;
        gsize n = 0, m = 0;
        if (!_residue_read(ops, &text, &lines, &n, error))
            return FALSE;
        g_autofree gsize *idx = g_new(gsize, n + 1);                                    
        g_autofree guint *depth = g_new(guint, n + 1);
        for (gsize i = 0; i < n; i++) {
            if (pcv_mountinfo_last_under(&lines[i], 1, prefix, NULL, 0) != 0)
                continue;                                                             
            const gint p = pick(&lines[i], lines[i].mount_point + plen, data);
            if (p < 0) {
                res->refused = TRUE;                                               
                return TRUE;
            }
            if (p > 0)
                idx[m++] = i;
        }
        res->left = (guint)m;
        if (limit == 0)                                                                       
            limit = (guint)m + PCV_LXC_RESIDUE_MAX_ATTEMPTS;
        if (m == 0 || attempts >= limit)
            break;                                                          
        pcv_mountinfo_chain(lines, idx, m, depth, NULL);                                                     
        for (gsize k = m; k-- > 0;) {                                                           
            gboolean shared_below = FALSE;                                                       
            const PcvMountinfoLine *v = depth[k] ? NULL : &lines[idx[pcv_mountinfo_stack_visible(lines, idx, m, k, &shared_below)]];
            if (depth[k] != 0 || _ids_has(done_top, v->mnt_id) || _ids_has(skip, v->mnt_id) || _ids_has(held, v->mnt_id))
                continue;
            if (shared_below)                                                                         
                _stack_refuse(res, held, v, what, name);
            else if (_unmount_step(ops, pfd, plen, v, VOL_STEP_PRIVATE, what, name, res))
                g_array_append_vals(done_top, &v->mnt_id, 1);
            else
                g_array_append_vals(skip, &v->mnt_id, 1);                                
        }
        gssize best = -1;                                                                  
        for (gsize k = 0; k < m; k++)
            if (pcv_mountinfo_chain_has(lines, idx, m, k, (const guint64 *)(gconstpointer)done_top->data, done_top->len) &&
                !_ids_has(skip, lines[idx[k]].mnt_id) && (best < 0 || depth[k] >= depth[best]))
                best = (gssize)k;
        if (best < 0)
            break;                                                             
        attempts++;
        const guint64 id = lines[idx[best]].mnt_id;
        if (_unmount_step(ops, pfd, plen, &lines[idx[best]], VOL_STEP_UMOUNT, what, name, res)) {
            res->done++;
            g_array_set_size(skip, 0);
            for (guint k = done_top->len; k-- > 0;)                                            
                if (g_array_index(done_top, guint64, k) == id)
                    g_array_remove_index_fast(done_top, k);
        } else {
            g_array_append_val(skip, id);
        }
    }
    return TRUE;
}

                                                                      
                                            
static gint
_pick_any(const PcvMountinfoLine *m, const gchar *rel, gconstpointer data)
{
    (void)m;
    (void)rel;
    (void)data;
    return 1;
}

                                                                                                         
                                                                                
static void
_residue_staging_remove(const PcvLxcMountOps *ops, const gchar *stage_root, const gchar *name, int name_fd)
{
    GError *e = NULL;
    GPtrArray *ents = ops->list_at(name_fd, &e);
    gboolean ok = ents != NULL;
    for (guint i = 0; ok && i < ents->len; i++)
        ok = ops->remove_at(name_fd, g_ptr_array_index(ents, i), &e);
    int root_fd = ok ? ops->open_dir(stage_root, &e) : -1;
    ok = root_fd >= 0 && ops->remove_at(root_fd, name, &e);
    if (root_fd >= 0)
        close(root_fd);
    if (!ok)
        PCV_LOG_WARN(LXC_LOG_DOM, "container '%s': cannot remove the empty volume staging directory: %s", name,
                     e ? e->message : "unknown error");
    g_clear_error(&e);
    if (ents)
        g_ptr_array_unref(ents);
}

                                                                                               
                                                                             
   
gboolean
pcv_lxc_rootfs_residue_clear(const gchar *lxc_path, const gchar *name, guint *cleared, GError **error)
{
                                                                                                                 
                                                                                                   
    const PcvLxcMountOps *ops = _mount_ops();
    guint done = 0, left = 0;
    g_autofree gchar *stack_why = NULL;                                                
    if (cleared)
        *cleared = 0;
    if (!lxc_path || !*lxc_path || !name || !*name || strchr(name, '/') || g_str_equal(name, ".") || g_str_equal(name, "..")) {
        g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "invalid container path or name");
        return FALSE;
    }
    g_autofree gchar *stage_root = _volstage_root();
    g_autofree gchar *rootfs = g_strdup_printf("%s/%s/rootfs", lxc_path, name);
    g_autofree gchar *stage = g_strdup_printf("%s/%s", stage_root, name);
    const gchar *bases[2] = { rootfs, stage };                                         
    gboolean ok = TRUE;
    for (guint b = 0; ok && b < G_N_ELEMENTS(bases); b++) {
        g_autofree gchar *prefix = NULL;
        int pfd = -1;
        if (!(ok = _residue_prefix_open(ops, bases[b], &pfd, &prefix, error)) || pfd < 0)
            continue;                                                           
        VolUnmountResult r = { 0 };                                                             
        ok = _unmount_matching(ops, pfd, prefix, _pick_any, NULL, "leftover host mount", name, &r, error);
        done += r.done;
        if (ok) {
            left += r.left;
            if (b == 1 && r.left == 0)
                _residue_staging_remove(ops, stage_root, name, pfd);
        }
        if (!stack_why)
            stack_why = g_steal_pointer(&r.stack_why);
        _result_clear(&r);
        close(pfd);
    }
    if (cleared)
        *cleared = done;
    if (!ok)
        return FALSE;
    if (left > 0) {
        g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL,
                    "%u volume mount(s) left under the container rootfs could not be removed%s%s", left, stack_why ? "; " : "",
                    stack_why ? stack_why : "");
        return FALSE;
    }
    return TRUE;
}

                                                                                                        
                                                                                                    
                                                                                                 
                                                                                                        
                                                                                                     
                          
                                                                      

                                                                   
static gboolean
_write_all(int fd, const gchar *text)
{
    gsize len = strlen(text), off = 0;
    while (off < len) {
        ssize_t w = write(fd, text + off, len - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return FALSE;
        off += (gsize)w;
    }
    return TRUE;
}

                                                                                                 
                                                                       
static int
_runtime_dir_at(int at, const gchar *name)
{
    if (mkdirat(at, name, 0700) < 0 && errno != EEXIST)
        return -1;
    int fd = openat(at, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    int e = 0;
    if (fstat(fd, &st) < 0)
        e = errno;
    else if (st.st_uid != geteuid())
        e = EPERM;                                                    
    else if (fchmod(fd, 0700) < 0)                                    
        e = errno;
    if (e != 0) {
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

                                                                                                                
                                                                                                           
                                            
                                                        
static gboolean
_runtime_config_write(const gchar *name, const gchar *text, GError **error)
{
    g_autofree gchar *root = _runtime_root();
    g_autofree gchar *parent = g_path_get_dirname(root);
    g_autofree gchar *dir = g_build_filename(root, name, NULL);
    g_autofree gchar *tmp = NULL;
    const gchar *at = parent;                                      
    int rfd = -1, nfd = -1, fd = -1;
    gboolean ok = g_mkdir_with_parents(parent, 0755) == 0;
    if (ok) {
        at = root;
        ok = (rfd = _runtime_dir_at(AT_FDCWD, root)) >= 0;
    }
    if (ok) {
        at = dir;
        ok = (nfd = _runtime_dir_at(rfd, name)) >= 0;
    }
    for (guint i = 0; ok && fd < 0 && i < 8; i++) {                                        
        g_free(tmp);
        tmp = g_strdup_printf(".config.%08x", g_random_int());
        fd = openat(nfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST)
            ok = FALSE;
    }
    ok = ok && fd >= 0 && _write_all(fd, text) && fchmod(fd, 0600) == 0;
    int e = errno;
    if (fd >= 0 && close(fd) < 0 && ok) {
        ok = FALSE;
        e = errno;
    }
    if (ok && renameat(nfd, tmp, nfd, "config") < 0) {
        ok = FALSE;
        e = errno;
    }
    if (!ok && fd >= 0)
        (void)unlinkat(nfd, tmp, 0);                         
    if (!ok)
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "%s: %s", at, g_strerror(e));
    if (nfd >= 0)
        close(nfd);
    if (rfd >= 0)
        close(rfd);
    return ok;
}

                                                                                 
                                    
void
pcv_lxc_runtime_config_remove(const gchar *name)
{
    if (!name || !*name || strchr(name, '/') || g_str_equal(name, ".") || g_str_equal(name, ".."))
        return;
    g_autofree gchar *root = _runtime_root();
    GError *e = NULL;
    int rfd = open(root, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int nfd = rfd >= 0 ? openat(rfd, name, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    gboolean ok = nfd >= 0 || errno == ENOENT;                                 
    if (!ok)
        g_set_error(&e, G_IO_ERROR, g_io_error_from_errno(errno), "open: %s", g_strerror(errno));
    GPtrArray *ents = nfd >= 0 ? _real_list_at(nfd, &e) : NULL;
    if (nfd >= 0)
        ok = ents != NULL;
    for (guint i = 0; ok && ents && i < ents->len; i++)                         
        ok = _real_remove_at(nfd, g_ptr_array_index(ents, i), &e);
    if (ok && nfd >= 0)
        ok = _real_remove_at(rfd, name, &e);
    if (!ok)
        PCV_LOG_WARN(LXC_LOG_DOM, "container '%s': cannot remove the runtime config directory %s/%s: %s", name, root, name,
                     e ? e->message : "unknown error");
    g_clear_error(&e);
    if (ents)
        g_ptr_array_unref(ents);
    if (nfd >= 0)
        close(nfd);
    if (rfd >= 0)
        close(rfd);
}

                                                                                
                                                 
void
pcv_lxc_volume_start_abort(const gchar *lxc_path, const gchar *name)
{
    GError *e = NULL;
    if (!pcv_lxc_rootfs_residue_clear(lxc_path, name, NULL, &e))
        PCV_LOG_WARN(LXC_LOG_DOM, "container '%s': the volume staging of the failed start was not fully removed: %s", name,
                     e ? e->message : "unknown error");
    g_clear_error(&e);
    pcv_lxc_runtime_config_remove(name);
}

                                                                                                     
                                                                                                
static void
_stage_fail(GError **error, GError *e, const gchar *path, const gchar *container_path)
{
    if (g_error_matches(e, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_STAGING_MISMATCH))
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "staging path %s has unexpected type, owner or mode; refusing to start", path);
    else
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot prepare the volume staging for %s: %s", container_path,
                    e ? e->message : "unknown error");
    g_clear_error(&e);
}

                                                                                                                       
                                                                                   
                                           
static gboolean
_stage_dir_open(const PcvLxcMountOps *ops, const gchar *stage_root, const gchar *name, guint mode, gint64 gid,
                const gchar *container_path, int *n_fd, GError **error)
{
    g_autofree gchar *parent = g_path_get_dirname(stage_root), *base = g_path_get_basename(stage_root);
    g_autofree gchar *n_path = g_build_filename(stage_root, name, NULL);
    const gchar *at = stage_root;
    GError *e = NULL;
    int pfd = ops->open_dir(parent, &e), sfd = -1;
    if (pfd >= 0) {
                                                                                                
                                                                   
        static GMutex stage_root_mu;
        g_mutex_lock(&stage_root_mu);
        sfd = ops->prepare_dir(pfd, base, 0711, 0, &e);
        g_mutex_unlock(&stage_root_mu);
        close(pfd);
    }
    if (sfd >= 0) {
        at = n_path;
        *n_fd = ops->prepare_dir(sfd, name, mode, gid, &e);
        close(sfd);
    }
    if (*n_fd >= 0)
        return TRUE;
    _stage_fail(error, e, at, container_path);
    return FALSE;
}

                                                                    
                                                                            
gboolean
pcv_lxc_volume_start_prepare(const gchar *lxc_path, const gchar *name, GError **error)
{
                                                                                                            
                                                                                                          
                                                                                                          
    const PcvLxcMountOps *ops = _mount_ops();
    g_autofree gchar *cfg_path = g_build_filename(lxc_path, name, "config", NULL);
    g_autofree gchar *text = NULL;
    GError *e = NULL;
    if (!g_file_get_contents(cfg_path, &text, NULL, &e)) {                     
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot write the runtime config: %s", e->message);
        g_clear_error(&e);
        return FALSE;
    }
    if (!pcv_lxc_rootfs_residue_clear(lxc_path, name, NULL, &e)) {                                               
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "container '%s' has leftover volume mounts under its rootfs that could not be removed: %s",
                    name, e ? e->message : "unknown error");
        g_clear_error(&e);
        return FALSE;
    }
    gint64 gid = 0;                                                                                        
    const gboolean userns = pcv_lxc_config_root_gid(text, &gid);
    const guint n_mode = userns ? 0710 : 0700;
    if (!userns)
        gid = 0;
    g_autofree gchar *stage_root = _volstage_root();                                  
    g_autofree gchar *rt_text = pcv_lxc_runtime_config_text(text, stage_root, name, &e);
    GPtrArray *lines = rt_text ? pcv_lxc_volstage_lines(text, stage_root, name, &e) : NULL;
    gboolean ok = lines != NULL;
    if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot write the runtime config: %s", e ? e->message : "unknown error");
    g_clear_error(&e);
    int n_fd = -1, root_fd = -1;
    for (guint i = 0; ok && i < lines->len; i++) {                                              
        const PcvLxcVolstageLine *l = lines->pdata[i];
        int src = ops->source_open(l->source, NULL, &e);
        if (src < 0) {
            if (g_error_matches(e, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH)) {                                  
                g_autofree gchar *msg = pcv_lxc_volume_guard_message(l->container_path);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, msg);
                ok = FALSE;
            } else if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {                                              
                PCV_LOG_WARN(LXC_LOG_DOM, "container '%s' volume %s: source not opened, no staging for it: %s", name,
                             l->container_path, e ? e->message : "unknown error");
            }
            g_clear_error(&e);
            continue;
        }
        struct stat sst;
        int slot = -1;
        gboolean nested = FALSE;                                                                  
        const guint flags = (l->readonly ? PCV_LXC_MOUNT_RO : 0) | (l->recursive ? PCV_LXC_MOUNT_RECURSIVE : 0);
        if (fstat(src, &sst) < 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot prepare the volume staging for %s: %s",
                        l->container_path, g_strerror(errno));
            ok = FALSE;
        } else if (!pcv_lxc_volume_nest_check(lines, i, S_ISDIR(sst.st_mode), &nested, &e) ||
                   (!nested && !pcv_lxc_volume_target_ensure(lxc_path, name, &root_fd, l->container_path, S_ISDIR(sst.st_mode), &e))) {
            _stage_fail(error, g_steal_pointer(&e), l->slot, l->container_path);                                           
            ok = FALSE;
        } else if (n_fd < 0 && !_stage_dir_open(ops, stage_root, name, n_mode, gid, l->container_path, &n_fd, error)) {
            ok = FALSE;
        } else if ((slot = ops->prepare_slot(n_fd, strrchr(l->slot, '/') + 1, S_ISDIR(sst.st_mode), &e)) < 0) {
            _stage_fail(error, g_steal_pointer(&e), l->slot, l->container_path);
            ok = FALSE;
        } else if (!ops->mount_clone(src, slot, flags, NULL, &e)) {                               
            _stage_fail(error, g_steal_pointer(&e), l->slot, l->container_path);
            ok = FALSE;
        }
        if (slot >= 0)
            close(slot);
        close(src);
    }
    if (n_fd >= 0)
        close(n_fd);
    if (root_fd >= 0)
        close(root_fd);
    if (ok && !_runtime_config_write(name, rt_text, &e)) {                                                
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot write the runtime config: %s", e->message);
        ok = FALSE;
    }
    g_clear_error(&e);
    if (lines)
        g_ptr_array_unref(lines);
    if (!ok)
        pcv_lxc_volume_start_abort(lxc_path, name);                                                  
    return ok;
}

                                                                                                          
                                                                                                  
                                                                                               
                                                                                          
                                                                                                          
                                                                                                  
                                                                     
                                                                                   
                                        

                                                                                                                 
                            
static void
_volume_line_free(gpointer data)
{
    PcvLxcVolumeLine *l = data;
    g_free((gpointer)l->ident.root);
    g_free(l->dest);
    g_free(l->slot);
    g_free(l);
}

                                                                                                                            
                                              
static gchar *
_dest_strip(const gchar *dest)
{
    return pcv_lxc_volume_dest_canon(dest);
}

                                                                                        
                                  
static PcvMountinfoLine *
_mountinfo_split(gchar *text, gsize *n)
{
    gsize cap = 1;
    for (const gchar *p = text; *p; p++)
        cap += *p == '\n';
    PcvMountinfoLine *lines = g_new(PcvMountinfoLine, cap);
    *n = pcv_mountinfo_parse(text, lines, cap, NULL);
    return lines;
}

                                                                                                        
                                                                                         
                                             
static gboolean
_volume_ident_from_slot(const PcvMountinfoLine *lines, gsize n, const gchar *slot, PcvVolIdent *out, gboolean *ok_out)
{
    const gssize found = pcv_mountinfo_top_at(lines, n, slot);
    if (found < 0)
        return FALSE;
    const gsize cap = strlen(lines[found].root) + 1;
    gchar *root = g_malloc(cap);
    out->major = lines[found].dev_major;
    out->minor = lines[found].dev_minor;
    *ok_out = pcv_mountinfo_path_normalize(lines[found].root, root, cap);                            
    if (*ok_out)
        out->root = root;
    else
        g_free(root);
    return TRUE;
}

                                                                                                      
                                                                           
                                             
static gboolean
_ident_from_fd(const PcvLxcMountOps *ops, const PcvMountinfoLine *lines, gsize n, int fd, const gchar *canon, PcvVolIdent *out)
{
    guint64 id = 0;
    gboolean ok = FALSE;
    if (fd < 0 || !canon || !ops->mnt_id(fd, &id))
        return FALSE;
    for (gsize i = 0; i < n && !ok; i++) {
        if (lines[i].mnt_id != id)                                 
            continue;
        const gsize cap = pcv_vol_ident_root_cap(lines[i].root, canon);                       
        gchar *root = g_malloc(cap);
        ok = pcv_vol_ident_from_mount(lines[i].dev_major, lines[i].dev_minor, lines[i].root, lines[i].mount_point, canon,
                                      root, cap, out);
        if (!ok)
            g_free(root);
    }
    return ok;
}

                                                                                                         
                                              
                                                              
static gboolean
_volume_ident_from_source(const PcvLxcMountOps *ops, const PcvMountinfoLine *lines, gsize n, const gchar *source, PcvVolIdent *out)
{
    GError *e = NULL;
    gchar *canon = NULL;
    int fd = ops->source_open(source, &canon, &e);
    const gboolean ok = _ident_from_fd(ops, lines, n, fd, canon, out);
    if (fd >= 0)
        close(fd);
    g_free(canon);
    g_clear_error(&e);
    return ok;
}

                                                                             
                                                                  
gboolean
pcv_lxc_volume_ident_from_fd(int fd, const gchar *canon, const gchar *host_mountinfo, PcvVolIdent *out)
{
    g_autofree gchar *mi = g_strdup(host_mountinfo ? host_mountinfo : "");                           
    gsize n = 0;
    g_autofree PcvMountinfoLine *lines = _mountinfo_split(mi, &n);
    return _ident_from_fd(_mount_ops(), lines, n, fd, canon, out);
}

                                                         
                                                                         
                                                      
PcvLxcVolumeSet *
pcv_lxc_volume_set_load(const gchar *config_text, const gchar *name, const gchar *host_mountinfo)
{
    if (!name || !*name || strchr(name, '/') || g_str_equal(name, ".") || g_str_equal(name, ".."))
        return NULL;
    const PcvLxcMountOps *ops = _mount_ops();
    g_autofree gchar *stage_root = _volstage_root();
    g_autofree gchar *mi = g_strdup(host_mountinfo ? host_mountinfo : "");                           
    gsize n = 0;
    g_autofree PcvMountinfoLine *lines = _mountinfo_split(mi, &n);
    PcvLxcVolumeSet *set = g_new0(PcvLxcVolumeSet, 1);
    set->entries = pcv_lxc_config_mount_entries(config_text);
    set->lines = g_ptr_array_new_with_free_func(_volume_line_free);
    GHashTable *ordinals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);                     
    for (guint i = 0; i < set->entries->len; i++) {
        const PcvLxcMountEntry *e = set->entries->pdata[i];
        if (!pcv_lxc_mount_entry_is_volume(e))                                                
            continue;
        PcvLxcVolumeLine *l = g_new0(PcvLxcVolumeLine, 1);
        l->entry = e;
        l->dest = _dest_strip(e->dest);
        l->ordinal = GPOINTER_TO_UINT(g_hash_table_lookup(ordinals, l->dest));
        g_hash_table_insert(ordinals, g_strdup(l->dest), GUINT_TO_POINTER(l->ordinal + 1));
        l->slot = pcv_lxc_volstage_slot(stage_root, name, e->dest, l->ordinal);
        if (!_volume_ident_from_slot(lines, n, l->slot, &l->ident, &l->ident_ok))                               
            l->ident_ok = _volume_ident_from_source(ops, lines, n, e->source, &l->ident);
        g_ptr_array_add(set->lines, l);
    }
    g_hash_table_unref(ordinals);
    return set;
}

                                                      
                                                                                     
                                  
void
pcv_lxc_volume_set_free(PcvLxcVolumeSet *set)
{
    if (!set)
        return;
    g_ptr_array_unref(set->lines);
    g_ptr_array_unref(set->entries);
    g_free(set);
}

                                                                                                             
                                                                                                                
                                                                                                        
                                                                                           
                                                                                               
                                                                                                      
                                                                           
                                                                                 
                                                                                 
              

                                                                                          
static gboolean
_name_ok(const gchar *name)
{
    return name && *name && !strchr(name, '/') && !g_str_equal(name, ".") && !g_str_equal(name, "..");
}

                                                                                                   
                                                      
static gchar *
_container_path_of(const gchar *dest)
{
    g_autofree gchar *c = pcv_lxc_volume_dest_canon(dest);
    return g_strconcat("/", c, NULL);
}

                                                                              
typedef struct {
    const PcvVolIdent **idents;
    const char        **dests;
    gboolean           *target;
    gsize               n;
} VolAttrPick;

                                                                                                 
                                                                                              
static gint
_pick_attributed(const PcvMountinfoLine *m, const gchar *rel, gconstpointer data)
{
    const VolAttrPick *a = data;
    const gint at = pcv_vol_attribute(a->idents, a->dests, a->n, m->dev_major, m->dev_minor, m->root, rel);
    if (at == PCV_VOL_ATTR_AMBIGUOUS)
        return -1;
    return at >= 0 && a->target[at] ? 1 : 0;
}

                                                                                        
                                                                           
gboolean
pcv_lxc_volume_unmount_attributed(const gchar *lxc_path, const gchar *name, const PcvLxcVolumeSet *set, const gchar *target_dest,
                                  GError **error)
{
    if (!lxc_path || !*lxc_path || !_name_ok(name) || !set || !set->lines || !target_dest) {
        g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "invalid container path, name or volume set");
        return FALSE;
    }
    const PcvLxcMountOps *ops = _mount_ops();
    g_autofree gchar *rootfs = g_strdup_printf("%s/%s/rootfs", lxc_path, name);
    g_autofree gchar *prefix = NULL;
    int pfd = -1;
    if (!_residue_prefix_open(ops, rootfs, &pfd, &prefix, error))
        return FALSE;
    if (pfd < 0)
        return TRUE;                                                                
    const gsize n = set->lines->len;
    VolAttrPick a = { g_new0(const PcvVolIdent *, n + 1), g_new0(const char *, n + 1), g_new0(gboolean, n + 1), n };
    for (gsize i = 0; i < n; i++) {
        const PcvLxcVolumeLine *l = set->lines->pdata[i];
        a.idents[i] = l->ident_ok && l->ident.root ? &l->ident : NULL;                                    
        a.dests[i] = l->dest;
        a.target[i] = pcv_vol_dest_equal(l->dest, target_dest);
    }
    VolUnmountResult r = { 0 };
    gboolean ok = _unmount_matching(ops, pfd, prefix, _pick_attributed, &a, "host volume mount", name, &r, error);
    close(pfd);
    if (ok && r.refused) {
        g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT,
                            "the same host path is attached twice to this container; stop the container, then detach again");
        ok = FALSE;
    } else if (ok && r.left > 0 && r.stack_why) {                                                        
        g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT, r.stack_why);
        ok = FALSE;
    } else if (ok && r.left > 0) {                                                          
        g_autofree gchar *cpath = _container_path_of(target_dest);
        g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT,
                    "volume is still mounted inside the container: %s; stop the container, then detach again", cpath);
        ok = FALSE;
    }
    _result_clear(&r);
    g_free(a.idents);
    g_free(a.dests);
    g_free(a.target);
    return ok;
}

                                                                                               
                                    
static gint
_pick_mnt_id(const PcvMountinfoLine *m, const gchar *rel, gconstpointer data)
{
    (void)rel;
    return m->mnt_id == *(const guint64 *)data ? 1 : 0;
}

                                                                                                         
                                                                             
gboolean
pcv_lxc_volume_unmount_id(const gchar *lxc_path, const gchar *name, guint64 mnt_id, GError **error)
{
    if (!lxc_path || !*lxc_path || !_name_ok(name)) {
        g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "invalid container path or name");
        return FALSE;
    }
    const PcvLxcMountOps *ops = _mount_ops();
    g_autofree gchar *rootfs = g_strdup_printf("%s/%s/rootfs", lxc_path, name);
    g_autofree gchar *prefix = NULL;
    int pfd = -1;
    if (!_residue_prefix_open(ops, rootfs, &pfd, &prefix, error))
        return FALSE;
    if (pfd < 0)
        return TRUE;                                                          
    VolUnmountResult r = { 0 };
    gboolean ok = _unmount_matching(ops, pfd, prefix, _pick_mnt_id, &mnt_id, "attached volume mount", name, &r, error);
    close(pfd);
    if (ok && r.left > 0) {
        g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, r.last_why ? r.last_why : "the mount is still present");
        ok = FALSE;
    }
    _result_clear(&r);
    return ok;
}

                                                                                                    
                                                
static gint
_pick_stage_slot(const PcvMountinfoLine *m, const gchar *rel, gconstpointer data)
{
    (void)m;
    const gchar *hp = data;                                         
    if (rel[0] != '/' || !g_str_has_prefix(rel + 1, hp))
        return 0;
    const gchar *d = rel + 1 + strlen(hp);
    if (!g_ascii_isdigit(*d))
        return 0;
    while (g_ascii_isdigit(*d))
        d++;
    return *d == '\0' || *d == '/' ? 1 : 0;
}

                                                                                              
                                                  
gboolean
pcv_lxc_volume_staging_unmount(const gchar *name, const gchar *target_dest, GError **error)
{
    if (!_name_ok(name) || !target_dest) {
        g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "invalid container name or path");
        return FALSE;
    }
    const PcvLxcMountOps *ops = _mount_ops();
    g_autofree gchar *stage_root = _volstage_root();
    g_autofree gchar *stage = g_strdup_printf("%s/%s", stage_root, name);
    g_autofree gchar *prefix = NULL;
    int pfd = -1;
    if (!_residue_prefix_open(ops, stage, &pfd, &prefix, error))
        return FALSE;
    if (pfd < 0)
        return TRUE;                                                               
    g_autofree gchar *slot0 = pcv_lxc_volstage_slot(stage_root, name, target_dest, 0);                             
    g_autofree gchar *hp = g_path_get_basename(slot0);
    hp[strlen(hp) - 1] = '\0';                                                                      
    VolUnmountResult r = { 0 };
    gboolean ok;
    ok = _unmount_matching(ops, pfd, prefix, _pick_stage_slot, hp, "staging mount", name, &r, error);
    close(pfd);
    if (ok && r.left > 0) {
        g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "%u staging mount(s) could not be removed: %s", r.left,
                    r.last_why ? r.last_why : "unknown error");
        ok = FALSE;
    }
    _result_clear(&r);
    return ok;
}

                                                                                                      
                        
                                                                                 
PcvLxcVolumeSet *
pcv_lxc_volume_set_load_running(const gchar *lxc_path, const gchar *name, const gchar *config_text, const gchar *target_dest,
                                GError **error)
{
    if (!lxc_path || !*lxc_path || !_name_ok(name) || !target_dest) {
        g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "invalid container path or name");
        return NULL;
    }
    const PcvLxcMountOps *ops = _mount_ops();
    GError *e = NULL;
    g_autofree gchar *mi = ops->read_mountinfo(&e);
    if (!mi) {
        g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "cannot read the host mount table: %s",
                    e ? e->message : "unknown error");
        g_clear_error(&e);
        return NULL;
    }
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load(config_text, name, mi);                            
    for (guint i = 0; i < set->lines->len; i++)
        if (pcv_vol_dest_equal(((const PcvLxcVolumeLine *)set->lines->pdata[i])->dest, target_dest))
            return set;                                                         
    g_autofree gchar *dest = _dest_strip(target_dest);
    if (!*dest)
        return set;                                                        
    g_autofree gchar *rootfs = g_strdup_printf("%s/%s/rootfs", lxc_path, name);
    g_autofree gchar *prefix = NULL;
    int pfd = -1;
    if (!_residue_prefix_open(ops, rootfs, &pfd, &prefix, error)) {
        pcv_lxc_volume_set_free(set);
        return NULL;
    }
    if (pfd < 0)
        return set;                                                                 
    close(pfd);
    g_autofree gchar *want = g_strdup_printf("%s/%s", strlen(prefix) > 1 ? prefix : "", dest);
    gsize n = 0;
    g_autofree PcvMountinfoLine *lines = _mountinfo_split(mi, &n);
    const gssize found = pcv_mountinfo_top_at(lines, n, want);                                
    if (found < 0)
        return set;                                                                      
    PcvLxcVolumeLine *l = g_new0(PcvLxcVolumeLine, 1);                                                      
    l->dest = g_steal_pointer(&dest);
    l->ident.major = lines[found].dev_major;
    l->ident.minor = lines[found].dev_minor;
    const gsize cap = strlen(lines[found].root) + 1;
    gchar *root = g_malloc(cap);
    if (pcv_mountinfo_path_normalize(lines[found].root, root, cap)) {
        l->ident.root = root;
        l->ident_ok = TRUE;
    } else {
        g_free(root);                                                                           
    }
    g_ptr_array_add(set->lines, l);                                                      
    return set;
}
