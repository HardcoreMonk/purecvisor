                                                                             
                              
                                                                                                                                                    
  
                           
                                                                                           
                                                                                                             
                                                                                           
                                                                                          
                                                                                                            
                                                                                              
                                                                           
                                                                                                
                                                                                               
                                                                               
                                                                                                       
                                                                                                                       
                                                                                                     
                                                                                                 
                                                  
                                                                                                 
                                                   
  
                                                                                  
                                                                             
                                 
                                                                                
#include "modules/lxc/lxc_clone.h"
#include "modules/lxc/lxc_userns.h"                                                                     
#include "modules/lxc/lxc_driver.h"                                                                    
#include "modules/lxc/lxc_volume_guard.h"                                                                
#include "modules/lxc/lxc_volume_mount.h"                                                        
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"                                          
#include "utils/pcv_zfs_exec.h"                                                            

#include <gio/gio.h>
#include <lxc/lxccontainer.h>                                           
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>                                                       
#include <string.h>
#include <unistd.h>
#include <sys/random.h>
#include <sys/stat.h>

#define CLONE_LOG_DOM "container"
#define CLONE_HWADDR_LEN 17
#define CLONE_HWADDR_PREFIX "00:16:3e"
#define CLONE_SNAP_PREFIX "pcv-clone-"                                                                          
#define CLONE_OWNER_FILE "purecvisor.owner"                                               

                                                  
typedef struct {
    const gchar *key;
    gsize        key_len;
    const gchar *value;                            
    gsize        value_len;                          
} CloneLine;

                                                                                     
                                                      
static gboolean
_clone_line_parse(const gchar *line, gsize len, CloneLine *out)
{
    const gchar *p = line, *end = line + len;
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    if (p == end || *p == '#')
        return FALSE;
    const gchar *k = p;
    while (p < end && *p != '=' && *p != ' ' && *p != '\t')
        p++;
    out->key = k;
    out->key_len = (gsize)(p - k);
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    if (p == end || *p != '=' || out->key_len == 0)
        return FALSE;
    p++;
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    out->value = p;
    const gchar *v_end = end;
    while (v_end > p && (v_end[-1] == ' ' || v_end[-1] == '\t' || v_end[-1] == '\r'))
        v_end--;
    out->value_len = (gsize)(v_end - p);
    return TRUE;
}

static gboolean
_clone_key_is(const CloneLine *l, const gchar *key)
{
    return l->key_len == strlen(key) && memcmp(l->key, key, l->key_len) == 0;
}

                                                 
static gboolean
_clone_key_is_hwaddr(const CloneLine *l)
{
    if (_clone_key_is(l, "lxc.network.hwaddr"))
        return TRUE;
    static const gchar pre[] = "lxc.net.", suf[] = ".hwaddr";
    gsize pl = sizeof pre - 1, sl = sizeof suf - 1;
    if (l->key_len <= pl + sl || memcmp(l->key, pre, pl) != 0 || memcmp(l->key + l->key_len - sl, suf, sl) != 0)
        return FALSE;
    for (gsize i = pl; i < l->key_len - sl; i++)
        if (!g_ascii_isdigit(l->key[i]))
            return FALSE;
    return TRUE;
}

                                        
static gboolean
_clone_key_is_path_key(const CloneLine *l)
{
    static const gchar pre[] = "lxc.hook.";
    return _clone_key_is(l, "lxc.mount.fstab") || (l->key_len > sizeof pre - 1 && memcmp(l->key, pre, sizeof pre - 1) == 0);
}

                                                                                 
                                                          
static gchar *
_clone_norm(const gchar *s, gsize len)
{
    if (len == 0 || s[0] != '/')
        return NULL;
    GString *o = g_string_sized_new(len);
    gsize i = 0;
    while (i < len) {
        while (i < len && s[i] == '/')
            i++;
        gsize st = i;
        while (i < len && s[i] != '/')
            i++;
        gsize cl = i - st;
        if (cl == 0 || (cl == 1 && s[st] == '.'))
            continue;
        if (cl == 2 && s[st] == '.' && s[st + 1] == '.') {
            g_string_free(o, TRUE);
            return NULL;
        }
        g_string_append_c(o, '/');
        g_string_append_len(o, s + st, (gssize)cl);
    }
    if (o->len == 0)
        g_string_append_c(o, '/');
    return g_string_free(o, FALSE);
}

                                                                                                                     
                                                                                                                                       
                                               
static gchar *
_clone_swap_prefix(const gchar *val, gsize len, const gchar *const prefixes[2], const gchar *src, const gchar *dst)
{
    for (int pi = 0; pi < 2; pi++) {
        if (!prefixes[pi])
            continue;
        g_autofree gchar *from_dir = g_strdup_printf("%s/%s", prefixes[pi], src);
                                                                                           
        gsize i = 0, comps = 0, want = 0;
        for (const gchar *q = from_dir; *q; q++)
            if (*q == '/' && q[1] && q[1] != '/')
                want++;
        gsize cut = 0;
        while (i < len && comps < want) {
            while (i < len && val[i] == '/')
                i++;
            gsize st = i;
            while (i < len && val[i] != '/')
                i++;
            if (i == st)
                break;
            if (!(i - st == 1 && val[st] == '.'))
                comps++;
            cut = i;
        }
        if (comps != want)
            continue;
        g_autofree gchar *head = _clone_norm(val, cut);
        if (!head || strcmp(head, from_dir) != 0 || cut >= len || val[cut] != '/')
            continue;
        return g_strdup_printf("%s/%s%.*s", prefixes[pi], dst, (int)(len - cut), val + cut);
    }
    return NULL;
}

                                                                                                     
                                                 
static gchar *
_clone_rootfs_value(const gchar *val, gsize len, const gchar *const prefixes[2], const gchar *src, const gchar *dst)
{
    const gchar *dir = "";
    if (len >= 4 && memcmp(val, "dir:", 4) == 0) {
        dir = "dir:";
        val += 4;
        len -= 4;
    }
    for (int pi = 0; pi < 2; pi++) {
        if (!prefixes[pi])
            continue;
        g_autofree gchar *want = g_strdup_printf("%s/%s/rootfs", prefixes[pi], src);
        g_autofree gchar *w = _clone_norm(want, strlen(want));
        g_autofree gchar *got = _clone_norm(val, len);
        if (w && got && strcmp(w, got) == 0)
            return g_strdup_printf("%s%s/%s/rootfs", dir, prefixes[pi], dst);
    }
    return NULL;
}

static void
_clone_fill_random(guint8 *buf, gsize n)
{
    gsize off = 0;
    while (off < n) {
        ssize_t r = getrandom(buf + off, n - off, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {                                                                              
            for (; off < n; off++)
                buf[off] = (guint8)g_random_int_range(0, 256);
            return;
        }
        off += (gsize)r;
    }
}

gchar *
pcv_lxc_clone_config_text(const gchar *text, const gchar *lxc_path, const gchar *lxc_path_canon,
                          const gchar *src, const gchar *dst,
                          void (*rand_bytes)(guint8 *buf, gsize n), GError **error)
{
    if (!text || !lxc_path || !src || !*src || !dst || !*dst) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "clone config rewrite: missing argument");
        return NULL;
    }
    const gchar *const prefixes[2] = { lxc_path, lxc_path_canon };
    if (!rand_bytes)
        rand_bytes = _clone_fill_random;

    GString *out = g_string_sized_new(strlen(text) + 64);
    gboolean rootfs_done = FALSE;
    const gchar *p = text;
    while (*p) {
        const gchar *nl = strchr(p, '\n');
        gsize len = nl ? (gsize)(nl - p) : strlen(p);
        gsize full = nl ? len + 1 : len;                 
        CloneLine l;
        gboolean keep = TRUE;
        if (_clone_line_parse(p, len, &l)) {
            const gchar *vstart = l.value;
            gsize head = (gsize)(vstart - p);                    
            g_autofree gchar *nv = NULL;
            if (_clone_key_is(&l, "lxc.uts.name") || _clone_key_is(&l, "lxc.utsname")) {
                keep = FALSE;                                         
            } else if (_clone_key_is(&l, "lxc.rootfs.path")) {
                nv = _clone_rootfs_value(l.value, l.value_len, prefixes, src, dst);
                if (!nv) {
                    g_string_free(out, TRUE);
                    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "lxc.rootfs.path is not <lxc_path>/%s/rootfs", src);
                    return NULL;
                }
                rootfs_done = TRUE;
            } else if (_clone_key_is_hwaddr(&l)) {
                if (l.value_len == CLONE_HWADDR_LEN) {
                    guint8 b[3] = { 0, 0, 0 };
                    rand_bytes(b, sizeof b);
                    nv = g_strdup_printf(CLONE_HWADDR_PREFIX ":%02x:%02x:%02x", b[0], b[1], b[2]);
                }
            } else if (_clone_key_is_path_key(&l)) {
                nv = _clone_swap_prefix(l.value, l.value_len, prefixes, src, dst);
            }
            if (nv) {                                                                    
                g_string_append_len(out, p, (gssize)head);
                g_string_append(out, nv);
                if (nl)
                    g_string_append_c(out, '\n');
                p += full;
                continue;
            }
        }
        if (keep)
            g_string_append_len(out, p, (gssize)full);
        p += full;
    }
    if (!rootfs_done) {
        g_string_free(out, TRUE);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "lxc.rootfs.path not found in the source config");
        return NULL;
    }
    if (out->len > 0 && out->str[out->len - 1] != '\n')
        g_string_append_c(out, '\n');
    g_string_append_printf(out, "lxc.uts.name = %s\n", dst);
    return g_string_free(out, FALSE);
}

gboolean
pcv_lxc_clone_has_clone_hook(const gchar *text)
{
    if (!text)
        return FALSE;
    const gchar *p = text;
    while (*p) {
        const gchar *nl = strchr(p, '\n');
        gsize len = nl ? (gsize)(nl - p) : strlen(p);
        CloneLine l;
        if (_clone_line_parse(p, len, &l) && _clone_key_is(&l, "lxc.hook.clone"))
            return TRUE;
        p += nl ? len + 1 : len;
    }
    return FALSE;
}

gboolean
pcv_lxc_clone_write_hostname(int rootfs_fd, const gchar *dst)
{
    if (rootfs_fd < 0 || !dst || !*dst)
        return FALSE;
    g_autoptr(GError) werr = NULL;
    int etcfd = pcv_lxc_rootfs_walk_dir(rootfs_fd, "/etc", FALSE, &werr);
    if (etcfd < 0) {
        PCV_LOG_WARN(CLONE_LOG_DOM, "clone: /etc not opened in the new rootfs, hostname left as is (lxc.uts.name sets it at start): %s",
                     werr ? werr->message : "unknown");
        return FALSE;
    }
    gboolean ok = FALSE;
    int fd = openat(etcfd, "hostname", O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        PCV_LOG_WARN(CLONE_LOG_DOM, "clone: /etc/hostname not opened for writing (%s), left as is", g_strerror(errno));
    } else {
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
            PCV_LOG_WARN(CLONE_LOG_DOM, "clone: /etc/hostname is not a plain single-link file, left as is");
        } else if (ftruncate(fd, 0) != 0) {
            PCV_LOG_WARN(CLONE_LOG_DOM, "clone: /etc/hostname truncate failed (%s)", g_strerror(errno));
        } else {
            gsize n = strlen(dst), off = 0;
            ok = TRUE;
            while (off < n) {
                ssize_t w = write(fd, dst + off, n - off);
                if (w < 0 && errno == EINTR)
                    continue;
                if (w <= 0) {
                    PCV_LOG_WARN(CLONE_LOG_DOM, "clone: /etc/hostname write failed (%s)", w < 0 ? g_strerror(errno) : "short write");
                    ok = FALSE;
                    break;
                }
                off += (gsize)w;
            }
        }
        close(fd);
    }
    close(etcfd);
    return ok;
}

                                                                                                    

                                                                                                         
static gboolean
_clone_real_run(const gchar *const *argv, gchar **stdout_out, GError **error)
{
    g_autofree gchar *err_text = NULL;                                   
    return pcv_spawn_sync(argv, stdout_out, &err_text, error);
}

                                                                                                                  
static gboolean
_clone_real_pipe(const gchar *const *producer, const gchar *const *consumer, GError **error)
{
    return pcv_spawn_pipe_sync(producer, consumer, NULL, NULL, error);
}

                                                                                        
                                                                                
static gboolean
_clone_real_is_running(const gchar *name)
{
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (!c)
        return TRUE;
    gboolean running = c->is_defined(c) && c->is_running(c);
    lxc_container_put(c);
    return running;
}

                                                                                                          
static gboolean
_clone_real_dir_exists(const gchar *path)
{
    struct stat st;
    return lstat(path, &st) == 0 || errno != ENOENT;
}

                                                                                  
                                     
static gboolean
_clone_real_residue_clear(const gchar *lxc_path, const gchar *name, GError **error)
{
    return pcv_lxc_rootfs_residue_clear(lxc_path, name, NULL, error);
}

                      
typedef struct { const gchar *lxc_path, *canon, *src, *dst; } CloneRewrite;

                                                                                                               
static gchar *
_clone_rewrite_text(const gchar *text, gconstpointer data, GError **error)
{
    const CloneRewrite *r = data;
    return pcv_lxc_clone_config_text(text, r->lxc_path, r->canon, r->src, r->dst, NULL, error);
}

                                                                                              
                                                                            
                                            
static gboolean
_clone_real_rewrite_config(const gchar *lxc_path, const gchar *dst, const gchar *src, GError **error)
{
    char *rp = realpath(lxc_path, NULL);
    g_autofree gchar *canon = rp ? g_strdup(rp) : NULL;
    free(rp);
    const CloneRewrite r = { lxc_path, canon, src, dst };
    g_autoptr(GMutexLocker) cfg_lock = g_mutex_locker_new(pcv_lxc_config_mutex());
    gboolean ok = pcv_lxc_userns_config_rewrite(lxc_path, dst, _clone_rewrite_text, &r, error);
    g_clear_pointer(&cfg_lock, g_mutex_locker_free);
    return ok;
}

                                                                                                                 
                                               
static gboolean
_clone_real_write_hostname(const gchar *lxc_path, const gchar *dst)
{
    g_autofree gchar *dir = g_strdup_printf("%s/%s", lxc_path, dst);
    int dfd = open(dir, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int rfd = dfd >= 0 ? openat(dfd, "rootfs", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    int e = errno;
    gboolean ok = FALSE;
    if (rfd < 0)
        PCV_LOG_WARN(CLONE_LOG_DOM, "clone: rootfs of '%s' not opened (%s), hostname left as is (lxc.uts.name sets it at start)",
                     dst, g_strerror(e));
    else
        ok = pcv_lxc_clone_write_hostname(rfd, dst);
    if (rfd >= 0)
        close(rfd);
    if (dfd >= 0)
        close(dfd);
    return ok;
}

                                                                                                           
static gboolean
_clone_real_settle(const gchar *lxc_path, const gchar *dst, GError **error)
{
    return pcv_lxc_userns_settle_dirs(lxc_path, dst, error) &&
           pcv_lxc_userns_check_top(lxc_path, dst, error);
}

                                                                                                       
                                                                                                   
                                                                 
static gboolean
_clone_real_product_files(const gchar *lxc_path, const gchar *src, const gchar *dst, GError **error)
{
    g_autofree gchar *dir = g_strdup_printf("%s/%s", lxc_path, dst);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) {
        int e = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "cannot open %s: %s", dir, g_strerror(e));
        return FALSE;
    }
    g_autofree gchar *log0 = g_strdup_printf("%s.log", src);
    g_autofree gchar *log1 = g_strdup_printf("%s.log.1", src);
    const gchar *names[] = { CLONE_OWNER_FILE, log0, log1 };
    gboolean ok = TRUE;
    for (gsize i = 0; ok && i < G_N_ELEMENTS(names); i++) {
        if (unlinkat(dfd, names[i], 0) != 0 && errno != ENOENT) {
            int e = errno;
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "cannot remove %s/%s: %s", dir, names[i], g_strerror(e));
            ok = FALSE;
        }
    }
    close(dfd);
    if (ok && !pcv_lxc_volume_guard_forget(lxc_path, dst)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot remove %s/%s", dir, PCV_LXC_VOLUME_GUARD_VOLGEN);
        ok = FALSE;
    }
    return ok;
}

                                                                                   
static gboolean
_clone_real_destroy_snapshot(const gchar *snapshot, GError **error)
{
    return pcv_zfs_destroy_locked(NULL, snapshot, NULL, error);
}

                                                                                  
                                                                                
static gboolean
_clone_real_destroy_dataset(const gchar *dataset, GError **error)
{
    g_autofree gchar *err_text = NULL;
    GError *e = NULL;
    if (pcv_zfs_destroy_locked("-r", dataset, &err_text, &e))
        return TRUE;
    if (err_text && strstr(err_text, "dataset does not exist")) {
        g_clear_error(&e);
        return TRUE;
    }
    g_propagate_error(error, e);
    return FALSE;
}

                                                                                        
                                                             
static gboolean
_clone_real_remove_dir(const gchar *path, GError **error)
{
    if (rmdir(path) == 0 || errno == ENOENT)
        return TRUE;
    int e = errno;
    g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(e), g_strerror(e));
    return FALSE;
}

                                                                        
static const PcvLxcCloneOps G_clone_real_ops = {
    .run              = _clone_real_run,
    .pipe             = _clone_real_pipe,
    .is_running       = _clone_real_is_running,
    .dir_exists       = _clone_real_dir_exists,
    .residue_clear    = _clone_real_residue_clear,
    .rewrite_config   = _clone_real_rewrite_config,
    .write_hostname   = _clone_real_write_hostname,
    .settle           = _clone_real_settle,
    .product_files    = _clone_real_product_files,
    .stamp_owner      = pcv_lxc_stamp_owner,
    .destroy_snapshot = _clone_real_destroy_snapshot,
    .destroy_dataset  = _clone_real_destroy_dataset,
    .remove_dir       = _clone_real_remove_dir,
};
static PcvLxcCloneOps        G_clone_test_ops;                                     
static const PcvLxcCloneOps *G_clone_ops = &G_clone_real_ops;

                                                                                                
                                                     
                                                            
void
pcv_lxc_clone_set_ops_for_test(const PcvLxcCloneOps *ops)
{
    if (!ops) {
        G_clone_ops = &G_clone_real_ops;
        return;
    }
    G_clone_test_ops = *ops;
#define CLONE_FILL(slot) do { if (!G_clone_test_ops.slot) G_clone_test_ops.slot = G_clone_real_ops.slot; } while (0)
    CLONE_FILL(run);
    CLONE_FILL(pipe);
    CLONE_FILL(is_running);
    CLONE_FILL(dir_exists);
    CLONE_FILL(residue_clear);
    CLONE_FILL(rewrite_config);
    CLONE_FILL(write_hostname);
    CLONE_FILL(settle);
    CLONE_FILL(product_files);
    CLONE_FILL(stamp_owner);
    CLONE_FILL(destroy_snapshot);
    CLONE_FILL(destroy_dataset);
    CLONE_FILL(remove_dir);
#undef CLONE_FILL
    G_clone_ops = &G_clone_test_ops;
}

                                                                              

                                                                                             
                                                  
static gboolean
_clone_name_ok(const gchar *n)
{
    return n && *n && !strchr(n, '/') && !strchr(n, '@') && strcmp(n, ".") != 0 && strcmp(n, "..") != 0;
}

                                                                                                                
                                                                   
                                               
static gboolean
_clone_mount(const PcvLxcCloneOps *ops, const gchar *ds, GError **error)
{
    const gchar *get_argv[] = { "zfs", "get", "-H", "-o", "value", "mounted", ds, NULL };
    g_autofree gchar *out = NULL;
    if (!ops->run(get_argv, &out, error))
        return FALSE;
    const gchar *v = out ? g_strstrip(out) : "";
    if (strcmp(v, "yes") == 0)
        return TRUE;
    if (strcmp(v, "no") != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot read whether %s is mounted (got '%s')", ds, v);
        return FALSE;
    }
    const gchar *mount_argv[] = { "zfs", "mount", ds, NULL };
    return ops->run(mount_argv, NULL, error);
}

                                                                                                     
                                                                                                    
                                                                                      
                                                                              
                                              
                                                                    
static gboolean
_clone_fail(const PcvLxcCloneOps *ops, const gchar *dst, const gchar *stage, GError *cause, const gchar *dst_ds,
            const gchar *dst_dir, const gchar *src_snap, gboolean src_snap_left, GError **error)
{
    GString *left = g_string_new(NULL);
    GError *e = NULL;
    if (!ops->destroy_dataset(dst_ds, &e)) {
        PCV_LOG_WARN(CLONE_LOG_DOM, "clone rollback: destroying %s failed: %s", dst_ds, e ? e->message : "unknown");
        g_string_append(left, dst_ds);
    } else if (!ops->remove_dir(dst_dir, &e)) {
        PCV_LOG_WARN(CLONE_LOG_DOM, "clone rollback: removing %s failed: %s", dst_dir, e ? e->message : "unknown");
        g_string_append_printf(left, "%s: %s", dst_dir, e ? e->message : "unknown error");
    }
    g_clear_error(&e);
    if (src_snap_left && !ops->destroy_snapshot(src_snap, &e)) {
        PCV_LOG_WARN(CLONE_LOG_DOM, "clone rollback: destroying %s failed: %s", src_snap, e ? e->message : "unknown");
        g_string_append_printf(left, "%s%s", left->len ? " and " : "", src_snap);
    }
    g_clear_error(&e);
    const gchar *why = cause ? cause->message : "unknown error";
    if (left->len)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "clone '%s' failed at %s: %s; could not remove %s; remove it manually",
                    dst, stage, why, left->str);
    else
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "clone '%s' failed at %s: %s; clone removed", dst, stage, why);
    g_string_free(left, TRUE);
    g_clear_error(&cause);
    return FALSE;
}

                                                                                                       
                                                                           
gboolean
pcv_lxc_clone_run(const gchar *lxc_path, const gchar *zfs_base, const gchar *src, const gchar *dst,
                  const gchar *subject, gboolean src_unpriv, GError **error)
{
    if (!lxc_path || !*lxc_path || !zfs_base || !*zfs_base || !_clone_name_ok(src) || !_clone_name_ok(dst) ||
        strcmp(src, dst) == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "clone: invalid source or target name");
        return FALSE;
    }
    const PcvLxcCloneOps *ops = G_clone_ops;
    g_autofree gchar *dst_dir = g_strdup_printf("%s/%s", lxc_path, dst);
    g_autofree gchar *dst_ds = g_strdup_printf("%s/%s", zfs_base, dst);
    g_autofree gchar *src_snap = g_strdup_printf("%s/%s@" CLONE_SNAP_PREFIX "%s", zfs_base, src, dst);
    g_autofree gchar *dst_snap = g_strdup_printf("%s@" CLONE_SNAP_PREFIX "%s", dst_ds, dst);
    GError *err = NULL;

                               
    if (ops->is_running(src)) {                                                                   
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "stop the source container before cloning");
        return FALSE;
    }
    const gchar *list_argv[] = { "zfs", "list", "-H", "-o", "name", dst_ds, NULL };
    if (ops->dir_exists(dst_dir) || ops->run(list_argv, NULL, &err)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "clone target '%s' already exists", dst);
        return FALSE;
    }
    if (!err || !strstr(err->message, "dataset does not exist")) {                                                
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot check whether clone target '%s' exists: %s", dst,
                    err ? err->message : "unknown error");
        g_clear_error(&err);
        return FALSE;
    }
    g_clear_error(&err);
    g_autofree gchar *src_cfg = g_strdup_printf("%s/%s/config", lxc_path, src);
    g_autofree gchar *text = NULL;
    if (!g_file_get_contents(src_cfg, &text, NULL, &err)) {                                             
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot read the config of source container '%s': %s", src, err->message);
        g_clear_error(&err);
        return FALSE;
    }
    if (pcv_lxc_clone_has_clone_hook(text)) {                            
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "source config has lxc.hook.clone, which purecvisor does not run; remove it before cloning");
        return FALSE;
    }
    if (!ops->residue_clear(lxc_path, src, &err)) {                                          
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "container '%s' has leftover volume mounts under its rootfs that could not "
                    "be removed: %s; nothing was cloned", src, err ? err->message : "unknown error");
        g_clear_error(&err);
        return FALSE;
    }

                                                           
    const gchar *snap_argv[] = { "zfs", "snapshot", src_snap, NULL };
    if (!ops->run(snap_argv, NULL, &err)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "clone '%s' failed at snapshot: %s; nothing was created", dst,
                    err ? err->message : "unknown error");
        g_clear_error(&err);
        return FALSE;
    }

                                                          
    const gchar *send_argv[] = { "zfs", "send", src_snap, NULL };
    const gchar *recv_argv[] = { "zfs", "recv", "-u", dst_ds, NULL };
    g_autofree gchar *mp_opt = g_strdup_printf("mountpoint=%s", dst_dir);
    const gchar *set_argv[] = { "zfs", "set", mp_opt, dst_ds, NULL };                                          
    const gchar *stage = NULL;
    gboolean src_snap_left = TRUE;
    if (!ops->pipe(send_argv, recv_argv, &err))
        stage = "send/recv";
    else if (!ops->run(set_argv, NULL, &err))
        stage = "mountpoint";
    else if (!_clone_mount(ops, dst_ds, &err))
        stage = "mount";
    else if (!ops->destroy_snapshot(dst_snap, &err))
        stage = "snapshot cleanup";
    if (!stage) {                                                                                         
        GError *w = NULL;
        src_snap_left = !ops->destroy_snapshot(src_snap, &w);
        if (src_snap_left)
            PCV_LOG_WARN(CLONE_LOG_DOM, "clone '%s': source snapshot %s was not removed: %s", dst, src_snap,
                         w ? w->message : "unknown");
        g_clear_error(&w);
    }

                                                                                                   
    if (!stage && !ops->rewrite_config(lxc_path, dst, src, &err))
        stage = "config rewrite";
    if (!stage)
        (void)ops->write_hostname(lxc_path, dst);
    if (!stage && src_unpriv && !ops->settle(lxc_path, dst, &err))
        stage = "directory setup";
    if (!stage && !ops->product_files(lxc_path, src, dst, &err))                                       
        stage = "product files";
    if (!stage && subject && *subject && !ops->stamp_owner(dst, subject)) {                                             
        stage = "owner";
        err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "cannot write the owner file");
    }
    if (stage)
        return _clone_fail(ops, dst, stage, err, dst_ds, dst_dir, src_snap, src_snap_left, error);
    return TRUE;
}
