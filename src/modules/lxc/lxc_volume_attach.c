                                                                             
                                      
                                                                                                                 
  
                           
                                                                                                      
                                                                                                      
                                                                                                    
                                                                                                 
                                                                                                   
                                                                                
                                                                                            
                                                                                                     
                                                                                                     
                                                                                   
                                                                                                             
                                                                                                    
                                                                                                 
                                                                 
                                                                                                
                                                                                                        
                                                                            
                                                                                        
                                                                              
  
                                                                                       
                                                                                        
                                                                   
                                                                                

#include "lxc_volume_mount.h"
#include "lxc_userns.h"                                                                                                      
#include "lxc_mountinfo.h"                                                                                                     

#include <glib.h>
#include <gio/gio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>                                         
#include <lxc/lxccontainer.h>                                
#include "utils/pcv_log.h"

#define LXC_LOG_DOM "lxc_driver"

                                                                                          
static gboolean
_name_ok(const gchar *name)
{
    return name && *name && !strchr(name, '/') && !g_str_equal(name, ".") && !g_str_equal(name, "..");
}

                                                                                                    
                                                
static gboolean
_volume_ro_inside(const gchar *lxc_path, const gchar *name, const gchar *container_path)
{
    struct lxc_container *c = lxc_container_new(name, lxc_path);
    pid_t pid = (c && c->is_running(c)) ? c->init_pid(c) : -1;
    if (c) lxc_container_put(c);
    gchar *path = g_strdup_printf("/proc/%d/mountinfo", (int)pid);
    gchar *text = NULL;
    gboolean found = FALSE;
    gboolean ro = pid > 0 && g_file_get_contents(path, &text, NULL, NULL) &&
                  pcv_lxc_mountinfo_is_readonly(text, container_path, &found);
    if (!ro)
        PCV_LOG_WARN(LXC_LOG_DOM, "container '%s' %s: read-only check failed (init pid %d, mountinfo %s, mount %s)",
                     name, container_path, (int)pid, text ? "read" : "unreadable", found ? "not ro" : "absent");
    g_free(path);
    g_free(text);
    return ro;
}

                                                                                                                         
                                                                                                    
                                           
static gchar *
_path_tidy(const gchar *container_path)
{
    g_autofree gchar *c = pcv_lxc_volume_dest_canon(container_path);
    return g_strconcat("/", c, NULL);
}

                                                                                                           
                                                                            
static gboolean
_path_inside(const gchar *child, const gchar *parent)
{
    const gsize n = strlen(parent);
    return n > 1 && strncmp(child, parent, n) == 0 && child[n] == '/';
}

                                                                                                                       
                                                                                                           
                                                                                          
static gboolean
_container_path_check(const gchar *container_path, GError **error)
{
    if (!container_path || container_path[0] != '/') {
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH, "container_path must be an absolute path");
        return FALSE;
    }
    g_autofree gchar *norm = _path_tidy(container_path);
    if (g_str_equal(norm, "/")) {
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH, PCV_LXC_VOLUME_ROOT_TARGET_MSG);
        return FALSE;
    }
    return TRUE;
}

                                                                                                        
                                                                                                            
                                                                                              
                                                                                                                
                                                                                                                   
                                                                                                                       
                                                                                                 
                                                                                  
static int
_attach_target_open(const PcvLxcMountOps *ops, int rootfd, const gchar *container_path, gboolean src_is_dir, GError **error)
{
    g_autofree gchar *norm = _path_tidy(container_path);
    const gchar *slash = strrchr(norm, '/');
    if (!slash[1]) {
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH, PCV_LXC_VOLUME_ROOT_TARGET_MSG);
        return -1;
    }
    g_autofree gchar *parent = slash == norm ? g_strdup("/") : g_strndup(norm, (gsize)(slash - norm));
    int pfd = ops->walk(rootfd, parent, TRUE, error);
    if (pfd < 0)
        return -1;
    GError *e = NULL;
    int fd = ops->open_leaf(pfd, slash, &e);
    if (fd < 0 && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {                               
        g_clear_error(&e);
        gboolean made = TRUE;                                                              
        if (src_is_dir) {
            made = pcv_lxc_rootfs_mkdir_at(pfd, slash + 1, &e);
        } else {
            int w = openat(pfd, slash + 1, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
            int er = w < 0 && errno != EEXIST ? errno : 0;
            if (w >= 0 && fchmod(w, 0644) < 0)                                                 
                er = errno;
            if (w >= 0)
                close(w);
            if (er != 0) {
                made = FALSE;
                if (w < 0)
                    g_set_error(&e, G_IO_ERROR, g_io_error_from_errno(er), "create container mount target: %s", g_strerror(er));
                else
                    g_set_error(&e, G_IO_ERROR, g_io_error_from_errno(er), "set container mount target mode: %s", g_strerror(er));
            }
        }
        if (made)
            fd = ops->open_leaf(pfd, slash, &e);
    }
    close(pfd);
    if (fd < 0 && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE)) {                                  
        g_clear_error(&e);
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH, PCV_LXC_VOLUME_TYPE_MISMATCH_MSG);
        return -1;
    }
    if (fd < 0) {
        g_propagate_error(error, e);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int er = errno;
        close(fd);
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(er), "stat container mount target: %s", g_strerror(er));
        return -1;
    }
    const gboolean is_dir = S_ISDIR(st.st_mode) != 0;
    if (is_dir != (src_is_dir != FALSE)) {                                                
        close(fd);
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH, PCV_LXC_VOLUME_TYPE_MISMATCH_MSG);
        return -1;
    }
    return fd;
}

                                                                                                                     
                                                                                                
                                                        
gboolean
pcv_lxc_volume_target_ensure(const gchar *lxc_path, const gchar *name, int *rootfs_fd, const gchar *container_path,
                             gboolean src_is_dir, GError **error)
{
    const PcvLxcMountOps *ops = pcv_lxc_volume_mount_ops();
    if (*rootfs_fd < 0) {
        g_autofree gchar *rootfs = g_strdup_printf("%s/%s/rootfs", lxc_path, name);
        GError *e = NULL;
        if ((*rootfs_fd = ops->open_dir(rootfs, &e)) < 0) {
            const gboolean missing = g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
            if (!missing)
                g_propagate_prefixed_error(error, g_steal_pointer(&e), "open rootfs: ");
            g_clear_error(&e);
            return missing;                                                                          
        }
    }
    int fd = _attach_target_open(ops, *rootfs_fd, container_path, src_is_dir, error);
    if (fd >= 0)
        close(fd);
    return fd >= 0;
}

                                                                                             
                                                                                
gboolean
pcv_lxc_volume_mount_running(const gchar *lxc_path, const gchar *name, int src_fd, const gchar *container_path,
                             gboolean readonly, guint64 *mnt_id_out, GError **error)
{
    const PcvLxcMountOps *ops = pcv_lxc_volume_mount_ops();
    struct stat sst;
    if (!_container_path_check(container_path, error))
        return FALSE;
    if (!lxc_path || !*lxc_path || !_name_ok(name)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid container path or name");
        return FALSE;
    }
    if (fstat(src_fd, &sst) < 0) {
        int er = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(er), "stat volume source: %s", g_strerror(er));
        return FALSE;
    }
    g_autofree gchar *rootfs = g_strdup_printf("%s/%s/rootfs", lxc_path, name);                                          
    GError *e = NULL;
    int rootfd = ops->open_dir(rootfs, &e);
    if (rootfd < 0) {
        g_set_error(error, G_IO_ERROR, e ? e->code : G_IO_ERROR_FAILED, "open rootfs for '%s': %s", name, e ? e->message : "unknown error");
        g_clear_error(&e);
        return FALSE;
    }
    int target_fd = _attach_target_open(ops, rootfd, container_path, S_ISDIR(sst.st_mode), error);
    close(rootfd);
    guint64 id = 0;                                                                                
    gboolean ok = target_fd >= 0 &&
                  ops->mount_clone(src_fd, target_fd, readonly ? PCV_LXC_MOUNT_RO : 0, &id, error);                             
    if (ok && readonly && !_volume_ro_inside(lxc_path, name, container_path)) {
        if (!pcv_lxc_volume_unmount_id(lxc_path, name, id, &e))                           
            PCV_LOG_WARN(LXC_LOG_DOM, "volume %s on '%s' still mounted after failed read-only check: %s",
                         container_path, name, e ? e->message : "unknown error");
        g_clear_error(&e);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "read-only bind mount could not be enforced inside the container");
        ok = FALSE;
    }
    if (target_fd >= 0)
        close(target_fd);
    if (ok && mnt_id_out)
        *mnt_id_out = id;
    return ok;
}

                                                                                                            
                                                                                                                        
                                           
                                                        
static gboolean
_attach_refuse_duplicate(const PcvLxcVolumeSet *set, const gchar *mi, int src_fd, const gchar *canon, GError **error)
{
    PcvVolIdent id = { 0 };
    if (!pcv_lxc_volume_ident_from_fd(src_fd, canon, mi, &id))
        return TRUE;                                                               
    const PcvLxcVolumeLine *dup = NULL;
    for (guint i = 0; set && !dup && i < set->lines->len; i++) {
        const PcvLxcVolumeLine *l = set->lines->pdata[i];
        if (l->ident_ok && l->ident.root && l->ident.major == id.major && l->ident.minor == id.minor &&
            pcv_mountinfo_path_equal(l->ident.root, id.root))
            dup = l;                                                                             
    }
    if (dup)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "host_path is already attached to this container at /%s", dup->dest);
    g_free((gpointer)id.root);
    return dup == NULL;
}

                                                                                                                           
                                                                                                         
                                                                                                                  
                                                                                                              
                                                                                                        
                                                                                           
                                                                                                          
                                                                                             
                                                                            
                                                                                           
                                                                                              
                                                                                        
static gboolean
_attach_refuse_occupied(const PcvLxcMountOps *ops, const gchar *lxc_path, const gchar *name, const PcvLxcVolumeSet *set,
                        const gchar *mi, const gchar *container_path, GError **error)
{
    g_autofree gchar *want = _path_tidy(container_path);
    g_autofree gchar *inside = NULL;                                                
    gboolean occupied = FALSE;
    for (guint i = 0; set && !occupied && i < set->lines->len; i++) {
        g_autofree gchar *line_tidy = _path_tidy(((const PcvLxcVolumeLine *)set->lines->pdata[i])->dest);                   
        occupied = pcv_vol_dest_equal(line_tidy, want);
        if (!occupied && _path_inside(want, line_tidy)) {                                                    
            g_free(inside);
            inside = g_steal_pointer(&line_tidy);
        }
    }
    g_autofree gchar *rootfs = g_strdup_printf("%s/%s/rootfs", lxc_path, name);
    GError *e = NULL;
    int rootfd = occupied ? -1 : ops->open_dir(rootfs, &e);
    g_clear_error(&e);
    if (rootfd >= 0) {
        g_autofree gchar *prefix = ops->fd_path(rootfd);                                                    
        close(rootfd);
        if (!prefix || prefix[0] != '/') {
            g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "cannot resolve %s", rootfs);
            return FALSE;
        }
        const gchar *base = strlen(prefix) > 1 ? prefix : "";
        g_autofree gchar *copy = g_strdup(mi);                                         
        gsize cap = 1;
        for (const gchar *p = copy; *p; p++)
            cap += *p == '\n';
        g_autofree PcvMountinfoLine *lines = g_new(PcvMountinfoLine, cap);
        const gsize n = pcv_mountinfo_parse(copy, lines, cap, NULL);
        g_autofree gchar *mp = g_strconcat(base, want, NULL);
        occupied = pcv_mountinfo_top_at(lines, n, mp) >= 0;                          
        g_autofree gchar *up = g_strdup(want);                                                          
        for (gchar *cut = strrchr(up, '/'); !occupied && !inside && cut && cut != up; cut = strrchr(up, '/')) {
            *cut = '\0';
            g_autofree gchar *anc = g_strconcat(base, up, NULL);
            if (pcv_mountinfo_top_at(lines, n, anc) >= 0)
                inside = g_strdup(up);
        }
    }
    if (occupied)
        g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT, PCV_LXC_VOLUME_OCCUPIED_FMT, want);
    else if (inside)
        g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT, PCV_LXC_VOLUME_INSIDE_FMT, want, inside);
    return !occupied && !inside;
}

                                                                                                     
                                                                                                      
                                                                                              
                                                                                                   
                                                                                                
                                                                                               
                                                                                                                            
                                                                                                              
                                                                                        
                                     

                                                                                                                              
                                                                                                    
                                                                         
                                                                                                    
                                                                                                              
                                                                                                          
                                                              
                                                            
static gint
_nest_outer(const GPtrArray *lines, guint n, gboolean volstage, const gchar *want, gchar **outer_out)
{
    gint at = -1;
    for (guint j = 0; j < n; j++) {
        const PcvLxcVolstageLine *s = volstage ? lines->pdata[j] : NULL;
        const PcvLxcVolumeLine *v = volstage ? NULL : lines->pdata[j];
        if (v && !v->entry)
            continue;
        g_autofree gchar *t = _path_tidy(s ? s->dest : v->dest);
        if (_path_inside(want, t)) {                                                           
            g_free(*outer_out);
            *outer_out = g_steal_pointer(&t);
            at = (gint)j;
        }
    }
    return at;
}

                                                                                                                     
                                                                                                  
                                                                                                                
                                                                                                     
                                                                
static gboolean
_nest_source_check(const PcvLxcMountOps *ops, const gchar *outer_source, const gchar *outer, const gchar *want, gboolean src_is_dir,
                   GError **error)
{
    const gchar *rel = want + strlen(outer);                                                         
    GError *e = NULL;
    const int ofd = ops->source_open(outer_source, NULL, &e);
    const int fd = ofd >= 0 ? ops->open_leaf(ofd, rel, &e) : -1;
    struct stat st;
    int er = 0;
    if (fd >= 0 && fstat(fd, &st) < 0)
        er = errno;
    if (fd >= 0)
        close(fd);
    if (ofd >= 0)
        close(ofd);
    if (fd >= 0 && er == 0 && (S_ISDIR(st.st_mode) != 0) == (src_is_dir != FALSE))
        return TRUE;                                                                        
    if (fd >= 0 && er == 0)
        g_set_error_literal(&e, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE, PCV_LXC_VOLUME_TYPE_MISMATCH_MSG);                  
    else if (fd >= 0)
        g_set_error(&e, G_IO_ERROR, g_io_error_from_errno(er), "stat: %s", g_strerror(er));
    if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) || g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY)) {
        g_set_error(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH, PCV_LXC_VOLUME_NEST_MISSING_FMT, outer, rel + 1);
    } else if (ofd < 0 && g_error_matches(e, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH)) {                           
        g_autofree gchar *msg = pcv_lxc_volume_guard_message(outer);
        g_set_error_literal(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH, msg);
    } else if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE)) {                          
        g_set_error(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH, PCV_LXC_VOLUME_NEST_CTX_FMT, outer, rel + 1,
                    PCV_LXC_VOLUME_TYPE_MISMATCH_MSG);
    } else {                                                                                              
        g_set_error(error, e ? e->domain : G_IO_ERROR, e ? e->code : G_IO_ERROR_FAILED, PCV_LXC_VOLUME_NEST_CTX_FMT, outer, rel + 1,
                    e ? e->message : "unknown error");
    }
    g_clear_error(&e);
    return FALSE;
}

                                                                                                              
                                                                                                                        
                                                            
static gboolean
_attach_refuse_cover(const PcvLxcVolumeSet *set, const gchar *container_path, GError **error)
{
    g_autofree gchar *want = _path_tidy(container_path);
    for (guint i = 0; set && i < set->lines->len; i++) {
        g_autofree gchar *line_tidy = _path_tidy(((const PcvLxcVolumeLine *)set->lines->pdata[i])->dest);
        if (_path_inside(line_tidy, want)) {
            g_set_error(error, PCV_LXC_MOUNT_ERROR, PCV_LXC_MOUNT_ERROR_BAD_PATH, PCV_LXC_VOLUME_COVER_FMT, line_tidy);
            return FALSE;
        }
    }
    return TRUE;
}

                                                                                                                        
                                                                                                  
                                                     
                                                                 
static gboolean
_attach_refuse_nest_missing(const PcvLxcMountOps *ops, const PcvLxcVolumeSet *set, const gchar *container_path, int src_fd,
                            GError **error)
{
    g_autofree gchar *want = _path_tidy(container_path), *outer = NULL;
    const gint at = set ? _nest_outer(set->lines, set->lines->len, FALSE, want, &outer) : -1;
    struct stat sst;
    if (at < 0)
        return TRUE;
    if (fstat(src_fd, &sst) < 0) {
        int er = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(er), "stat volume source: %s", g_strerror(er));
        return FALSE;
    }
    const PcvLxcVolumeLine *l = set->lines->pdata[at];
    return _nest_source_check(ops, l->entry->source, outer, want, S_ISDIR(sst.st_mode), error);
}

                                                                                                             
                                                            
                                                                  
gboolean
pcv_lxc_volume_nest_check(const GPtrArray *lines, guint i, gboolean src_is_dir, gboolean *nested, GError **error)
{
    g_autofree gchar *want = _path_tidy(((const PcvLxcVolstageLine *)lines->pdata[i])->dest), *outer = NULL;
    const gint at = _nest_outer(lines, i, TRUE, want, &outer);
    *nested = at >= 0;
    return at < 0 || _nest_source_check(pcv_lxc_volume_mount_ops(), ((const PcvLxcVolstageLine *)lines->pdata[at])->source, outer, want,
                                        src_is_dir, error);
}

                                                                                                            
                                                                                            
                                                                                      
                                                            
                                                                                          
                        
static gboolean
_attach_precheck(const PcvLxcMountOps *ops, const gchar *lxc_path, const gchar *name, int src_fd, const gchar *canon,
                 const gchar *container_path, gboolean running, GError **error)
{
    g_autofree gchar *cfg = g_build_filename(lxc_path, name, "config", NULL);
    g_autofree gchar *text = NULL, *mi = NULL;
    GError *e = NULL;
    if (!g_file_get_contents(cfg, &text, NULL, &e)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot read the container config: %s", e->message);
        g_clear_error(&e);
        return FALSE;
    }
    if (!(mi = ops->read_mountinfo(&e))) {
        g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "cannot read the host mount table: %s",
                    e ? e->message : "unknown error");
        g_clear_error(&e);
        return FALSE;
    }
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load(text, name, mi);                            
    const gboolean ok = _attach_refuse_duplicate(set, mi, src_fd, canon, error) && _attach_refuse_cover(set, container_path, error) &&
                        (running ? _attach_refuse_occupied(ops, lxc_path, name, set, mi, container_path, error)
                                 : _attach_refuse_nest_missing(ops, set, container_path, src_fd, error));
    pcv_lxc_volume_set_free(set);
    return ok;
}

                                                                        
                                                                                          
                                                      
gboolean
pcv_lxc_volume_attach(const gchar *lxc_path, const gchar *name, int src_fd, const gchar *canon,
                      const gchar *container_path, gboolean readonly, gboolean running, GError **error)
{
    if (!lxc_path || !*lxc_path || !_name_ok(name) || src_fd < 0 || !canon) {
        g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "invalid container or volume source");
        return FALSE;
    }
                                                                                      
    if (!_container_path_check(container_path, error))
        return FALSE;
    if (!_attach_precheck(pcv_lxc_volume_mount_ops(), lxc_path, name, src_fd, canon, container_path, running, error))                       
        return FALSE;
    guint64 new_id = 0;                                                                            
    if (running && !pcv_lxc_volume_mount_running(lxc_path, name, src_fd, container_path, readonly, &new_id, error))
        return FALSE;
                                                                                                       
                                                                                                                 
    g_autofree gchar *dest = pcv_lxc_volume_dest_canon(container_path);
    g_autofree gchar *entry = g_strdup_printf("%s %s none bind%s 0 0", canon, dest, readonly ? ",ro" : "");
    GError *e = NULL, *re = NULL;
    g_autoptr(GMutexLocker) cfg_lock = g_mutex_locker_new(pcv_lxc_config_mutex());                                        
    const gboolean saved = pcv_lxc_volume_config_save(lxc_path, name, entry, &e);
    g_clear_pointer(&cfg_lock, g_mutex_locker_free);
    if (saved)
        return TRUE;
    PCV_LOG_WARN(LXC_LOG_DOM, "container '%s': volume %s was not saved to the config: %s", name, container_path,
                 e ? e->message : "unknown error");
    g_clear_error(&e);
                                                                                
    if (running && !pcv_lxc_volume_unmount_id(lxc_path, name, new_id, &re))
        g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL,
                    "Failed to persist container volume config; the running mount could not be removed: %s",
                    re ? re->message : "unknown error");
    else
        g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "Failed to persist container volume config");
    g_clear_error(&re);
    return FALSE;
}
