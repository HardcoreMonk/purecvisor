                                                                             
                                      
                                                                                                                    
  
                           
                                                                                                        
                                                                                                           
                                                                                                       
                                                                                                          
                                                                                           
                                                                                                           
                                                                             
                                                                                                  
                                                                                      
                                                                                      
                                                                                              
                                                                       
                                                                                         
                                                             
  
                                                                                 
                                                                        
                                                                                

#include "lxc_volume_detach.h"
#include "lxc_mountinfo.h"
#include "lxc_volume_guard.h"                                                     

#include "lxc_limits.h"                                                                 
#include "lxc_userns.h"                                                                                                   

#include <glib.h>
#include <gio/gio.h>
#include <lxc/lxccontainer.h>                                      
#include "api/drain.h"                                                   
#include "utils/pcv_spawn.h"                                              
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

                                                                                                           
                                                                                                        
                                                                                                           
                                                                                             
                                                                                                                   
                                                                               

                                                                                                                   
                                                                                           
static gchar *
_real_read_text(const gchar *path, gint64 deadline_us, gint *errno_out)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        *errno_out = errno;
        return NULL;
    }
    GString *s = g_string_new(NULL);
    char buf[16384];
    int e = 0;
    for (;;) {
        if (g_get_monotonic_time() >= deadline_us) {                        
            e = ETIMEDOUT;
            break;
        }
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            e = errno;
            break;
        }
        if (r == 0)
            break;
        g_string_append_len(s, buf, r);
    }
    close(fd);
    if (e != 0) {
        g_string_free(s, TRUE);
        *errno_out = e;
        return NULL;
    }
    return g_string_free(s, FALSE);
}

                                                                                                        
                                                                                 
static GPtrArray *
_real_list_dirs(const gchar *path, gint64 deadline_us, gint *errno_out)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
    if (!d) {
        *errno_out = errno;
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    int e = 0;
    for (;;) {
        if (g_get_monotonic_time() >= deadline_us) {                        
            e = ETIMEDOUT;
            break;
        }
        errno = 0;
        struct dirent *de = readdir(d);
        if (!de) {
            e = errno;
            break;
        }
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        gboolean is_dir = de->d_type == DT_DIR;
        if (de->d_type == DT_UNKNOWN) {                                             
            struct stat st;
            is_dir = fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode);
        }
        if (is_dir)
            g_ptr_array_add(out, g_strdup(de->d_name));
    }
    closedir(d);
    if (e != 0) {
        g_ptr_array_unref(out);
        *errno_out = e;
        return NULL;
    }
    return out;
}

static const PcvLxcVerifyOps G_verify_real_ops = {
    .read_text = _real_read_text,
    .list_dirs = _real_list_dirs,
    .now_us    = g_get_monotonic_time,
};

                                                      
static const PcvLxcVerifyOps *G_verify_ops_test;

                                                                 
static const PcvLxcVerifyOps *
_verify_ops(void)
{
    const PcvLxcVerifyOps *ops = g_atomic_pointer_get(&G_verify_ops_test);
    return ops ? ops : &G_verify_real_ops;
}

                                                                        
                                                
                                                            
void
pcv_lxc_verify_set_ops_for_test(const PcvLxcVerifyOps *ops)
{
    g_atomic_pointer_set(&G_verify_ops_test, ops);
}

                                                      
                                         
                                                            
const PcvLxcVerifyOps *
pcv_lxc_verify_real_ops_for_test(void)
{
    return &G_verify_real_ops;
}

                                                                                                          
                                                                                                
                                                                                           
                                                                                                        
                                                                        

                               
typedef struct {
    const PcvLxcVerifyOps *ops;
    const PcvVolIdent    **idents;                            
    const char           **dests;                                                                                
    gboolean              *target;                                                
    gsize                  n;
    gsize                  n_target;                      
    gboolean               target_unidentified;                                   
    const gchar           *target_dest;                                  
    gchar                 *container_path;                                          
    guint                  tasks;                           
    gint64                 deadline;                                                       
} VerifyCtx;

                                                                                                
                                                                 
                                     
static void
_ctx_init(VerifyCtx *c, const PcvLxcVolumeSet *set, const gchar *target_dest)
{
    c->ops = _verify_ops();
    c->deadline = c->ops->now_us() + PCV_LXC_VERIFY_BUDGET_US;                          
    c->n = set->lines ? set->lines->len : 0;
    c->idents = g_new0(const PcvVolIdent *, c->n + 1);
    c->dests = g_new0(const char *, c->n + 1);
    c->target = g_new0(gboolean, c->n + 1);
    c->target_dest = target_dest;
    for (gsize i = 0; i < c->n; i++) {
        const PcvLxcVolumeLine *l = set->lines->pdata[i];
        c->dests[i] = l->dest;
        c->idents[i] = l->ident_ok && l->ident.root ? &l->ident : NULL;
        c->target[i] = pcv_vol_dest_equal(l->dest, target_dest);
        c->n_target += c->target[i];
        if (c->target[i] && !c->idents[i])
            c->target_unidentified = TRUE;
    }
    c->container_path = g_strconcat("/", target_dest + strspn(target_dest, "/"), NULL);
    gsize cl = strlen(c->container_path);
    while (cl > 1 && c->container_path[cl - 1] == '/')
        c->container_path[--cl] = '\0';
}

                                              
static void
_ctx_clear(VerifyCtx *c)
{
    g_free(c->idents);
    g_free(c->dests);
    g_free(c->target);
    g_free(c->container_path);
}

                                                                             
static gboolean
_cannot_verify(GError **error, const gchar *why)
{
    g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT,
                "cannot verify that the volume is gone from the container: %s; stop the container, then detach again", why);
    return FALSE;
}

                                                                                                 
static gboolean
_cannot_read(GError **error, const gchar *path, gint e)
{
    g_autofree gchar *why = g_strdup_printf("cannot read %s: %s", path, g_strerror(e));
    return _cannot_verify(error, why);
}

                                                                             
static gboolean
_still_mounted(const VerifyCtx *c, GError **error)
{
    g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT,
                "volume is still mounted inside the container: %s; stop the container, then detach again", c->container_path);
    return FALSE;
}

                                                                                                          
                                                                             
                                                                        
static gboolean
_budget_ok(const VerifyCtx *c, GError **error)
{
    if (c->ops->now_us() < c->deadline)
        return TRUE;
    g_autofree gchar *why = g_strdup_printf("verification took longer than %d s", (int)(PCV_LXC_VERIFY_BUDGET_US / G_USEC_PER_SEC));
    return _cannot_verify(error, why);
}

                                                                                     
static gboolean
_count_task(VerifyCtx *c, GError **error)
{
    if (++c->tasks <= PCV_LXC_VERIFY_MAX_TASKS)
        return TRUE;
    g_autofree gchar *why = g_strdup_printf("more than %u tasks", (guint)PCV_LXC_VERIFY_MAX_TASKS);
    return _cannot_verify(error, why);
}

                                                                                                             
                                             
static gboolean
_skip_task_errno(gint e)
{
    return e == ENOENT || e == ESRCH || e == EINVAL;
}

#define VERIFY_SCAN_BUDGET_EVERY 1024                              

                                                                                                       
                                                                                            
                                                                               
                                                                            
static gboolean
_mountinfo_has_target(const VerifyCtx *c, gchar *text, gboolean check_point, gboolean *hit_out, GError **error)
{
    gsize cap = 1;
    for (const gchar *p = text; *p; p++)
        cap += *p == '\n';
    PcvMountinfoLine *lines = g_new(PcvMountinfoLine, cap);
    gsize n = pcv_mountinfo_parse(text, lines, cap, NULL);
    gboolean hit = FALSE, ok = TRUE;
    for (gsize i = 0; ok && !hit && i < n; i++) {
        if (i > 0 && i % VERIFY_SCAN_BUDGET_EVERY == 0 && !(ok = _budget_ok(c, error)))
            break;
                                                                                   
        gint a = pcv_vol_attribute(c->idents, c->dests, c->n, lines[i].dev_major, lines[i].dev_minor, lines[i].root,
                                   lines[i].mount_point);
        hit = a == PCV_VOL_ATTR_AMBIGUOUS || (a >= 0 && c->target[a]);
        if (!hit && check_point)
            hit = pcv_vol_dest_equal(lines[i].mount_point, c->target_dest);
    }
    g_free(lines);
    *hit_out = hit;
    return ok;
}

                                                                
static gboolean
_parse_id(const gchar *s, guint *out)
{
    guint64 v = 0;
    if (!s || !g_ascii_isdigit(s[0]) || !g_ascii_string_to_unsigned(s, 10, 1, G_MAXINT, &v, NULL))
        return FALSE;
    *out = (guint)v;
    return TRUE;
}

                                                                                                
                                                            
                                                  
static gboolean
_verify_pid(VerifyCtx *c, guint pid, GError **error)
{
    g_autofree gchar *task_dir = g_strdup_printf("/proc/%u/task", pid);
    gint e = 0;
    GPtrArray *names = c->ops->list_dirs(task_dir, c->deadline, &e);
    if (!_budget_ok(c, error)) {                                                
        if (names)
            g_ptr_array_unref(names);
        return FALSE;
    }
    if (!names)
        return _skip_task_errno(e) ? TRUE : _cannot_read(error, task_dir, e);
    GArray *tids = g_array_new(FALSE, FALSE, sizeof(guint));
    gboolean ok = TRUE;
    for (guint i = 0; ok && i < names->len; i++) {
        guint tid;
        if (!_parse_id(names->pdata[i], &tid))
            continue;                                                                         
        g_array_append_val(tids, tid);
        if (tid != pid)
            ok = _count_task(c, error);                                                  
    }
    g_ptr_array_unref(names);
    for (guint i = 0; ok && i < tids->len; i++) {
        guint tid = g_array_index(tids, guint, i);
        g_autofree gchar *path = g_strdup_printf("/proc/%u/task/%u/mountinfo", pid, tid);
        gchar *text = c->ops->read_text(path, c->deadline, &e);
        gboolean hit = FALSE;
        if (!(ok = _budget_ok(c, error))) {                                  
            g_free(text);
            break;
        }
        if (!text) {
            if (!_skip_task_errno(e))
                ok = _cannot_read(error, path, e);
            continue;
        }
        if ((ok = _mountinfo_has_target(c, text, FALSE, &hit, error)) && hit)
            ok = _still_mounted(c, error);
        g_free(text);
    }
    g_array_unref(tids);
    return ok;
}

                                                                                                                       
                                                                          
                                             
static gboolean
_verify_cgroup_procs(VerifyCtx *c, const gchar *cg, GError **error)
{
    g_autofree gchar *path = g_build_filename(cg, "cgroup.procs", NULL);
    gint e = 0;
    g_autofree gchar *text = c->ops->read_text(path, c->deadline, &e);
    if (!_budget_ok(c, error))
        return FALSE;
    if (!text)
        return e == ENOENT || e == EOPNOTSUPP ? TRUE : _cannot_read(error, path, e);
    g_auto(GStrv) rows = g_strsplit(text, "\n", -1);
    gboolean ok = TRUE;
    for (gchar **r = rows; ok && *r; r++) {
        guint pid;
        if (!**r)
            continue;
        if (!_parse_id(*r, &pid)) {
            g_autofree gchar *why = g_strdup_printf("cannot parse %s", path);
            ok = _cannot_verify(error, why);
        } else {
            ok = _count_task(c, error) && _verify_pid(c, pid, error);
        }
    }
    return ok;
}

                                                                                                               
                                                                                            
                                         
                                                         
static gboolean
_verify_cgroup_children(VerifyCtx *c, const gchar *cg, gboolean is_root, GPtrArray *queue, GError **error)
{
    gint e = 0;
    GPtrArray *subs = c->ops->list_dirs(cg, c->deadline, &e);
    if (!_budget_ok(c, error)) {
        if (subs)
            g_ptr_array_unref(subs);
        return FALSE;
    }
    if (!subs)
        return !is_root && e == ENOENT ? TRUE : _cannot_read(error, cg, e);
    gboolean ok = TRUE;
    for (guint i = 0; ok && i < subs->len; i++) {
        if (queue->len - 1 >= PCV_LXC_VERIFY_MAX_CGROUPS) {                                           
            g_autofree gchar *why = g_strdup_printf("more than %u cgroups", (guint)PCV_LXC_VERIFY_MAX_CGROUPS);
            ok = _cannot_verify(error, why);
        } else {
            g_ptr_array_add(queue, g_build_filename(cg, (const gchar *)subs->pdata[i], NULL));
        }
    }
    g_ptr_array_unref(subs);
    return ok;
}

                                                                                                              
                                                                         
                                                           
static gboolean
_verify_init(VerifyCtx *c, pid_t init_pid, GError **error)
{
    g_autofree gchar *path = g_strdup_printf("/proc/%d/mountinfo", (int)init_pid);
    gint e = 0;
    gchar *text = c->ops->read_text(path, c->deadline, &e);
    gboolean hit = FALSE;
    if (!_budget_ok(c, error)) {
        g_free(text);
        return FALSE;
    }
    if (!text)
        return _cannot_read(error, path, e);
    gboolean ok = _mountinfo_has_target(c, text, TRUE, &hit, error);
    g_free(text);
    return ok && hit ? _still_mounted(c, error) : ok;
}

                                                                                                 
                                               
                                                                    
gboolean
pcv_lxc_volume_verify_gone(const gchar *cgroup_root, const gchar *name, pid_t init_pid,
                           const PcvLxcVolumeSet *set, const gchar *target_dest, GError **error)
{
                                                                                                         
                                                                                              
                                                                                                                           
    if (!cgroup_root || !*cgroup_root || !name || !*name || strchr(name, '/') || g_str_equal(name, ".") ||
        g_str_equal(name, "..") || init_pid <= 0 || !set || !target_dest)
        return _cannot_verify(error, "invalid container name, init pid or volume set");
    VerifyCtx c = { 0 };
    _ctx_init(&c, set, target_dest);
    if (c.n_target == 0 || c.target_unidentified) {                                                             
        _ctx_clear(&c);
        return _cannot_verify(error, "cannot identify the volume source on the host");
    }
    gboolean ok = _verify_init(&c, init_pid, error);
    GPtrArray *queue = g_ptr_array_new_with_free_func(g_free);                                 
    g_ptr_array_add(queue, g_strdup_printf("%s/lxc.payload.%s", cgroup_root, name));
    for (guint i = 0; ok && i < queue->len; i++) {
        const gchar *cg = queue->pdata[i];                          
        ok = _verify_cgroup_procs(&c, cg, error) && _verify_cgroup_children(&c, cg, i == 0, queue, error);
    }
    g_ptr_array_unref(queue);
    _ctx_clear(&c);
    return ok;
}

                                                                                                       
                                                                                                                          
                                                                                 
                                                                                                            
                                                                                            
                                                                                               
                                                                                                   
                                                               

typedef gboolean (*NsDetachFn)(const gchar *const *argv, gchar **out, GError **error);

                                                             
static NsDetachFn G_ns_detach_test;

                                                                                        
                                                            
void
pcv_lxc_volume_set_ns_detach_for_test(gboolean (*fn)(const gchar *const *argv, gchar **out, GError **error))
{
    g_atomic_pointer_set(&G_ns_detach_test, fn);
}

                                                                                                 
static void
_ns_detach_add_line(GPtrArray *a, const PcvLxcVolumeLine *l)
{
    g_ptr_array_add(a, g_strdup_printf("%u:%u", l->ident.major, l->ident.minor));
    g_ptr_array_add(a, g_strdup(l->ident.root));
    g_ptr_array_add(a, g_strdup(l->dest));
}

                                                                                              
                                                                         
GStrv
pcv_lxc_volume_ns_detach_argv(const gchar *pcvctl, const gchar *name, pid_t init_pid, const PcvLxcVolumeSet *set,
                              const gchar *target_dest)
{
    if (!pcvctl || !*pcvctl || !name || !*name || strchr(name, '/') || g_str_equal(name, ".") || g_str_equal(name, "..") ||
        init_pid <= 0 || !set || !target_dest)
        return NULL;
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(a, g_strdup(pcvctl));
    g_ptr_array_add(a, g_strdup("container"));
    g_ptr_array_add(a, g_strdup("volume-ns-detach"));
    g_ptr_array_add(a, g_strdup_printf("%d", (int)init_pid));
    g_ptr_array_add(a, g_strdup_printf("/lxc.payload.%s", name));                                       
    guint n_target = 0, n_lines = set->lines ? set->lines->len : 0;
    for (guint pass = 0; pass < 2; pass++) {                                    
        for (guint i = 0; i < n_lines; i++) {
            const PcvLxcVolumeLine *l = set->lines->pdata[i];
            if (!l->ident_ok || !l->ident.root || !l->dest)
                continue;                                                           
            gboolean is_target = pcv_vol_dest_equal(l->dest, target_dest);
            if (is_target != (pass == 0))
                continue;
            _ns_detach_add_line(a, l);
            n_target += is_target;
        }
    }
    if (n_target == 0) {
        g_ptr_array_unref(a);
        return NULL;
    }
    g_ptr_array_set_free_func(a, NULL);
    g_ptr_array_add(a, NULL);
    return (GStrv)g_ptr_array_free(a, FALSE);
}

                                                                                         
                                                
static gchar *
_ns_detach_output(const gchar *out, const gchar *err)
{
    g_autofree gchar *both = g_strconcat(out ? out : "", out && *out && err && *err ? "\n" : "", err ? err : "", NULL);
    g_auto(GStrv) rows = g_strsplit(g_strstrip(both), "\n", -1);
    gchar *joined = g_strjoinv("; ", rows);
    if (!*joined) {
        g_free(joined);
        return g_strdup("no output");
    }
    return joined;
}

                                                                                   
                                                            
static gboolean
_real_ns_detach(const gchar *const *argv, gchar **out_text, GError **error)
{
    const PcvSpawnBound bound = {
        .profile           = PCV_CHILD_CAP_RUNTIME,                                                                     
        .timeout_sec       = PCV_LXC_NS_DETACH_TIMEOUT_SEC,
        .max_output_bytes  = PCV_LXC_NS_DETACH_OUTPUT_MAX,
        .cancellable       = NULL,
        .should_abort      = pcv_drain_is_terminating,
        .new_process_group = TRUE,
    };
    g_autofree gchar *out = NULL, *err = NULL;
    gint status = -1;
    if (!pcv_spawn_sync_bounded(argv, &bound, &out, &err, &status, error))
        return FALSE;
    if (status == 0) {
        if (out_text)
            *out_text = g_steal_pointer(&out);                                                  
        return TRUE;
    }
    g_autofree gchar *said = _ns_detach_output(out, err);
    g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL,
                "container namespace detach exited with status %d: %s", status, said);
    return FALSE;
}

                                                                                                        
                                                                         
static void
_ns_detach_warn_locked(const gchar *out, const gchar *name, pid_t init_pid, const gchar *target_dest)
{
    if (!out)
        return;
    g_auto(GStrv) rows = g_strsplit(out, "\n", -1);
    for (guint i = 0; rows[i]; i++) {
        if (!g_str_has_prefix(rows[i], "locked "))
            continue;
        g_warning("ns detach (container '%s', init pid %d, container_path '%s'): locked mount id %s could not be detached; "
                  "the final check will report it as still mounted (-32002)",
                  name ? name : "?", (int)init_pid, target_dest, rows[i] + strlen("locked "));
    }
}

                                                                                        
                                            
gboolean
pcv_lxc_volume_ns_detach(const gchar *name, pid_t init_pid, const PcvLxcVolumeSet *set, const gchar *target_dest, GError **error)
{
    g_auto(GStrv) argv = pcv_lxc_volume_ns_detach_argv(PCV_LXC_VOLUME_GUARD_HOOK, name, init_pid, set, target_dest);
    if (!argv) {
        g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT,
                            "cannot identify the volume source on the host; stop the container, then detach again");
        return FALSE;
    }
    NsDetachFn fn = g_atomic_pointer_get(&G_ns_detach_test);
    g_autofree gchar *out = NULL;
    if (!(fn ? fn : _real_ns_detach)((const gchar *const *)argv, &out, error))
        return FALSE;
    _ns_detach_warn_locked(out, name, init_pid, target_dest);
    return TRUE;
}

                                                                                                                  
                                                                                                      
                                                                                    
                                                                                     
                                                                                     
                                                                                        
                                                                                     
                                                                                      
                                                                                 
                                              

typedef PcvLxcState (*DetachStateFn)(const gchar *name);
typedef pid_t (*DetachInitFn)(const gchar *name);

                                                                
static DetachStateFn G_state_test;
static DetachInitFn  G_init_test;

                                                                                                                      
                                                                
static PcvLxcState
_real_state(const gchar *name)
{
    g_autofree gchar *s = pcv_lxc_get_state(name);
    static const struct { const gchar *str; PcvLxcState st; } map[] = {
        { "STOPPED", PCV_LXC_STATE_STOPPED }, { "STARTING", PCV_LXC_STATE_STARTING }, { "RUNNING", PCV_LXC_STATE_RUNNING },
        { "STOPPING", PCV_LXC_STATE_STOPPING }, { "FROZEN", PCV_LXC_STATE_FROZEN },
    };
    for (guint i = 0; s && i < G_N_ELEMENTS(map); i++)
        if (g_str_equal(s, map[i].str))
            return map[i].st;
    return PCV_LXC_STATE_UNKNOWN;
}

                                                                                               
static pid_t
_real_init_pid(const gchar *name)
{
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    pid_t pid = (c && c->is_running(c)) ? c->init_pid(c) : -1;
    if (c)
        lxc_container_put(c);
    return pid;
}

                                                                                          
                                                            
void
pcv_lxc_volume_set_state_ops_for_test(PcvLxcState (*state)(const gchar *name), pid_t (*init_pid)(const gchar *name))
{
    g_atomic_pointer_set(&G_state_test, state);
    g_atomic_pointer_set(&G_init_test, init_pid);
}

                                                                                
static gboolean
_changing_state(GError **error)
{
    g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT, "container is changing state; try again");
    return FALSE;
}

                                                                                                                       
                                                                     
static gchar *
_dest_norm(const gchar *d)
{
    return pcv_lxc_volume_dest_canon(d);
}

                                                                                              
                                                                        
static gboolean
_dest_strictly_under(const gchar *other, const gchar *target)
{
    g_autofree gchar *o = _dest_norm(other), *t = _dest_norm(target);
    if (!*t)
        return *o != '\0';
    gsize tl = strlen(t);
    return strncmp(o, t, tl) == 0 && o[tl] == '/';
}

                                                                                                                           
static gboolean
_detach_conflict(GError **error, const gchar *what, const gchar *dest)
{
    g_autofree gchar *d = _dest_norm(dest);
    g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT, "%s: /%s; stop the container, then detach again", what, d);
    return FALSE;
}

                                                                                                        
                                                                                  
static gboolean
_detach_prechecks(const PcvLxcVolumeSet *set, const gchar *container_path, guint *n_target, gboolean *has_line, GError **error)
{
    *n_target = 0;
    *has_line = FALSE;
    for (guint i = 0; i < set->lines->len; i++) {
        const PcvLxcVolumeLine *l = set->lines->pdata[i];
        if (!pcv_vol_dest_equal(l->dest, container_path))
            continue;
        (*n_target)++;
        *has_line |= l->entry != NULL;
        if (!l->ident_ok) {                                                       
            g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_CONFLICT,
                                "cannot identify the volume source on the host; stop the container, then detach again");
            return FALSE;
        }
    }
    if (*n_target == 0)
        return TRUE;
    for (guint i = 0; i < set->lines->len; i++) {                                                       
        const PcvLxcVolumeLine *l = set->lines->pdata[i];
        if (!pcv_vol_dest_equal(l->dest, container_path) && _dest_strictly_under(l->dest, container_path))
            return _detach_conflict(error, "another attached volume is mounted under this path", l->dest);
    }
    for (guint i = 0; i < set->lines->len; i++) {                                                           
        const PcvLxcVolumeLine *l = set->lines->pdata[i];
        if (!pcv_vol_dest_equal(l->dest, container_path) && !l->ident_ok)
            return _detach_conflict(error, "cannot identify the host source of another attached volume", l->dest);
    }
    return TRUE;
}

                                                                                                   
                                                                               
static gboolean
_detach_staging(const gchar *name, const PcvLxcVolumeSet *set, const gchar *container_path, GError **error)
{
    for (guint i = 0; i < set->lines->len; i++) {
        const PcvLxcVolumeLine *l = set->lines->pdata[i];
        gboolean seen = FALSE;
        if (!l->entry || !pcv_vol_dest_equal(l->dest, container_path))
            continue;                                                     
        for (guint k = 0; k < i && !seen; k++) {                                  
            const PcvLxcVolumeLine *p = set->lines->pdata[k];
            seen = p->entry && g_str_equal(p->dest, l->dest);
        }
        GError *e = NULL;
        if (!seen && !pcv_lxc_volume_staging_unmount(name, l->dest, &e)) {
            g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL,
                        "volume detached from the running container, but its staging mount was not removed: %s; run detach again",
                        e ? e->message : "unknown error");
            g_clear_error(&e);
            return FALSE;
        }
    }
    return TRUE;
}

                                                                                                                    
                                                                           
static gboolean
_config_has_volume_line(const gchar *lxc_path, const gchar *name, const gchar *container_path, gboolean *has_out, GError **error)
{
    g_autofree gchar *cfg_path = g_build_filename(lxc_path, name, "config", NULL);
    g_autofree gchar *text = NULL;
    GError *e = NULL;
    *has_out = FALSE;
    if (!g_file_get_contents(cfg_path, &text, NULL, &e)) {
        g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "cannot read the container config: %s", e->message);
        g_clear_error(&e);
        return FALSE;
    }
    GPtrArray *entries = pcv_lxc_config_mount_entries(text);
    for (guint i = 0; !*has_out && i < entries->len; i++) {
        const PcvLxcMountEntry *m = entries->pdata[i];
        *has_out = pcv_lxc_mount_entry_is_volume(m) && pcv_vol_dest_equal(m->dest, container_path);
    }
    g_ptr_array_unref(entries);
    return TRUE;
}

                                                                                             
static gboolean
_detach_running(const gchar *lxc_path, const gchar *name, const gchar *container_path, pid_t init_pid, gboolean *drop_line_out,
                GError **error)
{
    g_autofree gchar *cfg_path = g_build_filename(lxc_path, name, "config", NULL);
    g_autofree gchar *text = NULL;
    GError *e = NULL;
    if (!g_file_get_contents(cfg_path, &text, NULL, &e)) {
        g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "cannot read the container config: %s", e->message);
        g_clear_error(&e);
        return FALSE;
    }
    PcvLxcVolumeSet *set = pcv_lxc_volume_set_load_running(lxc_path, name, text, container_path, error);                     
    if (!set)
        return FALSE;
    guint n_target = 0;
    gboolean has_line = FALSE;
    gboolean ok = _detach_prechecks(set, container_path, &n_target, &has_line, error);
    if (ok && n_target > 0) {
        ok = pcv_lxc_volume_unmount_attributed(lxc_path, name, set, container_path, error);                     
        if (ok && !pcv_lxc_volume_ns_detach(name, init_pid, set, container_path, &e)) {                        
            g_warning("container '%s' volume %s: container namespace detach failed (%s); the final check decides", name, container_path,
                      e ? e->message : "unknown error");
            g_clear_error(&e);
        }
        ok = ok && pcv_lxc_volume_verify_gone(PCV_LXC_CGROUP_ROOT, name, init_pid, set, container_path, error)          
                && _detach_staging(name, set, container_path, error)                                                   
                && pcv_lxc_volume_verify_gone(PCV_LXC_CGROUP_ROOT, name, init_pid, set, container_path, error);              
    }
    pcv_lxc_volume_set_free(set);
    *drop_line_out = ok && has_line;                                             
    return ok;
}

                                                                     
                                                                         
gboolean
pcv_lxc_volume_detach_run(const gchar *lxc_path, const gchar *name, const gchar *container_path, gboolean *drop_line_out,
                          gboolean *was_running_out, GError **error)
{
                                                                                                               
                                                                                             
                                                                                                                                    
    gboolean drop = FALSE, was = FALSE;
    gboolean ok;
    if (!lxc_path || !*lxc_path || !name || !*name || strchr(name, '/') || g_str_equal(name, ".") || g_str_equal(name, "..") ||
        !container_path || container_path[0] != '/') {
        g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL, "invalid container path, name or container_path");
        ok = FALSE;
    } else {
        DetachStateFn state = g_atomic_pointer_get(&G_state_test);
        DetachInitFn init = g_atomic_pointer_get(&G_init_test);
        const PcvLxcState st = (state ? state : _real_state)(name);
        pid_t pid = st == PCV_LXC_STATE_RUNNING ? (init ? init : _real_init_pid)(name) : -1;
        if (st == PCV_LXC_STATE_STOPPED) {                                                     
            ok = _config_has_volume_line(lxc_path, name, container_path, &drop, error) &&                           
                 pcv_lxc_rootfs_residue_clear(lxc_path, name, NULL, error);
        } else if (st != PCV_LXC_STATE_RUNNING || pid <= 0) {
            ok = _changing_state(error);                                                                             
        } else {
            was = TRUE;
            ok = _detach_running(lxc_path, name, container_path, pid, &drop, error);
        }
    }
    if (drop_line_out)
        *drop_line_out = ok && drop;
    if (was_running_out)
        *was_running_out = was;
    return ok;
}

                                                                                                                  
                                                                                                                   
                                                                      
                                                              
static gchar *
_drop_rewrite(const gchar *text, gconstpointer data, GError **error)
{
    guint removed = 0;
    gchar *out = pcv_lxc_config_remove_mount_entry(text, data, &removed);
    if (removed == 0) {
        g_free(out);
        g_set_error_literal(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL,
                            "the volume line was not found in the container config");
        return NULL;
    }
    return out;
}

                                                                                      
                                                         
gboolean
pcv_lxc_volume_config_drop(const gchar *lxc_path, const gchar *name, const gchar *container_path, gboolean running, GError **error)
{
    GError *e = NULL;
    g_autoptr(GMutexLocker) cfg_lock = g_mutex_locker_new(pcv_lxc_config_mutex());                      
    gboolean ok = pcv_lxc_userns_config_rewrite(lxc_path, name, _drop_rewrite, container_path, &e);
    g_clear_pointer(&cfg_lock, g_mutex_locker_free);                  
    if (!ok) {
        g_set_error(error, PCV_LXC_VOLUME_ERROR, PCV_LXC_VOLUME_ERROR_INTERNAL,
                    running ? "volume detached from the running container, but its config line was not removed: %s; run detach again"
                            : "volume mounts were removed, but the config line was not removed: %s; run detach again",
                    e ? e->message : "unknown error");
        g_clear_error(&e);
    }
    return ok;
}
