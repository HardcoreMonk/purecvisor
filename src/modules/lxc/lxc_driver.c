   
                     
                                                    
  
                           
                                                   
                                                    
                                        
  
          
                                          
                                              
                                       
                                    
                                          
                                                   
  
          
                                                              
                                                               
                                                   
  
            
                                              
                                                                 
                                                          
                                                
  
                       
                                                                      
                                                               
                                        
                                                                                               
                                                                                
                                                 
                                                                          
                                                     
                                                                      
                                                
  
          
                                                               
                                                                              
                                                    
                                                      
                                                        
  
                              
                                            
           
                                                                    
                                          
                                          
                                                 
                                           
                                                                
                                           
                                                 
  
                             
                                                      
                                                 
                                                           
                                                  
  
                                   
                                                    
                                                                            
                                                                 
                                                             
                                                          
  
                                            
                                                           
                                                   
                                           
                                               
                                                        
                                                    
  
                                    
                                                   
                            
                                          
                                                 
                                                     
                                                               
  
         
                                                                
                                             
                                                       
                                           
  
                        
         
                                                                        
                                                                  
                                                              
                                                                          
                                                              
                                                            
         
                                                                   
                                                                          
                              
                                                     
                      
                                                                
                                                                             
                                                                      
                                                                  
                                                                          
   
                                                                                

#include "api/drain.h"
#include "lxc_driver.h"
#include "lxc_ns_exec.h"                                      
#include "lxc_limits.h"                                                                                
#include "lxc_unit.h"                                                                           
#include "lxc_volume_mount.h"                                                                                                           
#include "lxc_clone.h"                                                                  

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ftw.h>
#include <fcntl.h>
#include <sys/mount.h>                                                                 
#include "utils/pcv_spawn.h"
#include "utils/pcv_zfs_exec.h"
#include "modules/dispatcher/poll_async.h"                                                     
#include "utils/pcv_config.h"
#include "utils/pcv_log.h"
#include "utils/pcv_validate.h"                                   
#include <net/if.h>                                                                     

                         
#include <lxc/lxccontainer.h>

#define LXC_LOG_DOM "lxc_driver"

                                                                             
                                         
  
                                                 
                                                
                         
  
                                                        
                             
  
                                                        
                                    
                                                        
                                                                               

                                                  
static GHashTable *g_ctr_locks = nullptr;
                                                   
static GMutex      g_ctr_lock_mu;

   
                                             
  
                                                 
                                          
  
                                             
                                                  
                                                                    
                                            
                                             
   
static gboolean
_lock_container_op(const gchar *name)
{
    g_mutex_lock(&g_ctr_lock_mu);
    if (!g_ctr_locks)                                           
        g_ctr_locks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    if (g_hash_table_contains(g_ctr_locks, name)) {                           
        g_mutex_unlock(&g_ctr_lock_mu);
        return FALSE;                      
    }
                                                                   
    g_hash_table_insert(g_ctr_locks, g_strdup(name), GINT_TO_POINTER(1));
    g_mutex_unlock(&g_ctr_lock_mu);
    return TRUE;
}

   
                                          
  
                                                 
  
                                      
                                                       
   
static void
_unlock_container_op(const gchar *name)
{
    g_mutex_lock(&g_ctr_lock_mu);
    if (g_ctr_locks) g_hash_table_remove(g_ctr_locks, name);                            
    g_mutex_unlock(&g_ctr_lock_mu);
}

                                                                                          
                                                      
gboolean
pcv_lxc_op_lock(const gchar *name)
{
    return _lock_container_op(name);
}

void
pcv_lxc_op_unlock(const gchar *name)
{
    _unlock_container_op(name);
}

                                                                             
          
  
                              
                                                   
                                           
                                                            
                                                     
                                       
                                                                               

   
                                                          
  
                                                       
                                          
  
                           
                                                   
                                        
                                                                          
  
                       
                                                                                     
                                                                       
   
static struct lxc_container *
_lxc_get(const gchar *name, GError **error)
{
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (!c) {                            
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "liblxc: cannot allocate container handle for '%s'", name);
        return NULL;
    }
    if (!c->is_defined(c)) {                                            
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' does not exist in %s", name, PCV_LXC_PATH);
        lxc_container_put(c);                         
        return NULL;
    }
    return c;
}

   
                                            
  
                                            
                                                 
                                          
  
                                                                        
  
              
                                           
                                                                        
  
                
                                                    
                                 
                                                         
                                                        
                                 
  
             
                                                     
                         
   
static gchar *
_lxc_get_ip(struct lxc_container *c)
{
    if (!c->is_running(c)) return g_strdup("N/A");                       

                                             
    char **ips = c->get_ips(c, NULL, "inet", 0);
    if (ips && ips[0]) {
        gchar *result = g_strdup(ips[0]);                     
                                                                      
        for (int i = 0; ips[i]; i++) free(ips[i]);
        free(ips);
        return result;
    }

                                                       
                                                 
                                    
                                             
    pid_t pid = c->init_pid(c);                                            
    if (pid > 0) {
        gchar *fib_path = g_strdup_printf("/proc/%d/net/fib_trie", pid);
        gchar *content = nullptr;
        if (g_file_get_contents(fib_path, &content, NULL, NULL) && content) {
            gchar **lines = g_strsplit(content, "\n", -1);
                                                        
            for (int i = 0; lines[i] && lines[i + 1]; i++) {
                                                                 
                gchar *next = g_strstrip(g_strdup(lines[i + 1]));
                if (g_str_has_prefix(next, "/32 host LOCAL")) {
                                                       
                    gchar *ip_line = g_strstrip(g_strdup(lines[i]));
                    if (g_str_has_prefix(ip_line, "|-- ")) {
                        const gchar *ip = ip_line + 4;
                        if (!g_str_has_prefix(ip, "127.") && g_strcmp0(ip, "0.0.0.0") != 0) {
                            gchar *result = g_strdup(ip);
                            g_free(ip_line);
                            g_free(next);
                            g_strfreev(lines);
                            g_free(content);
                            g_free(fib_path);
                            return result;
                        }
                    }
                    g_free(ip_line);
                }
                g_free(next);
            }
            g_strfreev(lines);
            g_free(content);
        }
        g_free(fib_path);
    }
    return g_strdup("N/A");
}

                     
                                                          
                                               
static const gchar *
_state_to_str(PcvLxcState state)
{
    switch (state) {
        case PCV_LXC_STATE_STOPPED:  return "STOPPED";
        case PCV_LXC_STATE_STARTING: return "STARTING";
        case PCV_LXC_STATE_RUNNING:  return "RUNNING";
        case PCV_LXC_STATE_STOPPING: return "STOPPING";
        case PCV_LXC_STATE_FROZEN:   return "FROZEN";
        default:                     return "UNKNOWN";
    }
}

                               
                                                     
                                                                    
static PcvLxcState
_str_to_state(const char *s)
{
    if (!s)                        return PCV_LXC_STATE_UNKNOWN;
    if (!strcmp(s, "STOPPED"))     return PCV_LXC_STATE_STOPPED;
    if (!strcmp(s, "STARTING"))    return PCV_LXC_STATE_STARTING;
    if (!strcmp(s, "RUNNING"))     return PCV_LXC_STATE_RUNNING;
    if (!strcmp(s, "STOPPING"))    return PCV_LXC_STATE_STOPPING;
    if (!strcmp(s, "FROZEN"))      return PCV_LXC_STATE_FROZEN;
    return PCV_LXC_STATE_UNKNOWN;
}


                                                                             
                                      
  
                                                  
                                                    
                                                         
  
                         
                                                                 
                                                    
                                        
                                             
  
           
                                           
                                                
                                                              
                                                                               

   
                                             
                                          
  
                                                     
  
                                               
                                                             
                                                      
   
static gboolean
_run_argv(const gchar * const *argv, GError **error)
{
    GSubprocess *proc = pcv_spawn_newv(
        argv, G_SUBPROCESS_FLAGS_STDERR_PIPE, error);
    if (!proc) return FALSE;                                  

    gchar *stderr_out = nullptr;
                                                           
    gboolean ok = g_subprocess_communicate_utf8(
        proc, NULL, NULL, NULL, &stderr_out, error);

                                                         
    if (ok && !g_subprocess_get_successful(proc)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "command failed: %s",
                    (stderr_out && *stderr_out)
                        ? g_strstrip(stderr_out) : "unknown error");
        ok = FALSE;
    }
    g_free(stderr_out);
    g_object_unref(proc);
    return ok;
}

   
                                                      
                                   
  
                                                     
                                 
  
                                                                     
   
static gchar *
_run_argv_capture(const gchar * const *argv, GError **error)
{
    GSubprocess *proc = pcv_spawn_newv(
        argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
        error);
    if (!proc) return NULL;

    gchar *stdout_out = nullptr;
    gchar *stderr_out = nullptr;
    gboolean ok = g_subprocess_communicate_utf8(
        proc, NULL, NULL, &stdout_out, &stderr_out, error);

    if (ok && !g_subprocess_get_successful(proc)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "command failed: %s",
                    (stderr_out && *stderr_out)
                        ? g_strstrip(stderr_out) : "unknown error");
        g_free(stdout_out);
        stdout_out = nullptr;
    }
    g_free(stderr_out);
    g_object_unref(proc);
    return stdout_out;
}

                                                                             
                                                                                          
                                             
static void
_create_rollback(GTask *task, const gchar *zfs_dataset, const gchar *name, const gchar *stage,
                 GError *cause, gboolean userns)
{
    GError *rb_err = nullptr;
    gboolean removed = pcv_zfs_destroy_locked("-r", zfs_dataset, NULL, &rb_err);
    if (!removed)
        PCV_LOG_WARN(LXC_LOG_DOM, "create rollback: zfs destroy -r %s failed: %s",
                     zfs_dataset, rb_err ? rb_err->message : "unknown");
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s failed for '%s': %s; %s%s",
                            stage, name, cause ? cause->message : "unknown",
                            removed ? "container removed" : "rollback of the ZFS dataset failed, remove it manually",
                            userns ? " (refusing privileged fallback)" : "");
    g_clear_error(&rb_err);
    g_clear_error(&cause);
}

                                                                                     
                                                                   
static gboolean
_lxc_handle_privileged(struct lxc_container *c)
{
    int len = c->get_config_item(c, "lxc.idmap", NULL, 0);
    if (len <= 0)
        return TRUE;
    gchar *buf = g_malloc0((gsize)len + 1);
    gboolean privileged = c->get_config_item(c, "lxc.idmap", buf, len + 1) != len ||
                          !pcv_lxc_userns_idmap_complete(buf);
    g_free(buf);
    return privileged;
}

                                                                             
         
                                                                               

                                                     
                                           
                                                                          
void
pcv_lxc_info_free(PcvLxcInfo *info)
{
    if (!info) return;
    g_free(info->name);
    g_free(info->state_str);
    g_free(info->ip_addr);
    g_free(info->image);
    g_free(info);
}

                                                       
                                                
                                    
void
pcv_lxc_metrics_free(PcvLxcMetrics *m)
{
    if (!m) return;
    g_free(m->name);
    g_free(m->state_str);
    g_free(m->ip_addr);
    g_free(m);
}

                                                                             
            
                                                                               

                                               
                                              
                      
                                                                       
                                                
gchar *
pcv_lxc_get_state(const gchar *name)
{
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (!c) return g_strdup("UNKNOWN");                                  
    if (!c->is_defined(c)) {                                          
        lxc_container_put(c);
        return g_strdup("UNKNOWN");
    }
    const char *state_str = c->state(c);                                          
    gchar *result = g_strdup(state_str ? state_str : "UNKNOWN");
    lxc_container_put(c);
    return result;
}

                                                                                                     
                                         
PcvLxcState
pcv_lxc_state_of(const gchar *name)
{
    g_autofree gchar *s = pcv_lxc_get_state(name);
    return _str_to_state(s);
}

                                                                                       
const gchar *
pcv_lxc_state_name(PcvLxcState state)
{
    return _state_to_str(state);
}

                                                                                     
                                                                         
gboolean
pcv_lxc_container_privileged(const gchar *name)
{
    struct lxc_container *c = name ? lxc_container_new(name, PCV_LXC_PATH) : NULL;
    gboolean privileged = !c || !c->is_defined(c) || _lxc_handle_privileged(c);
    if (c) lxc_container_put(c);
    return privileged;
}

   
                      
                                               
                                         
                                                 
                                                    
                                                                              
                                                                  
   
static void
_ensure_zfs_mounts(PcvPollGuard *g)
{
    const gchar *mount_argv[] = { "zfs", "mount", "-a", NULL };
    GError *err = nullptr;
    (void)pcv_poll_guard_spawn(g, mount_argv, NULL, NULL, &err);
    if (err) g_error_free(err);                                        
}

                                                                                             
                                                          
                                                                                 
                                                                                                   
                                                                                     
                                                                    
GPtrArray *
pcv_lxc_list_guarded(PcvPollGuard *g, GError **error)
{
                                 
    _ensure_zfs_mounts(g);

    GPtrArray *result = g_ptr_array_new_with_free_func(
                            (GDestroyNotify)pcv_lxc_info_free);

                                                      
                                                            
    char **names = nullptr;
    int count = list_all_containers(PCV_LXC_PATH, &names, NULL);
    if (count < 0) {                                           
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "lxc_list_all_containers failed (path: %s)", PCV_LXC_PATH);
        return result;
    }

    for (int i = 0; i < count; i++) {
        struct lxc_container *c = lxc_container_new(names[i], PCV_LXC_PATH);
        if (!c) { free(names[i]); continue; }                           

        PcvLxcInfo *info    = g_new0(PcvLxcInfo, 1);
        info->name          = g_strdup(names[i]);
        const char *raw_st  = c->state(c);
        info->state         = _str_to_state(raw_st);                         
        info->state_str     = g_strdup(_state_to_str(info->state));                       
        info->ip_addr       = _lxc_get_ip(c);                                
        info->privileged    = _lxc_handle_privileged(c);                                            

                                                                               
        {
            gchar *meta_path = g_strdup_printf("%s/%s/purecvisor.meta",
                                                PCV_LXC_PATH, names[i]);
            gchar *meta_content = nullptr;
            if (g_file_get_contents(meta_path, &meta_content, NULL, NULL) && meta_content) {
                g_strstrip(meta_content);
                                                                    
                                                   
                info->image = (meta_content[0]) ? meta_content : g_strdup("unknown");
                if (!meta_content[0]) g_free(meta_content);
            } else {
                info->image = g_strdup("unknown");                                  
            }
            g_free(meta_path);
        }

        g_ptr_array_add(result, info);
        lxc_container_put(c);              
        free(names[i]);                                           
    }
    if (names) free(names);                      
    return result;
}

                                                              
                                                          
                                                        
                                                                      
                                                                 
                                               
PcvLxcMetrics *
pcv_lxc_get_metrics(const gchar *name, GError **error)
{
    struct lxc_container *c = _lxc_get(name, error);
    if (!c) return NULL;

    PcvLxcMetrics *m = g_new0(PcvLxcMetrics, 1);
    m->name      = g_strdup(name);
    const char *raw_st = c->state(c);
    m->state_str = g_strdup(_state_to_str(_str_to_state(raw_st)));
    m->ip_addr   = _lxc_get_ip(c);
    m->init_pid  = c->init_pid(c);

    const gchar *cg = pcv_lxc_metrics_cgroup_root();                                                               
    m->mem_used_bytes  = pcv_lxc_cgroup_read_u64(cg, name, "memory.current");
    if (m->mem_used_bytes == 0)                        
        m->mem_used_bytes = pcv_lxc_cgroup_read_u64(cg, name, "memory.usage_in_bytes");

    m->mem_limit_bytes = pcv_lxc_cgroup_read_u64(cg, name, "memory.max");
    if (m->mem_limit_bytes == 0)
        m->mem_limit_bytes = pcv_lxc_cgroup_read_u64(cg, name, "memory.limit_in_bytes");

                                                                                                                
    m->cpu_time_ns = pcv_lxc_cgroup_cpu_time_ns(cg, name);
    m->cpu_percent = pcv_lxc_cpu_sample_record(name, (gint)m->init_pid, m->cpu_time_ns, g_get_monotonic_time() * 1000);

                                                                       
    if (m->init_pid > 0) {                            
        gchar *netdev_path = g_strdup_printf("/proc/%d/net/dev", m->init_pid);
        gchar *content = nullptr;
        if (g_file_get_contents(netdev_path, &content, NULL, NULL)) {
            gchar **lines = g_strsplit(content, "\n", -1);
            for (int i = 2; lines[i]; i++) {                 
                gchar *line = g_strstrip(lines[i]);
                                                            
                if (g_str_has_prefix(line, "eth") ||
                    g_str_has_prefix(line, "veth")) {
                    guint64 rx, tx;
                                                                             
                    gchar **parts = g_strsplit(line, ":", 2);
                    if (parts[1]) {
                                                                  
                                                                             
                        if (sscanf(g_strstrip(parts[1]),
                                   "%" G_GUINT64_FORMAT
                                   " %*u %*u %*u %*u %*u %*u %*u"
                                   " %" G_GUINT64_FORMAT,
                                   &rx, &tx) == 2) {
                            m->net_rx_bytes += rx;                 
                            m->net_tx_bytes += tx;
                        }
                    }
                    g_strfreev(parts);
                }
            }
            g_strfreev(lines);
            g_free(content);
        }
        g_free(netdev_path);
    }

    lxc_container_put(c);
    return m;
}

                                                                             
                                                                  
  
                                                         
                                                        
                                                    
                                                          
  
                
                                                                 
                                                              
                                                    
                                                 
  
                            
                                              
                                            
                                      
  
            
                                                                               
                                                                                      
                                                                               

                                                         
                                         
typedef struct {
    gchar *name;
    gchar *image;                           
    guint  memory_mb;
    guint  vcpu_count;
    gchar *bridge;
    gint   rootless;                                                    
} LxcCreateData;

                                                    
static void
_lxc_create_data_free(LxcCreateData *d)
{
    g_free(d->name);
    g_free(d->image);
    g_free(d->bridge);
    g_free(d);
}

                                                     
                                                        
                                                                          
                                                                 
                                                 
static void
_lxc_create_thread(GTask        *task,
                   gpointer      source_object __attribute__((unused)),
                   gpointer      task_data,
                   GCancellable *cancellable __attribute__((unused)))
{
    LxcCreateData *d = (LxcCreateData *)task_data;
    GError *error    = nullptr;

                                                                           
    gchar **parts   = g_strsplit(d->image, ":", 2);
    const gchar *distro  = parts[0] ? parts[0] : "ubuntu";
    const gchar *release = (parts[1] && parts[1][0]) ? parts[1] : "jammy";

                                                          
                                                            
                                                  
    if (g_strcmp0(distro, "ubuntu") == 0) {
        static const struct { const char *ver; const char *code; } ubuntu_map[] = {
            {"20.04", "focal"}, {"22.04", "jammy"}, {"24.04", "noble"},
            {"24.10", "plucky"}, {"25.04", "questing"}, {NULL, NULL}                       
        };
        for (int i = 0; ubuntu_map[i].ver; i++) {
            if (g_strcmp0(release, ubuntu_map[i].ver) == 0) {
                release = ubuntu_map[i].code;
                break;
            }
        }
    }

                                                                    
                                                                           
                                                             
    PcvLxcUsernsSettings us = pcv_lxc_userns_settings_load();
    gboolean userns_wanted = d->rootless == 1 || (d->rootless < 0 && us.default_on);
    PcvLxcUsernsHost host = userns_wanted
        ? pcv_lxc_userns_host_check(PCV_LXC_SUBUID_PATH, PCV_LXC_SUBGID_PATH, us.start, us.count)
        : PCV_LXC_USERNS_READY;
    PcvLxcRootlessDecision rootless_mode =
        pcv_lxc_rootless_decide(d->rootless, us.default_on, host == PCV_LXC_USERNS_READY);
    if (rootless_mode == PCV_LXC_ROOTLESS_REJECT) {
        gchar *why = pcv_lxc_userns_host_message(host, us.start, us.count);
        PCV_LOG_WARN(LXC_LOG_DOM, "container '%s' not created: %s", d->name, why);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", why);
        g_free(why);
        g_strfreev(parts);
        return;
    }
    if (rootless_mode == PCV_LXC_ROOTLESS_OFF)                                         
        PCV_LOG_WARN(LXC_LOG_DOM, "creating privileged container '%s' (%s)", d->name,
                     us.default_on ? "rootless:false by admin" : "[container] rootless=false host default or request");

                                                                 
    gchar *zfs_dataset  = g_strdup_printf("%s/%s", PCV_LXC_ZFS_BASE, d->name);
    gchar *mountpoint   = g_strdup_printf("%s/%s", PCV_LXC_PATH, d->name);
    {
                                                                 
                                                   
                                                   
                                                   
        const gchar *parent_probe[] = {
            "zfs", "list", "-H", "-o", "name", PCV_LXC_ZFS_BASE, NULL
        };
        GError *parent_error = nullptr;
        if (!_run_argv(parent_probe, &parent_error)) {
            g_clear_error(&parent_error);
            const gchar *parent_options[] = { "-p", NULL };
            if (!pcv_zfs_create_locked(parent_options, PCV_LXC_ZFS_BASE, NULL, &parent_error)) {
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                        "ZFS container parent create failed for '%s': %s",
                                        PCV_LXC_ZFS_BASE,
                                        parent_error ? parent_error->message : "unknown");
                g_clear_error(&parent_error);
                g_free(mountpoint); g_free(zfs_dataset); g_strfreev(parts);
                return;
            }
        }
        g_autofree gchar *mountpoint_opt = g_strdup_printf("mountpoint=%s", mountpoint);
        const gchar *zfs_options[] = { "-o", mountpoint_opt, NULL };
        if (!pcv_zfs_create_locked(zfs_options, zfs_dataset, NULL, &error)) {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "ZFS create failed for '%s': %s",
                                    d->name,
                                    error ? error->message : "unknown");
            g_error_free(error);
            g_free(mountpoint); g_free(zfs_dataset); g_strfreev(parts);
            return;
        }

                                                           
        {
            const gchar *mount_argv[] = { "zfs", "mount", zfs_dataset, NULL };
            GError *mnt_err = nullptr;
            _run_argv(mount_argv, &mnt_err);
            if (mnt_err) g_error_free(mnt_err);                    
        }
    }

                                                                         
    guint memory_mb = (d->memory_mb > 0) ? d->memory_mb : 512;
    guint vcpu = (d->vcpu_count > 0) ? d->vcpu_count : 1;
    const gchar *bridge = d->bridge ? d->bridge : PCV_LXC_DEFAULT_BRIDGE;

                                                                                                  
                                                                                          
                                                                                                
    error = nullptr;
    gchar *userns_dir = nullptr;
    if (rootless_mode == PCV_LXC_ROOTLESS_ON) {
        gchar *res = pcv_lxc_userns_resource_lines(bridge, memory_mb, vcpu);
        userns_dir = res ? pcv_lxc_userns_write_create_config(lxc_get_global_config_item("lxc.default_config"),
                                                              us.start, us.count, res, &error) : nullptr;
        g_free(res);
        if (!userns_dir) {
            _create_rollback(task, zfs_dataset, d->name, "idmap config", error ? error :
                             g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "invalid bridge name"), TRUE);
            g_free(mountpoint); g_free(zfs_dataset); g_strfreev(parts);
            return;
        }
    }
    {
        gchar *userns_conf = userns_dir ? g_build_filename(userns_dir, "config", NULL) : nullptr;
        const gchar *plain_argv[] = {
            "lxc-create", "-P", PCV_LXC_PATH, "-n", d->name, "-t", "download",
            "--", "-d", distro, "-r", release, "-a", "amd64", NULL
        };
        const gchar *userns_argv[] = {
            "lxc-create", "-P", PCV_LXC_PATH, "-n", d->name, "-f", userns_conf, "-t", "download",
            "--", "-d", distro, "-r", release, "-a", "amd64", NULL
        };
        gboolean created = _run_argv(userns_conf ? userns_argv : plain_argv, &error);
        g_free(userns_conf);
        pcv_lxc_userns_remove_create_config(userns_dir);                               
        g_free(userns_dir);
        if (!created) {
            PCV_LOG_WARN("lxc", "Container '%s' lxc-create FAILED (distro=%s release=%s): %s",
                         d->name, distro, release, error ? error->message : "(no error)");
            _create_rollback(task, zfs_dataset, d->name, "lxc-create", error,
                             rootless_mode == PCV_LXC_ROOTLESS_ON);
            g_free(mountpoint); g_free(zfs_dataset); g_strfreev(parts);
            return;
        }
    }

                                                                                   
                                                                                   
                                                                          
    gboolean fell_back = FALSE;
    if (rootless_mode == PCV_LXC_ROOTLESS_ON &&
        (!pcv_lxc_userns_settle_dirs(PCV_LXC_PATH, d->name, &error) ||
         !pcv_lxc_userns_settle_rootfs(PCV_LXC_PATH, d->name, us.start, us.count, &fell_back, &error) ||
         !pcv_lxc_userns_check_top(PCV_LXC_PATH, d->name, &error))) {
        _create_rollback(task, zfs_dataset, d->name, "unprivileged rootfs setup", error, TRUE);
        g_free(mountpoint); g_free(zfs_dataset); g_strfreev(parts);
        return;
    }

                                                                   
                                                                                                        
                                                                                         
    const gboolean userns = rootless_mode == PCV_LXC_ROOTLESS_ON;
    const gchar *stage = nullptr;
    g_autoptr(GMutexLocker) cfg_lock = g_mutex_locker_new(pcv_lxc_config_mutex());
    struct lxc_container *c = lxc_container_new(d->name, PCV_LXC_PATH);
    if (!c || !c->is_defined(c)) {
        stage = "open after create";
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "liblxc cannot open the created container");
    } else if (userns) {
                                                                       
        char link[64] = "";
        if (_lxc_handle_privileged(c)) {
            stage = "idmap check";
            error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "lxc.idmap u/g missing after create");
        } else if (c->get_config_item(c, "lxc.net.0.link", link, sizeof(link)) <= 0 || g_strcmp0(link, bridge) != 0) {
            stage = "config check";
            error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "lxc.net.0.link is '%s', expected '%s'", link, bridge);
        }
    } else {
                                                                                         
        gchar *mem_val  = g_strdup_printf("%uM", memory_mb);
        c->set_config_item(c, "lxc.cgroup.memory.limit_in_bytes", mem_val);
        c->set_config_item(c, "lxc.cgroup2.memory.max",           mem_val);
        g_free(mem_val);

                                                                                             
        gchar *cpu_max = g_strdup_printf("%" G_GUINT64_FORMAT " %d", (guint64)vcpu * PCV_LXC_CPU_PERIOD_US, PCV_LXC_CPU_PERIOD_US);
        c->set_config_item(c, "lxc.cgroup2.cpu.max", cpu_max);
        g_free(cpu_max);

                                                                     
        c->clear_config_item(c, "lxc.net");                                       
        c->set_config_item(c, "lxc.net.0.type",    "veth");
        c->set_config_item(c, "lxc.net.0.link",    bridge);
        c->set_config_item(c, "lxc.net.0.flags",   "up");
                                                                 
        c->set_config_item(c, "lxc.net.0.hwaddr",  "00:16:3e:xx:xx:xx");                 
    }

                                                                                   
                                                                
    if (!stage && userns && fell_back) {
        gchar *conf_path = g_build_filename(PCV_LXC_PATH, d->name, "config", NULL), *conf_text = nullptr;
        const PcvLxcConfigEdit inc[] = { { "lxc.include", PCV_LXC_USERNS_INCLUDE, TRUE } };
        if (!g_file_get_contents(conf_path, &conf_text, NULL, &error) ||
            (!pcv_lxc_userns_config_has_include(conf_text) &&
             !pcv_lxc_userns_config_edit(PCV_LXC_PATH, d->name, inc, G_N_ELEMENTS(inc), &error)))
            stage = "userns include";
        g_free(conf_text); g_free(conf_path);
    }

                                                 
    if (!stage) {
        gchar *image_tag = g_strdup_printf("%s:%s", distro, release);
        gchar *meta_path = g_strdup_printf("%s/%s/purecvisor.meta",
                                            PCV_LXC_PATH, d->name);
        g_file_set_contents(meta_path, image_tag, -1, NULL);
        g_free(meta_path);
        g_free(image_tag);
    }

                                                                    
    if (!stage && !userns && !c->save_config(c, NULL)) {
        stage = "save config";
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "liblxc save_config failed");
    }

    if (c) lxc_container_put(c);
    g_clear_pointer(&cfg_lock, g_mutex_locker_free);                        
    if (stage)
        _create_rollback(task, zfs_dataset, d->name, stage, error, userns);
    else
        g_task_return_boolean(task, TRUE);
    g_strfreev(parts);
    g_free(mountpoint);
    g_free(zfs_dataset);
}

                                                    
                                                       
                                                                        
                                                                                          
void
pcv_lxc_create_async(const gchar        *name,
                     const gchar        *image,
                     guint               memory_mb,
                     guint               vcpu_count,
                     const gchar        *network_bridge,
                     GCancellable       *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer            user_data)
{
    pcv_lxc_create_async_full(name, image, memory_mb, vcpu_count,
                              network_bridge, -1,
                              cancellable, callback, user_data);
}

                                                          
                                                          
                                                           
                                                          
                                                                     
void
pcv_lxc_create_async_full(const gchar        *name,
                          const gchar        *image,
                          guint               memory_mb,
                          guint               vcpu_count,
                          const gchar        *network_bridge,
                          gint                rootless,
                          GCancellable       *cancellable,
                          GAsyncReadyCallback callback,
                          gpointer            user_data)
{
    GTask *task         = pcv_drain_task_new(NULL, cancellable, callback, user_data);
    LxcCreateData *data = g_new0(LxcCreateData, 1);
    data->name          = g_strdup(name);
    data->image         = g_strdup(image ? image : "ubuntu:22.04");
    data->memory_mb     = memory_mb;
    data->vcpu_count    = vcpu_count;
    data->bridge        = g_strdup(network_bridge ? network_bridge
                                                   : PCV_LXC_DEFAULT_BRIDGE);
    data->rootless      = rootless;                              

    g_task_set_task_data(task, data, (GDestroyNotify)_lxc_create_data_free);
    g_task_run_in_thread(task, _lxc_create_thread);
    g_object_unref(task);
}

                                                  
                                              
                                                               
gboolean
pcv_lxc_create_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

                                                                             
                                                    
  
                                                     
                                                          
                                               
  
           
                                       
                                             
                                                                              
  
                                                                                    
                                                                                  
                                                          
                                                                               

                                                                                       
static gint G_zfs_destroy_interval_ms;

                                                                              
                               
                                                            
void
pcv_lxc_zfs_destroy_set_interval_for_test(guint ms)
{
    g_atomic_int_set(&G_zfs_destroy_interval_ms, (gint)ms);
}

                                                                                         
                                                                        
gboolean
pcv_lxc_zfs_destroy_retry(const gchar *dataset, GError **error)
{
    GError *err = nullptr;
    for (guint i = 0; i <= PCV_LXC_ZFS_DESTROY_RETRIES; i++) {
        gint ms = g_atomic_int_get(&G_zfs_destroy_interval_ms);
        if (i > 0)
            g_usleep((gulong)(ms > 0 ? ms : PCV_LXC_ZFS_DESTROY_INTERVAL_MS) * 1000);
        g_clear_error(&err);
        if (pcv_zfs_destroy_locked("-r", dataset, NULL, &err))                           
            return TRUE;
        if (strstr(err->message, "dataset does not exist")) {                           
            g_error_free(err);
            return TRUE;
        }
        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))                                             
            break;
        PCV_LOG_WARN(LXC_LOG_DOM, "zfs destroy -r %s failed (try %u of %u): %s",
                     dataset, i + 1, PCV_LXC_ZFS_DESTROY_RETRIES + 1, err->message);
    }
    g_propagate_error(error, err);
    return FALSE;
}

                                                          
                                                            
                                                             
                                                             
                                                
static int
_pcv_recursive_unlink_cb(const char *fpath,
                         const struct stat *sb __attribute__((unused)),
                         int typeflag,
                         struct FTW *ftwbuf __attribute__((unused)))
{
    if (typeflag == FTW_DP || typeflag == FTW_D) {
        return rmdir(fpath);                    
    }
    return unlink(fpath);                           
}

                                                                                                          
static gboolean (*G_destroy_running_probe)(const gchar *name);

                                                                                                        
                                                           
                                                            
void
pcv_lxc_destroy_set_running_probe_for_test(gboolean (*fn)(const gchar *name))
{
    G_destroy_running_probe = fn;
}

                                                  
                                       
                                                                  
                                                                     
                                                                 
static void
_lxc_destroy_thread(GTask        *task,
                    gpointer      source_object __attribute__((unused)),
                    gpointer      task_data,
                    GCancellable *cancellable __attribute__((unused)))
{
    const gchar *name = (const gchar *)task_data;
    GError      *error = nullptr;

                                                      
    if (!_lock_container_op(name)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "Container '%s' has a concurrent operation in progress", name);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY,
                                "Container '%s' has a concurrent operation in progress", name);
        return;
    }

                         
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (c && c->is_defined(c) && c->is_running(c)) {
        if (!c->stop(c)) {
            g_warning("lxc_driver: stop failed for '%s', continuing destroy", name);
        }
    }

                                                                                             
                                                                                   
                                                                
    const gboolean still_running = G_destroy_running_probe ? G_destroy_running_probe(name)
                                                          : (!c || (c->is_defined(c) && c->is_running(c)));
    if (c) lxc_container_put(c);
    if (still_running) {
        PCV_LOG_WARN(LXC_LOG_DOM, "container '%s' is still running after the forced stop; skipping leftover volume mount cleanup", name);
    } else {
        GError *clear_err = nullptr;
        if (!pcv_lxc_rootfs_residue_clear(PCV_LXC_PATH, name, NULL, &clear_err)) {
            _unlock_container_op(name);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "container '%s' has leftover volume mounts under its rootfs that could not be removed: %s; "
                                    "nothing was destroyed", name, clear_err ? clear_err->message : "unknown error");
            g_clear_error(&clear_err);
            return;
        }
    }
    pcv_lxc_runtime_config_remove(name);                                                        

                                                   
    {
        gboolean lxc_destroy_ok = FALSE;
        const gchar *argv[] = {
            "lxc-destroy", "-P", PCV_LXC_PATH, "-n", name, "-f", NULL
        };
        if (_run_argv(argv, &error)) {
            lxc_destroy_ok = TRUE;
        } else {
                                                          
                                                               
            gchar *config_path = g_strdup_printf("%s/%s/config", PCV_LXC_PATH, name);
            gboolean config_gone = !g_file_test(config_path, G_FILE_TEST_EXISTS);
            g_free(config_path);

            if (!config_gone) {                                
                _unlock_container_op(name);
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                        "lxc-destroy failed for '%s': %s",
                                        name,
                                        error ? error->message : "unknown");
                g_error_free(error);
                return;
            }

            PCV_LOG_WARN(LXC_LOG_DOM,
                         "lxc-destroy reported failure for '%s' but config is already gone; continuing with ZFS cleanup",
                         name);
            if (error) {
                g_error_free(error);
                error = NULL;
            }
        }

        if (!lxc_destroy_ok) {
            PCV_LOG_INFO(LXC_LOG_DOM, "Container '%s' entering destroy cleanup fallback path", name);
        }
    }

                                                                                   
    {
        g_autofree gchar *zfs_target = g_strdup_printf("%s/%s", PCV_LXC_ZFS_BASE, name);
        GError *zfs_err = nullptr;
        if (!pcv_lxc_zfs_destroy_retry(zfs_target, &zfs_err)) {
            _unlock_container_op(name);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "container '%s' removed but ZFS dataset '%s' "
                                    "remains: %s; run container.destroy again to finish cleanup",
                                    name, zfs_target, zfs_err->message);
            g_error_free(zfs_err);
            return;
        }
    }

                                                                                                                                          
    {
        g_autofree gchar *container_dir = g_strdup_printf("%s/%s", PCV_LXC_PATH, name);
        if (g_file_test(container_dir, G_FILE_TEST_IS_DIR) && g_rmdir(container_dir) != 0)
            PCV_LOG_WARN(LXC_LOG_DOM, "Post-destroy rmdir failed for '%s': %s", container_dir, g_strerror(errno));
        if (g_file_test(container_dir, G_FILE_TEST_EXISTS))
            nftw(container_dir, _pcv_recursive_unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
    }

    pcv_lxc_unit_reset_failed_best_effort(name);                                         
    pcv_lxc_cpu_sample_forget(name);                                                          
    _unlock_container_op(name);
    g_task_return_boolean(task, TRUE);
}

                                              
                                                     
void
pcv_lxc_destroy_async(const gchar        *name,
                      GCancellable       *cancellable,
                      GAsyncReadyCallback callback,
                      gpointer            user_data)
{
    GTask *task = pcv_drain_task_new(NULL, cancellable, callback, user_data);
    g_task_set_task_data(task, g_strdup(name), (GDestroyNotify)g_free);                     
    g_task_run_in_thread(task, _lxc_destroy_thread);
    g_object_unref(task);
}

                                                
                                                               
gboolean
pcv_lxc_destroy_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

                                                                             
                                                                      
  
                                                     
                                                     
                                                          
  
                                                                        
                                                                   
                                                                   
                                                                  
  
       
                                                           
                                                                                                 
                                                                                     
                                                                               

typedef struct {
    gchar *source;
    gchar *target;
    gchar *subject;                                                                       
    PcvLxcCloneReleaseFunc release_cb;                                                                        
    gpointer release_data;                                          
} CloneCtx;

                                                                                                      
                                                    
static void _clone_ctx_free(gpointer p) {
    CloneCtx *ctx = p;
    g_free(ctx->source);
    g_free(ctx->target);
    g_free(ctx->subject);
    g_free(ctx);
}

                                                                                              
                                                                                         
                                                                                               
                                            
                                                                   
                                                                      
static void
_clone_task_return(GTask *task, CloneCtx *ctx, GError *err)
{
    PcvLxcCloneReleaseFunc release = ctx->release_cb;
    ctx->release_cb = NULL;             
    if (release) release(ctx->release_data);
    if (err)
        g_task_return_error(task, err);
    else
        g_task_return_boolean(task, TRUE);
}

                                                                                               
                                                             
                                                            
static void
_clone_worker(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;                                               
    CloneCtx *ctx = data;
    if (!ctx->source || !ctx->target || !*ctx->source || !*ctx->target) {                                        
        _clone_task_return(task, ctx, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "source and target are required"));
        return;
    }
    if (!_lock_container_op(ctx->source)) {
        _clone_task_return(task, ctx, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_BUSY,
                                                          "source container has a concurrent operation in progress"));
        return;
    }
    if (!_lock_container_op(ctx->target)) {
        _unlock_container_op(ctx->source);
        _clone_task_return(task, ctx, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_BUSY,
                                                          "target container has a concurrent operation in progress"));
        return;
    }
    const gboolean src_unpriv = !pcv_lxc_container_privileged(ctx->source);                       
    GError *err = nullptr;
    gboolean ok = pcv_lxc_clone_run(PCV_LXC_PATH, PCV_LXC_ZFS_BASE, ctx->source, ctx->target, ctx->subject, src_unpriv, &err);
    _unlock_container_op(ctx->target);
    _unlock_container_op(ctx->source);                                
    if (!ok && !err)                                              
        err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "clone failed");
    if (ok)
        PCV_LOG_INFO(LXC_LOG_DOM, "Cloned container '%s' -> '%s'", ctx->source, ctx->target);
    else
        PCV_LOG_WARN(LXC_LOG_DOM, "Clone failed '%s' -> '%s': %s", ctx->source, ctx->target, err->message);
    _clone_task_return(task, ctx, ok ? NULL : err);
}

                                                    
                                         
                                                                                                     
                                                                                                       
void
pcv_lxc_clone_async(const gchar *source, const gchar *target, const gchar *subject,
                    PcvLxcCloneReleaseFunc release_cb, gpointer release_data,
                    GCancellable *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    GTask *task = pcv_drain_task_new(NULL, cancellable, callback, user_data);
    CloneCtx *ctx = g_new0(CloneCtx, 1);
    ctx->source = g_strdup(source);
    ctx->target = g_strdup(target);
    ctx->subject = g_strdup(subject);
    ctx->release_cb = release_cb;
    ctx->release_data = release_data;
    g_task_set_task_data(task, ctx, _clone_ctx_free);
    g_task_run_in_thread(task, _clone_worker);
    g_object_unref(task);
}

                                                                  
gboolean
pcv_lxc_clone_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

                                                         
                                          
                                                        
gboolean
pcv_lxc_clone(const gchar *source, const gchar *target)
{
    if (!source || !target || !*source || !*target) return FALSE;
    pcv_lxc_clone_async(source, target, NULL, NULL, NULL, NULL, NULL, NULL);                               
    return TRUE;
}

                                                                             
                                                                      
  
                                                                    
                                                                              
                                                            
  
                                                                                               
                                                                                            
  
        
                                               
                                                                               

                                                                                              
                                                                                         
gboolean
pcv_lxc_start_sync(const gchar *name, GError **error)
{
    g_autofree gchar *lxc_path = g_strdup(PCV_LXC_PATH);                                         
    struct lxc_container *c = lxc_container_new(name, lxc_path);
    const gboolean running = c && c->is_running(c);
    if (c) lxc_container_put(c);
    if (running)
        return TRUE;
                                                                                                        
                                                                            
    if (!pcv_lxc_volume_start_prepare(lxc_path, name, error))
        return FALSE;
    if (!pcv_lxc_unit_start(lxc_path, name, error)) {
        pcv_lxc_volume_start_abort(lxc_path, name);
        return FALSE;
    }
                                                                                 
    GError *e = nullptr;
    if (!pcv_lxc_limits_reapply(PCV_LXC_PATH, name, PCV_LXC_CGROUP_ROOT, &e))
        PCV_LOG_WARN(LXC_LOG_DOM, "container '%s' started but limits were not re-applied: %s", name, e ? e->message : "unknown");
    g_clear_error(&e);
    return TRUE;
}

                                            
                                                                 
                                                                                  
                                                                                                
static void
_lxc_start_thread(GTask        *task,
                  gpointer      source_object __attribute__((unused)),
                  gpointer      task_data,
                  GCancellable *cancellable __attribute__((unused)))
{
    const gchar *name = (const gchar *)task_data;
    GError      *error = nullptr;

    struct lxc_container *c = _lxc_get(name, &error);
    if (!c) { g_task_return_error(task, error); return; }                                

    if (c->is_running(c)) {                                     
        g_message("lxc_driver: container '%s' is already running", name);
        lxc_container_put(c);
        g_task_return_boolean(task, TRUE);
        return;
    }
    lxc_container_put(c);                                           

                                              
    {
        gchar *zfs_ds = g_strdup_printf("%s/%s", PCV_LXC_ZFS_BASE, name);
        const gchar *mount_argv[] = { "zfs", "mount", zfs_ds, NULL };
        GError *mnt_err = nullptr;
        _run_argv(mount_argv, &mnt_err);
        if (mnt_err) g_error_free(mnt_err);                    
        g_free(zfs_ds);
    }

    pcv_lxc_volume_hook_ensure(PCV_LXC_PATH, name);                                                                    

    g_autofree gchar *bad = nullptr, *bad_msg = nullptr;                                                                                                   
    if (!pcv_lxc_volume_guard_check_start(PCV_LXC_PATH, name, &bad)) {
        bad_msg = pcv_lxc_volume_guard_message(bad);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s", bad_msg);                                 
        return;
    }
    if (pcv_lxc_start_sync(name, &error))
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, error);                                              
}

                                                                                                     
                                                                              
void
pcv_lxc_start_async(const gchar        *name,
                    const gchar        *actor,
                    GCancellable       *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer            user_data)
{
    GTask *task = pcv_drain_task_new(NULL, cancellable, callback, user_data);
    if (actor)                                                                                          
        pcv_drain_task_set_actor(task, actor);
    g_task_set_task_data(task, g_strdup(name), (GDestroyNotify)g_free);
    g_task_run_in_thread(task, _lxc_start_thread);
    g_object_unref(task);
}

                                                                         
gboolean pcv_lxc_start_finish(GAsyncResult *result, GError **error)
{ return g_task_propagate_boolean(G_TASK(result), error); }

                                                                             
                                                         
  
                                                        
                                                    
                            
  
                   
                                                                                                                         
                                                                                               
  
        
                                            
                                                                               

   
                                  
  
                                                      
                                                                       
                                                     
                                                   
                          
   
typedef struct { gchar *name; gboolean force; } LxcStopData;                           
static void _lxc_stop_data_free(gpointer p) { LxcStopData *d = p; g_free(d->name); g_free(d); }

                                                                                           
static const PcvLxcStopOps *G_stop_ops;

                                                                                        
                                                           
                                                            
void
pcv_lxc_stop_set_ops_for_test(const PcvLxcStopOps *ops)
{
    G_stop_ops = ops;
}

                                                                                                                          
                                                                                                     
                                                                                                      
static gboolean
_lxc_halt(struct lxc_container *c, const gchar *name, guint grace_sec, GError **error)
{
    struct lxc_container *own = c ? nullptr : _lxc_get(name, error), *h = c ? c : own;
    gboolean ok = h && grace_sec > 0 && h->shutdown(h, (int)grace_sec);
    if (h && !ok && grace_sec > 0)
        g_warning("lxc_driver: graceful shutdown timed out for '%s', forcing stop", name);
    if (h && !ok && !(ok = h->stop(h)))                                     
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "liblxc: stop failed for '%s': %s", name,
                    h->error_string ? h->error_string : "unknown error");
    if (own)
        lxc_container_put(own);
    return ok;
}

                                                                                                 
                                               
gboolean
pcv_lxc_halt(const gchar *name, guint grace_sec, GError **error)
{
    return _lxc_halt(NULL, name, grace_sec, error);
}

                                           
                                                        
                                                                    
static void
_lxc_stop_thread(GTask        *task,
                 gpointer      source_object __attribute__((unused)),
                 gpointer      task_data,
                 GCancellable *cancellable __attribute__((unused)))
{
    LxcStopData *d = (LxcStopData *)task_data;
    GError      *error = nullptr;

                                               
    if (!_lock_container_op(d->name)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "Container '%s' has a concurrent operation in progress", d->name);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY,
                                "Container '%s' has a concurrent operation in progress", d->name);
        return;
    }

    const PcvLxcStopOps *ops = G_stop_ops;                                              
    struct lxc_container *c = nullptr;
    if (!ops) {
        c = _lxc_get(d->name, &error);
        if (!c) { _unlock_container_op(d->name); g_task_return_error(task, error); return; }
    }

    if (!(ops ? ops->is_running(d->name) : c->is_running(c))) {
        g_message("lxc_driver: container '%s' is already stopped", d->name);
        if (c) lxc_container_put(c);
        _unlock_container_op(d->name);
        g_task_return_boolean(task, TRUE);
        return;
    }

                                                                                                            
    const gboolean ok = ops ? ops->stop(d->name, d->force) : _lxc_halt(c, d->name, d->force ? 0 : PCV_LXC_STOP_GRACE_SEC, &error);
    if (!ok) {
        if (!error)                              
            g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED, "liblxc: stop failed for '%s': stop ops failed", d->name);
        g_task_return_error(task, error);
    } else {
                                                                                                
        GError *clear_err = nullptr;
        if (!pcv_lxc_rootfs_residue_clear(PCV_LXC_PATH, d->name, NULL, &clear_err))
            PCV_LOG_WARN(LXC_LOG_DOM, "container '%s' stopped, but leftover volume mounts remain: %s",
                         d->name, clear_err ? clear_err->message : "unknown error");
        g_clear_error(&clear_err);
        pcv_lxc_runtime_config_remove(d->name);                                                          
        g_task_return_boolean(task, TRUE);
    }
    if (c) lxc_container_put(c);
    _unlock_container_op(d->name);
}

                                           
                                        
                                                             
void
pcv_lxc_stop_async(const gchar        *name,
                   gboolean            force,
                   GCancellable       *cancellable,
                   GAsyncReadyCallback callback,
                   gpointer            user_data)
{
    GTask *task        = pcv_drain_task_new(NULL, cancellable, callback, user_data);
    LxcStopData *data  = g_new0(LxcStopData, 1);
    data->name         = g_strdup(name);
    data->force        = force;
    g_task_set_task_data(task, data, _lxc_stop_data_free);
    g_task_run_in_thread(task, _lxc_stop_thread);
    g_object_unref(task);
}

                                                                 
gboolean pcv_lxc_stop_finish(GAsyncResult *result, GError **error)
{ return g_task_propagate_boolean(G_TASK(result), error); }

                                                                             
                                                         
  
                                                  
                                                          
                                      
  
       
                                                                                                    
                                                                           
                                                                                
                                                                         
                                                                              
                                                                               

typedef struct { gchar *name; gchar **argv; } LxcExecData;                                  

static void
_lxc_exec_data_free(LxcExecData *d)
{ g_free(d->name); g_strfreev(d->argv); g_free(d); }

                                                        
                                                   
                                                           
                                                                      
                                                                      
                                                                                 
static void
_lxc_exec_thread(GTask        *task,
                 gpointer      source_object __attribute__((unused)),
                 gpointer      task_data,
                 GCancellable *cancellable __attribute__((unused)))
{
    LxcExecData *d = (LxcExecData *)task_data;
    GError *error  = nullptr;
    gchar *stdout_out = nullptr;
    gchar *stderr_out = nullptr;
    gint status = -1;

    gchar **argv = pcv_lxc_attach_argv(PCV_LXC_PATH, d->name, (const gchar * const *)d->argv);
    gboolean ran = argv && pcv_lxc_ns_run((const gchar * const *)argv, PCV_LXC_EXEC_TIMEOUT_SEC,
                                          PCV_LXC_EXEC_OUTPUT_MAX, &stdout_out, &stderr_out,
                                          &status, &error);
    g_strfreev(argv);

    if (!ran) {                                                  
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "exec failed (timeout or error): %s",
                                error ? error->message : "invalid exec request");
        g_clear_error(&error);
    } else if (status != 0) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "exec failed: %s",
                                (stderr_out && *stderr_out) ? stderr_out : "non-zero exit");
        g_free(stdout_out);
    } else {
        g_task_return_pointer(task,
                              stdout_out ? stdout_out : g_strdup(""),
                              (GDestroyNotify)g_free);
    }
    g_free(stderr_out);
}

                                                  
                                            
                                                                       
void
pcv_lxc_exec_async(const gchar        *name,
                   const gchar       **argv,
                   GCancellable       *cancellable,
                   GAsyncReadyCallback callback,
                   gpointer            user_data)
{
    GTask *task      = pcv_drain_task_new(NULL, cancellable, callback, user_data);
    LxcExecData *d   = g_new0(LxcExecData, 1);
    d->name          = g_strdup(name);
    d->argv          = g_strdupv((gchar **)argv);
    g_task_set_task_data(task, d, (GDestroyNotify)_lxc_exec_data_free);
    g_task_run_in_thread(task, _lxc_exec_thread);
    g_object_unref(task);
}

                                                     
                                                                 
gchar *
pcv_lxc_exec_finish(GAsyncResult *result, GError **error)
{
    return g_task_propagate_pointer(G_TASK(result), error);
}

                                                                             
                                                            
  
                                                      
                                                       
                                                        
  
               
                                                         
                                               
  
                                             
                                                 
  
           
                                            
                                                                                                  
                                           
                                                        
                                                                               

typedef struct { gchar *name; gchar *snap; } LxcSnapData;                             

static void
_snap_data_free(LxcSnapData *d) { g_free(d->name); g_free(d->snap); g_free(d); }

                                                                                             
                                        
static void
_snap_run(const gchar *name, const gchar *snap_name, GCancellable *c, GAsyncReadyCallback cb,
          gpointer user_data, GTaskThreadFunc fn)
{
    GTask *task     = pcv_drain_task_new(NULL, c, cb, user_data);
    LxcSnapData *d  = g_new0(LxcSnapData, 1);
    d->name = g_strdup(name); d->snap = g_strdup(snap_name);
    g_task_set_task_data(task, d, (GDestroyNotify)_snap_data_free);
    g_task_run_in_thread(task, fn);
    g_object_unref(task);
}

                                                                      
static void
_snap_create_thread(GTask *task, gpointer src __attribute__((unused)),
                    gpointer td, GCancellable *c __attribute__((unused)))
{
    LxcSnapData *d = td;
    GError *error  = nullptr;
    gchar *target = g_strdup_printf("%s/%s@%s", PCV_LXC_ZFS_BASE, d->name, d->snap);
    const gchar *argv[] = { "zfs", "snapshot", target, NULL };
    if (!_run_argv(argv, &error)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "snapshot create failed: %s",
                                error ? error->message : "unknown");
        g_error_free(error);
    } else {
        g_task_return_boolean(task, TRUE);
    }
    g_free(target);
}

                                                   
                                             
void pcv_lxc_snapshot_create_async(const gchar *name, const gchar *snap_name,
                                    GCancellable *c, GAsyncReadyCallback cb,
                                    gpointer user_data)
{ _snap_run(name, snap_name, c, cb, user_data, _snap_create_thread); }

                                               
gboolean pcv_lxc_snapshot_create_finish(GAsyncResult *r, GError **e)
{ return g_task_propagate_boolean(G_TASK(r), e); }

                                                               
static void
_snap_delete_thread(GTask *task, gpointer src __attribute__((unused)),
                    gpointer td, GCancellable *c __attribute__((unused)))
{
    LxcSnapData *d = td;
    GError *error  = nullptr;
    gchar *target = g_strdup_printf("%s/%s@%s", PCV_LXC_ZFS_BASE, d->name, d->snap);
    if (!pcv_zfs_destroy_locked_short(NULL, target, NULL, &error)) {                            
        g_task_return_new_error(task, G_IO_ERROR, error ? error->code : G_IO_ERROR_FAILED,
                                "snapshot delete failed: %s",
                                error ? error->message : "unknown");
        g_error_free(error);
    } else {
        g_task_return_boolean(task, TRUE);
    }
    g_free(target);
}

                                                     
                                      
void pcv_lxc_snapshot_delete_async(const gchar *name, const gchar *snap_name,
                                    GCancellable *c, GAsyncReadyCallback cb,
                                    gpointer user_data)
{ _snap_run(name, snap_name, c, cb, user_data, _snap_delete_thread); }

                                           
gboolean pcv_lxc_snapshot_delete_finish(GAsyncResult *r, GError **e)
{ return g_task_propagate_boolean(G_TASK(r), e); }

                                                  
static void
_snap_list_thread(GTask *task, gpointer src __attribute__((unused)),
                  gpointer td, GCancellable *c __attribute__((unused)))
{
    const gchar *name = (const gchar *)td;
    GError *error = nullptr;
    gchar *dataset = g_strdup_printf("%s/%s", PCV_LXC_ZFS_BASE, name);
    const gchar *argv[] = {
        "zfs", "list", "-H", "-t", "snapshot", "-o", "name", dataset, NULL
    };
    gchar *stdout_out = _run_argv_capture(argv, &error);
    g_free(dataset);

    if (!stdout_out) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "snapshot list failed: %s",
                                error ? error->message : "zfs list error");
        if (error) g_error_free(error);
        return;
    }

    GPtrArray *result = g_ptr_array_new_with_free_func(g_free);                   
    gchar **lines     = g_strsplit(stdout_out ? stdout_out : "", "\n", -1);
    g_free(stdout_out);

    for (int i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!line[0]) continue;                        
                                                                       
        const gchar *at = strchr(line, '@');
        if (at) g_ptr_array_add(result, g_strdup(at + 1));                      
    }
    g_strfreev(lines);
    g_task_return_pointer(task, result,
                          (GDestroyNotify)g_ptr_array_unref);
}

                                                   
                                           
void pcv_lxc_snapshot_list_async(const gchar *name,
                                   GCancellable *c, GAsyncReadyCallback cb,
                                   gpointer user_data)
{
    GTask *task = pcv_drain_task_new(NULL, c, cb, user_data);
    g_task_set_task_data(task, g_strdup(name), (GDestroyNotify)g_free);
    g_task_run_in_thread(task, _snap_list_thread);
    g_object_unref(task);
}

                                                    
                                                                    
GPtrArray *pcv_lxc_snapshot_list_finish(GAsyncResult *r, GError **e)
{ return g_task_propagate_pointer(G_TASK(r), e); }

                                                                             
                                               
  
                                                      
                                               
                                      
  
       
                                          
                                            
                                              
  
                                                       
                                                      
                                                                 
                                                      
                                                   
  
                           
                                                           
                                                                       
                                                                
                          
  
         
                                                             
                              
                                                
                                                                               

   
                                          
  
                                                       
                              
  
                                                                   
                                                               
   
static gboolean
_cgroup_write_with_fallback(const gchar *path_v2, const gchar *val_v2,
                             const gchar *path_v1, const gchar *val_v1,
                             GError **error)
{
                                                          
    if (g_file_test(path_v2, G_FILE_TEST_EXISTS)) {
        if (pcv_lxc_cgroup_write(path_v2, val_v2, error))
            return TRUE;
                                       
        return FALSE;
    }

                                                                  
    if (path_v1 && g_file_test(path_v1, G_FILE_TEST_EXISTS)) {
        if (pcv_lxc_cgroup_write(path_v1, val_v1 ? val_v1 : val_v2, error))
            return TRUE;
        return FALSE;
    }

    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "cgroup path not found: %s (v2) / %s (v1)", path_v2,
                path_v1 ? path_v1 : "N/A");
    return FALSE;
}

   
                                  
                                                                                                                   
                                                                                      
  
                                                     
                                                  
  
                                           
                                                                            
                                                                             
                                                                                                
                                
                                    
                                                                                
   
static gboolean
_set_limits_config(const gchar *name, gint cpu_percent, gint memory_mb,
                   gint cpu_weight, gint memory_low_mb, gint memory_high_mb,
                   gint64 io_read_bps, gint pids_max, GError **error)
{
    g_autoptr(GMutexLocker) cfg_lock = g_mutex_locker_new(pcv_lxc_config_mutex());
    struct lxc_container *c = _lxc_get(name, error);
    if (!c) return FALSE;

                                                                                      
    g_autofree gchar *cpu  = cpu_percent > 0 ? g_strdup_printf("%" G_GINT64_FORMAT " 100000", (gint64)cpu_percent * 1000) : NULL;
    g_autofree gchar *mem  = memory_mb > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, (gint64)memory_mb * 1024 * 1024) : NULL;
    g_autofree gchar *wgt  = cpu_weight > 0 ? g_strdup_printf("%d", cpu_weight) : NULL;
    g_autofree gchar *low  = memory_low_mb > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, (gint64)memory_low_mb * 1024 * 1024) : NULL;
    g_autofree gchar *high = memory_high_mb > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, (gint64)memory_high_mb * 1024 * 1024) : NULL;
    g_autofree gchar *io   = io_read_bps > 0 ? g_strdup_printf("8:0 rbps=%" G_GINT64_FORMAT, io_read_bps) : NULL;
    g_autofree gchar *pids = pids_max > 0 ? g_strdup_printf("%d", pids_max) : NULL;
    const PcvLxcConfigEdit all[] = {
        { "lxc.cgroup2.cpu.max", cpu, FALSE },      { "lxc.cgroup2.memory.max", mem, FALSE },
        { "lxc.cgroup2.cpu.weight", wgt, FALSE },   { "lxc.cgroup2.memory.low", low, FALSE },
        { "lxc.cgroup2.memory.high", high, FALSE }, { "lxc.cgroup2.io.max", io, FALSE },
        { "lxc.cgroup2.pids.max", pids, FALSE },
    };
    gboolean ok = TRUE;
    if (!_lxc_handle_privileged(c)) {
        PcvLxcConfigEdit edits[G_N_ELEMENTS(all)];
        gsize n = 0;
        for (gsize i = 0; i < G_N_ELEMENTS(all); i++)
            if (all[i].value)
                edits[n++] = all[i];
        ok = pcv_lxc_userns_config_edit(PCV_LXC_PATH, name, edits, n, error);
        lxc_container_put(c);
        return ok;
    }

    if (cpu && !c->set_config_item(c, all[0].key, cpu)) {                                                      
        gchar *v1_val = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)cpu_percent * 1000);
        if (!c->set_config_item(c, "lxc.cgroup.cpu.cfs_quota_us", v1_val)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to set CPU config for '%s'", name);
            ok = FALSE;
        }
        g_free(v1_val);
    }
    if (ok && mem && !c->set_config_item(c, all[1].key, mem) &&
        !c->set_config_item(c, "lxc.cgroup.memory.limit_in_bytes", mem)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to set memory config for '%s'", name);
        ok = FALSE;
    }
    for (gsize i = 2; ok && i < G_N_ELEMENTS(all); i++)                           
        if (all[i].value && !c->set_config_item(c, all[i].key, all[i].value))
            g_warning("Failed to set %s config for '%s' (v2 only)", all[i].key + strlen("lxc.cgroup2."), name);

    if (ok) {
        gchar *cfg_path = g_strdup_printf("%s/%s/config", PCV_LXC_PATH, name);
        if (!c->save_config(c, cfg_path)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Failed to save config for '%s'", name);
            ok = FALSE;
        }
        g_free(cfg_path);
    }

    lxc_container_put(c);
    return ok;
}

                                                             
                                                         
                                 
                                                             
                                                                             
                                                                           
                                                                  
gboolean
pcv_lxc_set_resource_limits(const gchar *name, gint cpu_percent,
                             gint memory_mb, gint cpu_weight,
                             gint memory_low_mb, gint memory_high_mb,
                             gint64 io_read_bps, gint pids_max,
                             GError **error)
{
    if (!name || !*name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Container name is required");
        return FALSE;
    }

                                                     
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (!c) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' not found", name);
        return FALSE;
    }
    gboolean running = c->is_running(c);
    lxc_container_put(c);

                                             
    if (!running) {
        return _set_limits_config(name, cpu_percent, memory_mb, cpu_weight,
                                  memory_low_mb, memory_high_mb,
                                  io_read_bps, pids_max, error);
    }

                                                             
    gboolean success = TRUE;

                                                             
    if (cpu_percent > 0) {
        gchar *v2_path = g_strdup_printf(
            "/sys/fs/cgroup/lxc.payload.%s/cpu.max", name);
        gchar *v1_path = g_strdup_printf(
            "/sys/fs/cgroup/cpu/lxc/%s/cpu.cfs_quota_us", name);
        gchar *v2_val = g_strdup_printf("%" G_GINT64_FORMAT " 100000", (gint64)cpu_percent * 1000);
        gchar *v1_val = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)cpu_percent * 1000);

        if (!_cgroup_write_with_fallback(v2_path, v2_val, v1_path, v1_val, error)) {
            g_prefix_error(error, "Failed to set CPU limit for '%s': ", name);
            success = FALSE;
        }
        g_free(v2_path); g_free(v1_path);
        g_free(v2_val);  g_free(v1_val);
    }

                                               
    if (success && cpu_weight > 0) {
        gchar *v2_path = g_strdup_printf(
            "/sys/fs/cgroup/lxc.payload.%s/cpu.weight", name);
        gchar *val = g_strdup_printf("%d", cpu_weight);

        if (!_cgroup_write_with_fallback(v2_path, val, NULL, NULL, error)) {
            g_prefix_error(error, "Failed to set cpu.weight for '%s': ", name);
            success = FALSE;
        }
        g_free(v2_path);
        g_free(val);
    }

                                                  
    if (success && memory_mb > 0) {
        gchar *v2_path = g_strdup_printf(
            "/sys/fs/cgroup/lxc.payload.%s/memory.max", name);
        gchar *v1_path = g_strdup_printf(
            "/sys/fs/cgroup/memory/lxc/%s/memory.limit_in_bytes", name);
        gchar *val = g_strdup_printf("%" G_GINT64_FORMAT,
                                     (gint64)memory_mb * 1024 * 1024);

        if (!_cgroup_write_with_fallback(v2_path, val, v1_path, val, error)) {
            g_prefix_error(error, "Failed to set memory limit for '%s': ", name);
            success = FALSE;
        }
        g_free(v2_path); g_free(v1_path);
        g_free(val);
    }

                                          
    if (success && memory_low_mb > 0) {
        gchar *v2_path = g_strdup_printf(
            "/sys/fs/cgroup/lxc.payload.%s/memory.low", name);
        gchar *val = g_strdup_printf("%" G_GINT64_FORMAT,
                                     (gint64)memory_low_mb * 1024 * 1024);

        if (!_cgroup_write_with_fallback(v2_path, val, NULL, NULL, error)) {
            g_prefix_error(error, "Failed to set memory.low for '%s': ", name);
            success = FALSE;
        }
        g_free(v2_path);
        g_free(val);
    }

                                                            
    if (success && memory_high_mb > 0) {
        gchar *v2_path = g_strdup_printf(
            "/sys/fs/cgroup/lxc.payload.%s/memory.high", name);
        gchar *val = g_strdup_printf("%" G_GINT64_FORMAT,
                                     (gint64)memory_high_mb * 1024 * 1024);

        if (!_cgroup_write_with_fallback(v2_path, val, NULL, NULL, error)) {
            g_prefix_error(error, "Failed to set memory.high for '%s': ", name);
            success = FALSE;
        }
        g_free(v2_path);
        g_free(val);
    }

                                                                     
    if (success && io_read_bps > 0) {
        gchar *v2_path = g_strdup_printf(
            "/sys/fs/cgroup/lxc.payload.%s/io.max", name);
        gchar *val = g_strdup_printf("8:0 rbps=%" G_GINT64_FORMAT, io_read_bps);

        if (!_cgroup_write_with_fallback(v2_path, val, NULL, NULL, error)) {
            g_prefix_error(error, "Failed to set io.max for '%s': ", name);
            success = FALSE;
        }
        g_free(v2_path);
        g_free(val);
    }

                                              
    if (success && pids_max > 0) {
        gchar *v2_path = g_strdup_printf(
            "/sys/fs/cgroup/lxc.payload.%s/pids.max", name);
        gchar *val = g_strdup_printf("%d", pids_max);

        if (!_cgroup_write_with_fallback(v2_path, val, NULL, NULL, error)) {
            g_prefix_error(error, "Failed to set pids.max for '%s': ", name);
            success = FALSE;
        }
        g_free(v2_path);
        g_free(val);
    }

                                            
    if (success) {
        GError *cfg_err = nullptr;
        if (!_set_limits_config(name, cpu_percent, memory_mb, cpu_weight,
                                memory_low_mb, memory_high_mb,
                                io_read_bps, pids_max, &cfg_err)) {
                                                                                           
            PCV_LOG_WARN(LXC_LOG_DOM, "container '%s' limits applied but config was not saved: %s", name,
                         cfg_err ? cfg_err->message : "unknown");
            g_clear_error(&cfg_err);
        }
    }

    return success;
}

                                                                             
                                                   
                                     
  
                                                   
                                                
                              
                                                                               

                                                                                
                                                         
void
pcv_lxc_nic_info_free(PcvLxcNicInfo *nic)
{
    if (!nic) return;
    g_free(nic->name);
    g_free(nic->type);
    g_free(nic->bridge);
    g_free(nic->hwaddr);
    g_free(nic->ipv4);
    g_free(nic->veth_peer);
    g_free(nic);
}

   
                                                         
                                          
  
                                                              
  
                                                                    
                                                                           
   
GPtrArray *
pcv_lxc_nic_list(const gchar *name, GError **error)
{
    GPtrArray *nics = g_ptr_array_new_with_free_func(
        (GDestroyNotify)pcv_lxc_nic_info_free);

    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (!c) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' not found", name);
        return nics;
    }
    if (!c->is_defined(c)) {
        lxc_container_put(c);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' not defined", name);
        return nics;
    }

                                                               
    for (int idx = 0; idx < 16; idx++) {
        gchar key_type[64], key_link[64], key_hwaddr[64], key_name[64];
        g_snprintf(key_type,   sizeof(key_type),   "lxc.net.%d.type",   idx);
        g_snprintf(key_link,   sizeof(key_link),   "lxc.net.%d.link",   idx);
        g_snprintf(key_hwaddr, sizeof(key_hwaddr), "lxc.net.%d.hwaddr", idx);
        g_snprintf(key_name,   sizeof(key_name),   "lxc.net.%d.name",   idx);

        char buf[256] = {0};
        int ret = c->get_config_item(c, key_type, buf, sizeof(buf));
        if (ret <= 0 || buf[0] == '\0') break;                             

        PcvLxcNicInfo *nic = g_new0(PcvLxcNicInfo, 1);
        nic->type = g_strdup(buf);

        buf[0] = '\0';
        c->get_config_item(c, key_link, buf, sizeof(buf));
        nic->bridge = g_strdup(buf[0] ? buf : "none");

        buf[0] = '\0';
        c->get_config_item(c, key_hwaddr, buf, sizeof(buf));
        nic->hwaddr = g_strdup(buf[0] ? buf : "auto");

        buf[0] = '\0';
        c->get_config_item(c, key_name, buf, sizeof(buf));
        nic->name = g_strdup(buf[0] ? buf : g_strdup_printf("eth%d", idx));

        nic->ipv4 = g_strdup("");
        nic->veth_peer = g_strdup("");
        g_ptr_array_add(nics, nic);
    }

                                                                                             
                                                                               
    gchar *out = nullptr;
    if (c->is_running(c) && nics->len > 0 && !pcv_lxc_ns_ip4_query(PCV_LXC_PATH, name, &out, NULL))
        for (guint n = 0; n < nics->len; n++)
            ((PcvLxcNicInfo *)g_ptr_array_index(nics, n))->ipv4_unavailable = TRUE;
    gchar **lines = out ? g_strsplit(out, "\n", -1) : NULL;
    for (int i = 0; lines && lines[i]; i++) {                                                           
        for (guint n = 0; n < nics->len; n++) {
            PcvLxcNicInfo *ni = g_ptr_array_index(nics, n);
            const gchar *inet = g_strstr_len(lines[i], -1, "inet ");
            if (!g_strstr_len(lines[i], -1, ni->name))
                continue;
            if (inet) {                                                  
                const gchar *sp = strchr(inet + 5, ' ');
                g_free(ni->ipv4);
                ni->ipv4 = sp ? g_strndup(inet + 5, (gsize)(sp - inet - 5)) : g_strdup(inet + 5);
            }
            break;
        }
    }
    g_strfreev(lines);
    g_free(out);
    lxc_container_put(c);
    return nics;
}

                                                                                                
typedef void (*NicRmwHook)(const gchar *name);
static NicRmwHook G_nic_attach_rmw_hook;

                                                                               
                                             
                                                            
void
pcv_lxc_nic_attach_set_rmw_hook_for_test(void (*fn)(const gchar *name))
{
    g_atomic_pointer_set(&G_nic_attach_rmw_hook, fn);
}

   
                                                      
  
                                                    
                                     
  
                                                                      
                                                                                 
                                                                                  
   
gboolean
pcv_lxc_nic_attach(const gchar *name, const gchar *bridge,
                     const gchar *hwaddr, GError **error)
{
                                                                                     
                                                                                  
    g_autoptr(GMutexLocker) cfg_lock = g_mutex_locker_new(pcv_lxc_config_mutex());
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (!c || !c->is_defined(c)) {
        if (c) lxc_container_put(c);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' not found", name);
        return FALSE;
    }

    const gchar *br = bridge ? bridge : PCV_LXC_DEFAULT_BRIDGE;

                                                           
    int idx = 0;
    for (; idx < 16; idx++) {
        gchar key[64]; char buf[32] = {0};
        g_snprintf(key, sizeof(key), "lxc.net.%d.type", idx);
        if (c->get_config_item(c, key, buf, sizeof(buf)) <= 0 || buf[0] == '\0')
            break;                        
    }

    gchar k1[64], k2[64], k3[64], k4[64], k5[64], ethname[16];
    g_snprintf(k1, sizeof(k1), "lxc.net.%d.type",   idx);
    g_snprintf(k2, sizeof(k2), "lxc.net.%d.link",   idx);
    g_snprintf(k3, sizeof(k3), "lxc.net.%d.flags",  idx);
    g_snprintf(k4, sizeof(k4), "lxc.net.%d.name",   idx);
    g_snprintf(k5, sizeof(k5), "lxc.net.%d.hwaddr", idx);
    g_snprintf(ethname, sizeof(ethname), "eth%d", idx);
    const gboolean has_mac = hwaddr && *hwaddr;
    const PcvLxcConfigEdit edits[] = { { k1, "veth", FALSE }, { k2, br, FALSE }, { k3, "up", FALSE },
                                       { k4, ethname, FALSE }, { k5, hwaddr, FALSE } };
    const gsize n_edits = has_mac ? 5 : 4;
    NicRmwHook hook = g_atomic_pointer_get(&G_nic_attach_rmw_hook);
    if (hook)
        hook(name);                                      

                                                                                                    
                                                                                                   
    gboolean saved;
    g_autofree gchar *checked = nullptr;
    if (!_lxc_handle_privileged(c)) {
        saved = pcv_lxc_userns_config_edit(PCV_LXC_PATH, name, edits, n_edits, error);
    } else if (!(checked = pcv_lxc_userns_config_apply_edits(NULL, edits, n_edits, error))) {
        saved = FALSE;
    } else {
        for (gsize i = 0; i < n_edits; i++)
            c->set_config_item(c, edits[i].key, edits[i].value);
        if (!(saved = c->save_config(c, NULL)))
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to save config for '%s'", name);
    }
    g_clear_pointer(&cfg_lock, g_mutex_locker_free);                                          

                                                                         
                                                                                 
                                                                                   
                                                                                           
    if (saved && c->is_running(c)) {
        gchar host_if[16];                                                                         
        if (!pcv_lxc_ns_host_veth_name(name, (guint)idx, host_if)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "config saved; live wiring not confirmed: NIC index %d exceeds the limit", idx);
            saved = FALSE;
        } else {
            saved = pcv_lxc_ns_nic_wire(host_if, ethname, br, c->init_pid(c), error);
        }
    }
    lxc_container_put(c);
    return saved;
}

   
                                        
  
                                                          
                 
  
                                                                        
                                                                                      
                                                                                  
                                                                                                    
                                                
   
gboolean
pcv_lxc_nic_detach(const gchar *name, const gchar *nic_name, gchar **warning_out, GError **error)
{
    if (warning_out) *warning_out = NULL;
                                                                                                
    g_autoptr(GMutexLocker) cfg_lock = g_mutex_locker_new(pcv_lxc_config_mutex());
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (!c || !c->is_defined(c)) {
        if (c) lxc_container_put(c);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' not found", name);
        return FALSE;
    }

                        
    int target = -1;
    for (int idx = 0; idx < 16; idx++) {
        gchar key[64]; char buf[64] = {0};
        g_snprintf(key, sizeof(key), "lxc.net.%d.name", idx);
        if (c->get_config_item(c, key, buf, sizeof(buf)) <= 0) break;
        if (g_strcmp0(buf, nic_name) == 0) { target = idx; break; }
    }

    if (target < 0) {
                                                                   
        if (g_str_has_prefix(nic_name, "eth"))
            target = atoi(nic_name + 3);
    }

    if (target <= 0) {                                               
        lxc_container_put(c);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Cannot remove primary NIC (eth0) or NIC not found: %s", nic_name);
        return FALSE;
    }

                                                                               
                                                                                              
    gboolean saved;
    if (!_lxc_handle_privileged(c)) {
        gchar prefix[32];
        g_snprintf(prefix, sizeof(prefix), "lxc.net.%d.", target);
        const PcvLxcConfigEdit edits[] = { { prefix, NULL, FALSE } };
        saved = pcv_lxc_userns_config_edit(PCV_LXC_PATH, name, edits, G_N_ELEMENTS(edits), error);
    } else {
        const gchar *keys[] = {"type", "link", "flags", "name", "hwaddr",
                                "ipv4.address", "ipv4.gateway", NULL};
        for (int i = 0; keys[i]; i++) {
            gchar key[64];
            g_snprintf(key, sizeof(key), "lxc.net.%d.%s", target, keys[i]);
            c->clear_config_item(c, key);
        }
        if (!(saved = c->save_config(c, NULL)))
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to save config for '%s'", name);
    }
    g_clear_pointer(&cfg_lock, g_mutex_locker_free);                                          

                                                                               
    if (saved && c->is_running(c)) {
        saved = pcv_lxc_ns_nic_remove(PCV_LXC_PATH, name, nic_name, warning_out, error);
        if (!saved)
            saved = pcv_lxc_ns_nic_detach_host_leftover(name, (guint)target, warning_out, error);
    }
    lxc_container_put(c);
    return saved;
}

                                                                          
  
                           
                                                                                           
                                                                      
                                                               
                                                           
                                                                                     
                                                                   
                                                                 
                                                                            
                                                          
                                           
                                                                               
                                                 
                                                                 
  
                                                        
                                                        
                                                                            

#define PCV_LXC_MAX_NETS 16                                                 

                                                         
                                                           
                                                                       
                                                         
gint
pcv_lxc_bw_net_index(const gchar *const *net_names, guint n_nets, const gchar *iface)
{
    if (!net_names || !iface || !*iface)
        return -1;
    for (guint i = 0; i < n_nets; i++) {
        if (net_names[i] && net_names[i][0] && g_strcmp0(net_names[i], iface) == 0)
            return (gint)i;
    }
    if (!g_str_has_prefix(iface, "eth") || !iface[3])
        return -1;
    for (const gchar *p = iface + 3; *p; p++) {
        if (!g_ascii_isdigit(*p))
            return -1;
    }
    guint64 idx = g_ascii_strtoull(iface + 3, NULL, 10);
    if (idx >= n_nets)
        return -1;
    if (net_names[idx] && net_names[idx][0])
        return -1;                                      
    return (gint)idx;
}

                                                                                 
                                                                     
                                                     
gchar *
pcv_lxc_bw_normalize_veth(const gchar *raw)
{
    if (!raw)
        return NULL;
    gchar *veth = g_strstrip(g_strdup(raw));
    if (!pcv_validate_iface_name(veth)) {
        g_free(veth);
        return NULL;
    }
    return veth;
}

                                                                             
                                                                      
static gchar *
_lxc_bw_running_item(struct lxc_container *c, const gchar *key)
{
    char *raw = c->get_running_config_item(c, key);
    if (!raw)
        return NULL;
    gchar *copy = g_strstrip(g_strdup(raw));
    free(raw);
    return copy;
}

                                                                       
                                                                   
static gchar *
_lxc_bw_resolve_host_veth(struct lxc_container *c, const gchar *name,
                          const gchar *iface, GError **error)
{
    gchar *types[PCV_LXC_MAX_NETS] = {0};
    gchar *names[PCV_LXC_MAX_NETS] = {0};
    guint n = 0;
    for (; n < PCV_LXC_MAX_NETS; n++) {
        gchar key[64];
        g_snprintf(key, sizeof(key), "lxc.net.%u.type", n);
        types[n] = _lxc_bw_running_item(c, key);
        if (!types[n] || !types[n][0])
            break;
        g_snprintf(key, sizeof(key), "lxc.net.%u.name", n);
        names[n] = _lxc_bw_running_item(c, key);
    }

    gchar *veth = NULL;
    gint idx = pcv_lxc_bw_net_index((const gchar *const *)names, n, iface);
    if (idx < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "NIC '%s' not found in running config of container '%s'", iface, name);
    } else if (g_strcmp0(types[idx], "veth") != 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "NIC '%s' of container '%s' is type '%s', not veth", iface, name,
                    types[idx] ? types[idx] : "?");
    } else {
        gchar key[64];
        g_snprintf(key, sizeof(key), "lxc.net.%d.veth.pair", idx);
        gchar *raw = _lxc_bw_running_item(c, key);
        veth = pcv_lxc_bw_normalize_veth(raw);
        if (!veth) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Cannot resolve host-side veth for NIC '%s' of container '%s'",
                        iface, name);
        } else if (if_nametoindex(veth) == 0) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Host-side veth '%s' for container '%s' does not exist", veth, name);
            g_clear_pointer(&veth, g_free);
        }
        g_free(raw);
    }

    for (guint i = 0; i <= n && i < PCV_LXC_MAX_NETS; i++) {
        g_free(types[i]);
        g_free(names[i]);
    }
    return veth;
}

                                                                                      
#define PCV_LXC_TBF_TAIL(rate) "root", "tbf", "rate", (rate), "burst", "32kbit", "latency", "50ms"

                                                                       
                                                                  
static gboolean
_lxc_bw_apply_host(const gchar *veth, guint kbps, GError **error)
{
    gchar *rate = g_strdup_printf("%ukbit", kbps);
    const gchar *argv[] = {"tc", "qdisc", "replace", "dev", veth, PCV_LXC_TBF_TAIL(rate), NULL};
    gchar *std_err = NULL;
    GError *spawn_err = NULL;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &spawn_err);
    if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "tc on host veth '%s' failed: %s", veth,
                    std_err && *std_err ? g_strstrip(std_err)
                                        : (spawn_err ? spawn_err->message : "unknown"));
    g_clear_error(&spawn_err);
    g_free(std_err);
    g_free(rate);
    return ok;
}

                                                                    
                                                                                         
                                                        
static gboolean
_lxc_bw_apply_in_container(const gchar *name, const gchar *iface, guint kbps, GError **error)
{
    gchar *rate = g_strdup_printf("%ukbit", kbps);
    const gchar *cmd[] = {"tc", "qdisc", "replace", "dev", iface, PCV_LXC_TBF_TAIL(rate), NULL};
    gchar **argv = pcv_lxc_attach_argv(PCV_LXC_PATH, name, cmd);
    gchar *std_err = NULL;
    GError *spawn_err = NULL;
    gint status = -1;
    gboolean ok = pcv_lxc_ns_run((const gchar * const *)argv, PCV_LXC_NS_CHANGE_TIMEOUT_SEC,
                                 PCV_LXC_NS_OUTPUT_MAX, NULL, &std_err, &status, &spawn_err) && status == 0;
    g_strfreev(argv);
    if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "tc inside container '%s' on '%s' failed: %s", name, iface,
                    std_err && *std_err ? g_strstrip(std_err)
                                        : (spawn_err ? spawn_err->message : "non-zero exit"));
    g_clear_error(&spawn_err);
    g_free(std_err);
    g_free(rate);
    return ok;
}

   
                                                                  
  
                                                       
                                                 
  
                                                                          
                                                     
                                                                   
                                                             
                                                                                   
                                                                   
   
gboolean
pcv_lxc_set_bandwidth(const gchar *name, const gchar *nic_name,
                        guint inbound_kbps, guint outbound_kbps,
                        GError **error)
{
    const gchar *iface = nic_name ? nic_name : "eth0";                          
    if (!name || !*name || !pcv_validate_vm_name(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Valid container name required");
        return FALSE;
    }
    if (!pcv_validate_iface_name(iface)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid NIC name");
        return FALSE;
    }
    if (inbound_kbps == 0 && outbound_kbps == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "No bandwidth limit requested (inbound_kbps and outbound_kbps are both 0; "
                    "removing limits is not supported)");
        return FALSE;
    }

                                                                 
                                                       
    gchar *config_path = g_build_filename(PCV_LXC_PATH, name, "config", NULL);
    gboolean defined = g_file_test(config_path, G_FILE_TEST_IS_REGULAR);
    g_free(config_path);
    if (!defined) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' not found", name);
        return FALSE;
    }

    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (!c || !c->is_defined(c)) {
        if (c) lxc_container_put(c);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' not found", name);
        return FALSE;
    }
    if (!c->is_running(c)) {
        lxc_container_put(c);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Container '%s' is not running; bandwidth limits apply to live interfaces",
                    name);
        return FALSE;
    }

                                                             
                              
    gchar *host_veth = NULL;
    if (outbound_kbps > 0) {
        host_veth = _lxc_bw_resolve_host_veth(c, name, iface, error);
        if (!host_veth) {
            lxc_container_put(c);
            return FALSE;
        }
    }
    lxc_container_put(c);

    if (outbound_kbps > 0 && !_lxc_bw_apply_host(host_veth, outbound_kbps, error)) {
        g_free(host_veth);
        return FALSE;
    }

                                                                            
    if (inbound_kbps > 0) {
        GError *in_err = NULL;
        if (!_lxc_bw_apply_in_container(name, iface, inbound_kbps, &in_err)) {
            if (outbound_kbps > 0)
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "outbound limit applied on '%s' but inbound failed: %s",
                            host_veth, in_err ? in_err->message : "unknown");
            else
                g_propagate_error(error, g_steal_pointer(&in_err));
            g_clear_error(&in_err);
            g_free(host_veth);
            return FALSE;
        }
    }

    g_free(host_veth);
    return TRUE;
}

                                                                             
                           
  
                                                     
                                                       
                                                                     
                                                                               

   
                                                  
  
                                                  
                                                    
  
                                 
                                           
                                                                   
   
gboolean
pcv_lxc_checkpoint(const gchar *name, const gchar *checkpoint_dir)
{
    if (!name || !checkpoint_dir) return FALSE;

                                                          
    g_mkdir_with_parents(checkpoint_dir, 0700);

    const gchar *argv[] = {
        "lxc-checkpoint", "-P", PCV_LXC_PATH,
        "-n", name, "-D", checkpoint_dir, "-s", NULL
    };
    gchar *std_err = nullptr;
    GError *err = nullptr;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &err);

    if (ok) {
        PCV_LOG_INFO(LXC_LOG_DOM, "Checkpointed container '%s' to %s",
                     name, checkpoint_dir);
    } else {
        PCV_LOG_WARN(LXC_LOG_DOM, "Checkpoint failed for '%s': %s",
                     name, err ? err->message :
                     (std_err ? std_err : "CRIU not available"));
        if (err) g_error_free(err);
    }
    g_free(std_err);
    return ok;
}

   
                                    
  
                                                 
                                           
  
                                 
                                        
                                                     
   
gboolean
pcv_lxc_restore(const gchar *name, const gchar *checkpoint_dir)
{
    if (!name || !checkpoint_dir) return FALSE;
    g_autofree gchar *bad = nullptr;                                                                                 
    if (!pcv_lxc_volume_guard_check_start(PCV_LXC_PATH, name, &bad)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "Restore refused for '%s': volume source changed since attach: %s", name, bad);
        return FALSE;
    }

    const gchar *argv[] = {
        "lxc-checkpoint", "-P", PCV_LXC_PATH,
        "-n", name, "-D", checkpoint_dir, "-r", NULL
    };
    gchar *std_err = nullptr;
    GError *err = nullptr;
    gboolean ok = pcv_spawn_sync(argv, NULL, &std_err, &err);

    if (ok) {
        PCV_LOG_INFO(LXC_LOG_DOM, "Restored container '%s' from %s",
                     name, checkpoint_dir);
    } else {
        PCV_LOG_WARN(LXC_LOG_DOM, "Restore failed for '%s': %s",
                     name, err ? err->message :
                     (std_err ? std_err : "unknown"));
        if (err) g_error_free(err);
    }
    g_free(std_err);
    return ok;
}

                                                                             
                        
  
                                                                    
  
                                                                                     
                                                                            
                                                                               

   
                                                         
  
                                                      
  
                                       
                                                        
   
gchar *
pcv_lxc_get_seccomp_profile(const gchar *name)
{
    if (!name) return NULL;
    gchar *config_path = g_strdup_printf("%s/%s/config", PCV_LXC_PATH, name);
    gchar *contents = nullptr;
    if (!g_file_get_contents(config_path, &contents, NULL, NULL)) {
        g_free(config_path);
        return NULL;
    }
    g_free(config_path);

    gchar *result = nullptr;
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        if (g_str_has_prefix(*l, "lxc.seccomp.profile")) {
            gchar *eq = strchr(*l, '=');
            if (eq) result = g_strdup(g_strstrip(eq + 1));
            break;
        }
    }
    g_strfreev(lines);
    g_free(contents);
    return result;
}
