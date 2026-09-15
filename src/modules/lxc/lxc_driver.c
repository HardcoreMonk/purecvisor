   
                     
                                                    
  
                           
                                                   
                                                    
                                        
  
          
                                          
                                              
                                       
                                    
                                          
                                                   
  
          
                                                              
                                                               
                                                   
  
            
                                              
                                                                 
                                                          
                                                
  
                       
                                                                      
                                                               
                                        
                                                                   
                                        
                                                 
                                                          
                                                         
                                               
                                                                      
                                                
  
          
                                                               
                                                                              
                                                    
                                                      
                                                        
  
                              
                                            
           
                                                                    
                                          
                                          
                                                 
                                           
                                                                
                                           
                                                 
  
                             
                                                      
                                                 
                                                           
                                                  
  
                                   
                                                    
                                                         
                                                                 
                                                             
                                                          
  
                                            
                                                           
                                                   
                                           
                                               
                                                        
                                                    
  
                                    
                                                   
                            
                                          
                                                 
                                                     
                                                               
  
         
                                                                
                                             
                                                       
                                           
  
                        
         
                                                                        
                                                                  
                                                              
                                                                          
                                                              
                                                            
         
                                                                   
                                                                          
                              
                                                     
                      
                                                                
                                                                             
                                                                      
                                                                  
                                                                          
   
                                                                                

#include "api/drain.h"
#include "lxc_driver.h"
#include "lxc_storage.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ftw.h>
#include <sys/stat.h>
#include "utils/pcv_spawn.h"
#include "utils/pcv_config.h"
#include "utils/pcv_validate.h"
#include "utils/pcv_log.h"

                         
#include <lxc/lxccontainer.h>

#define LXC_LOG_DOM "lxc_driver"

static gboolean _container_stopped(const gchar *name, GError **error);
static gboolean _btrfs_config_ready(const gchar *name, gboolean copying, GError **error);
static gboolean _snap_storage_begin(const gchar *name, PcvLxcStorage *storage, GError **error);
static void _snap_storage_end(const gchar *name, PcvLxcStorage *storage);


                                                                             
                                         
  
                                                 
                                                
                         
  
                                                        
                             
  
                                                        
                                    
                                                        
                                                                               

                                                  
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

struct _PcvLxcOperationGuard {
    gchar *name;
};

PcvLxcOperationGuard *
pcv_lxc_operation_guard_acquire(const gchar *name, GError **error)
{
    if (!name || !*name) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Container name is required");
        return NULL;
    }
    if (!_lock_container_op(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Container '%s' has an operation in progress", name);
        return NULL;
    }
    PcvLxcOperationGuard *guard = g_new0(PcvLxcOperationGuard, 1);
    guard->name = g_strdup(name);
    return guard;
}

void
pcv_lxc_operation_guard_free(PcvLxcOperationGuard *guard)
{
    if (!guard) return;
    _unlock_container_op(guard->name);
    g_free(guard->name);
    g_free(guard);
}

                                                                             
                         
  
                                                     
                                                 
                                            
                                                           
                                        
  
                                                   
                                                                 
  
                                                          
                                            
  
                                                     
                                                  
                                                                               

   
                                                          
                                                    
  
                                                   
                                                   
  
                                             
                       
                    
                                                          
                                                            
   
static gboolean
_rootless_check_subid(const gchar *path, gint uid_start, gint uid_count)
{
    gchar *contents = nullptr;
    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return FALSE;                                             

    gboolean found = FALSE;
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (gint i = 0; lines[i]; i++) {
                                                             
        gchar **fields = g_strsplit(lines[i], ":", 3);                        
        if (fields[0] && fields[1] && fields[2]) {
            gint64 start = g_ascii_strtoll(fields[1], NULL, 10);
            gint64 count = g_ascii_strtoll(fields[2], NULL, 10);
                                                                                
                                                 
            if (start <= uid_start && start + count >= uid_start + uid_count) {
                found = TRUE;
                g_strfreev(fields);
                break;                         
            }
        }
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(contents);
    return found;
}

   
                                                            
  
                                                
                                               
                                   
  
                                                    
                                                                 
  
                                                      
                                        
                               
                                                               
                                                               
   
static gboolean
_rootless_apply_config(struct lxc_container *c, gint uid_start, gint uid_count)
{
                                                                    
    if (!_rootless_check_subid("/etc/subuid", uid_start, uid_count)) {
        PCV_LOG_WARN(LXC_LOG_DOM,
                     "rootless: /etc/subuid missing range %d:%d, falling back to privileged",
                     uid_start, uid_count);
        return FALSE;
    }
    if (!_rootless_check_subid("/etc/subgid", uid_start, uid_count)) {
        PCV_LOG_WARN(LXC_LOG_DOM,
                     "rootless: /etc/subgid missing range %d:%d, falling back to privileged",
                     uid_start, uid_count);
        return FALSE;
    }

                              
                                                         
                                                                 
    gchar *uid_map = g_strdup_printf("u 0 %d %d", uid_start, uid_count);
    gchar *gid_map = g_strdup_printf("g 0 %d %d", uid_start, uid_count);

    gboolean ok = TRUE;
                                                                
    if (!c->set_config_item(c, "lxc.idmap", uid_map)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "rootless: failed to set lxc.idmap (uid)");
        ok = FALSE;
    }
    if (ok && !c->set_config_item(c, "lxc.idmap", gid_map)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "rootless: failed to set lxc.idmap (gid)");
        ok = FALSE;
    }

    if (ok) {
                                                       
        c->set_config_item(c, "lxc.init.uid", "0");
        c->set_config_item(c, "lxc.init.gid", "0");
        PCV_LOG_INFO(LXC_LOG_DOM,
                     "rootless: applied idmap u/g 0 %d %d", uid_start, uid_count);
    }

    g_free(uid_map);
    g_free(gid_map);
    return ok;
}

                                                                             
          
  
                              
                                                   
                                           
                                                            
                                                     
                                       
                                                                               

   
                                                          
  
                                                       
                                          
  
                           
                                                   
                                        
                                                                          
  
                       
                                                                                     
                                                                       
   
static gboolean
_container_config_file_valid(const gchar *name, GError **error)
{
    gchar *path = g_build_filename(PCV_LXC_PATH, name, "config", NULL);
    struct stat st;
    gboolean ok = lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
                  !(st.st_mode & 0022) && st.st_nlink == 1;
    if (!ok)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "Container '%s' config must be a trusted regular file under %s", name, path);
    g_free(path);
    return ok;
}

static struct lxc_container *
_lxc_get(const gchar *name, GError **error)
{
    if (!_container_config_file_valid(name, error)) return NULL;
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

   
                                                           
  
                                                           
                                                          
  
                                                               
                                 
  
                      
                                                               
                                           
                                       
  
                                                      
                                                             
   
static guint64
_cgroup_read_u64(const gchar *cgroup_path, const gchar *filename)
{
                                                        
    gchar *path_v2 = g_strdup_printf("/sys/fs/cgroup/lxc/%s/%s",
                                     cgroup_path, filename);
                                                               
    gchar *path_v1 = g_strdup_printf("/sys/fs/cgroup/memory/lxc/%s/%s",
                                     cgroup_path, filename);

    guint64 value = 0;
    gchar  *content = nullptr;

    if (g_file_get_contents(path_v2, &content, NULL, NULL) ||
        g_file_get_contents(path_v1, &content, NULL, NULL)) {
        value = (guint64)g_ascii_strtoull(g_strstrip(content), NULL, 10);
        g_free(content);
    }
    g_free(path_v2);
    g_free(path_v1);
    return value;
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

   
                      
                                               
                                         
  
                                                 
                                                    
  
                                                         
   
static void
_ensure_zfs_mounts(void)
{
    gchar *binary = g_find_program_in_path("zfs");
    if (!binary) return;
    g_free(binary);
    const gchar *argv[] = {
        "zfs", "list", "-H", "-t", "filesystem", "-o", "name,mountpoint,mounted", NULL
    };
    GError *error = NULL;
    gchar *output = _run_argv_capture(argv, &error);
    g_clear_error(&error);
    if (!output) return;
    gchar *prefix = g_strconcat(PCV_LXC_PATH, "/", NULL);
    gchar **lines = g_strsplit(output, "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        gchar **fields = g_strsplit(lines[i], "\t", 3);
        if (g_strv_length(fields) == 3 && g_str_equal(fields[2], "no") &&
            g_str_has_prefix(fields[1], prefix)) {
            gchar *relative = g_strdup(fields[1] + strlen(prefix));
            gboolean rootfs_mount = g_str_has_suffix(relative, "/rootfs");
            if (rootfs_mount) relative[strlen(relative) - 7] = '\0';
            if (pcv_validate_vm_name(relative) &&
                pcv_lxc_storage_zfs_mount_allowed(relative, fields[0], rootfs_mount)) {
                const gchar *mount_argv[] = { "zfs", "mount", fields[0], NULL };
                _run_argv(mount_argv, &error);
                g_clear_error(&error);
            }
            g_free(relative);
        }
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(prefix);
    g_free(output);
}

gboolean
pcv_lxc_ensure_config_ready(const gchar *name, gchar **out_config_path, GError **error)
{
    if (!pcv_validate_vm_name(name)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Invalid container name");
        return FALSE;
    }
    gchar *path = g_build_filename(PCV_LXC_PATH, name, "config", NULL);
    _ensure_zfs_mounts();
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR) || g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Container '%s' config is not visible under %s", name, path);
        g_free(path);
        return FALSE;
    }
    if (!_container_config_file_valid(name, error)) {
        g_free(path);
        return FALSE;
    }
    PcvLxcStorage storage = {0};
    if (!pcv_lxc_storage_resolve(name, &storage, error)) {
        g_free(path);
        return FALSE;
    }
    pcv_lxc_storage_clear(&storage);
    if (out_config_path) *out_config_path = path;
    else g_free(path);
    return TRUE;
}

GPtrArray *
pcv_lxc_list(GError **error)
{
                                 
    _ensure_zfs_mounts();

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

                           
    m->mem_used_bytes  = _cgroup_read_u64(name, "memory.current");
    if (m->mem_used_bytes == 0)                        
        m->mem_used_bytes = _cgroup_read_u64(name, "memory.usage_in_bytes");

    m->mem_limit_bytes = _cgroup_read_u64(name, "memory.max");
    if (m->mem_limit_bytes == 0)
        m->mem_limit_bytes = _cgroup_read_u64(name, "memory.limit_in_bytes");

                                                               
    m->cpu_time_ns = _cgroup_read_u64(name, "cpuacct.usage");
    if (m->cpu_time_ns == 0) {
                                                           
        gchar *path = g_strdup_printf("/sys/fs/cgroup/lxc/%s/cpu.stat", name);
        gchar *content = nullptr;
        if (g_file_get_contents(path, &content, NULL, NULL)) {
            gchar **lines = g_strsplit(content, "\n", -1);
            for (int i = 0; lines[i]; i++) {
                if (g_str_has_prefix(lines[i], "usage_usec")) {
                                                                           
                    guint64 usec = g_ascii_strtoull(lines[i] + 11, NULL, 10);
                    m->cpu_time_ns = usec * 1000;               
                    break;
                }
            }
            g_strfreev(lines);
            g_free(content);
        }
        g_free(path);
    }

                                                                       
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
    gchar *owner_sub;
    gint   rootless;                                                    
} LxcCreateData;

                                                    
static void
_lxc_create_data_free(LxcCreateData *d)
{
    g_free(d->owner_sub);
    g_free(d->name);
    g_free(d->image);
    g_free(d->bridge);
    g_free(d);
}

                                                     
                                                        
                                                                          
                                                                 
                                                        
static gboolean
_configure_create_cpu(struct lxc_container *container, guint vcpu_count)
{
    guint count = MAX(vcpu_count, 1u);
    g_autofree gchar *shares = g_strdup_printf("%u", MIN(count, 256u) * 1024u);
    g_autofree gchar *weight = g_strdup_printf("%u", MIN(count, 100u) * 100u);
    gboolean legacy = container->set_config_item(container, "lxc.cgroup.cpu.shares", shares);
    gboolean unified = container->set_config_item(container, "lxc.cgroup2.cpu.weight", weight);
    return legacy || unified;
}

static void
_lxc_create_locked(GTask        *task,
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
    PcvLxcStorageKind backend;
    if (!pcv_lxc_storage_default(&backend, &error) ||
        !pcv_lxc_storage_prepare_create(d->name, backend, &error)) {
        g_task_return_error(task, error);
        g_strfreev(parts);
        return;
    }
    const gchar *rootless_cfg = pcv_config_get_string("container", "rootless", "false");
    gboolean want_rootless = d->rootless >= 0 ? d->rootless == 1 :
        (g_ascii_strcasecmp(rootless_cfg, "true") == 0 ||
         g_ascii_strcasecmp(rootless_cfg, "yes") == 0 || g_str_equal(rootless_cfg, "1"));
    if (backend == PCV_LXC_STORAGE_BTRFS && want_rootless) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "Btrfs rootless containers are not supported; use rootless=false");
        g_strfreev(parts);
        return;
    }
    gchar *zfs_dataset  = g_strdup_printf("%s/%s", PCV_LXC_ZFS_BASE, d->name);
    gchar *mountpoint   = g_strdup_printf("%s/%s", PCV_LXC_PATH, d->name);
    if (backend == PCV_LXC_STORAGE_ZFS) {
        const gchar *parent_probe[] = {
            "zfs", "list", "-H", "-o", "name", PCV_LXC_ZFS_BASE, NULL
        };
        GError *parent_error = nullptr;
        if (!_run_argv(parent_probe, &parent_error)) {
            g_clear_error(&parent_error);
            const gchar *parent_create[] = {
                "zfs", "create", "-p", PCV_LXC_ZFS_BASE, NULL
            };
            if (!_run_argv(parent_create, &parent_error)) {
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                        "ZFS container parent create failed for '%s': %s",
                                        PCV_LXC_ZFS_BASE,
                                        parent_error ? parent_error->message : "unknown");
                g_clear_error(&parent_error);
                g_free(mountpoint); g_free(zfs_dataset); g_strfreev(parts);
                return;
            }
        }
        const gchar *zfs_argv[] = {
            "zfs", "create", "-o",
            g_strdup_printf("mountpoint=%s", mountpoint),
            zfs_dataset, NULL
        };
        if (!_run_argv(zfs_argv, &error)) {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "ZFS create failed for '%s': %s",
                                    d->name,
                                    error ? error->message : "unknown");
            g_error_free(error);
            g_free((gpointer)zfs_argv[3]);
            g_free(mountpoint); g_free(zfs_dataset); g_strfreev(parts);
            return;
        }
        g_free((gpointer)zfs_argv[3]);
        {
            const gchar *mount_argv[] = { "zfs", "mount", zfs_dataset, NULL };
            GError *mnt_err = nullptr;
            _run_argv(mount_argv, &mnt_err);
            if (mnt_err) g_error_free(mnt_err);                    
        }
    }
    error = nullptr;
    {
        const gchar *lxc_argv[] = {
            "lxc-create", "-P", PCV_LXC_PATH,
            "-n", d->name,
            "-t", "download",
            "--", "-d", distro, "-r", release, "-a", "amd64",
            NULL
        };
        const gchar *btrfs_argv[] = {
            "lxc-create", "-P", PCV_LXC_PATH, "-n", d->name,
            "-B", "btrfs", "-t", "download", "--", "-d", distro,
            "-r", release, "-a", "amd64", NULL
        };
        if (!_run_argv(backend == PCV_LXC_STORAGE_BTRFS ? btrfs_argv : lxc_argv, &error)) {
            PCV_LOG_WARN("lxc", "Container '%s' lxc-create FAILED (distro=%s release=%s): %s",
                         d->name, distro, release,
                         error ? error->message : "(no error)");
            const gchar *rb_argv[] = {
                "zfs", "destroy", "-r", zfs_dataset, NULL
            };
            GError *rb_err = nullptr;
            if (backend == PCV_LXC_STORAGE_ZFS) _run_argv(rb_argv, &rb_err);
            if (rb_err) g_error_free(rb_err);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "lxc-create failed for '%s': %s",
                                    d->name,
                                    error ? error->message : "unknown");
            g_error_free(error);
            g_free(mountpoint); g_free(zfs_dataset); g_strfreev(parts);
            return;
        }
    }
    if (!pcv_lxc_storage_record(d->name, backend,
                                backend == PCV_LXC_STORAGE_ZFS ? zfs_dataset : NULL, &error)) {
        g_prefix_error(&error, "Created container '%s' needs storage metadata recovery: ", d->name);
        g_task_return_error(task, error);
        goto cleanup;
    }
    struct lxc_container *c = lxc_container_new(d->name, PCV_LXC_PATH);
    if (!c || !c->is_defined(c)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Cannot open container '%s' after create", d->name);
        if (c) lxc_container_put(c);
        goto cleanup;
    }
    guint memory_mb = (d->memory_mb > 0) ? d->memory_mb : 512;
    gchar *mem_val  = g_strdup_printf("%uM", memory_mb);
    c->set_config_item(c, "lxc.cgroup.memory.limit_in_bytes", mem_val);
    c->set_config_item(c, "lxc.cgroup2.memory.max",           mem_val);
    g_free(mem_val);
    if (!_configure_create_cpu(c, d->vcpu_count)) {
        lxc_container_put(c);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Cannot configure container CPU weight for cgroup v1 or v2");
        goto cleanup;
    }
    c->clear_config_item(c, "lxc.net");                                       
    c->set_config_item(c, "lxc.net.0.type",    "veth");
    c->set_config_item(c, "lxc.net.0.link",    d->bridge ? d->bridge : PCV_LXC_DEFAULT_BRIDGE);
    c->set_config_item(c, "lxc.net.0.flags",   "up");
    c->set_config_item(c, "lxc.net.0.hwaddr",  "00:16:3e:xx:xx:xx");                 
    {
        if (want_rootless) {
            gint uid_start = pcv_config_get_int("container", "rootless_uid_start", 100000);
            gint uid_count = pcv_config_get_int("container", "rootless_uid_count", 65536);
            if (!_rootless_apply_config(c, uid_start, uid_count)) {
                PCV_LOG_WARN(LXC_LOG_DOM,
                             "rootless setup failed for '%s', continuing as privileged",
                             d->name);
            }
        }
    }
    {
        gchar *image_tag = g_strdup_printf("%s:%s", distro, release);
        gchar *meta_path = g_strdup_printf("%s/%s/purecvisor.meta",
                                            PCV_LXC_PATH, d->name);
        gboolean meta_ok = g_file_set_contents_full(meta_path, image_tag, -1, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, &error);
        g_free(meta_path);
        g_free(image_tag);
        if (!meta_ok) {
            lxc_container_put(c);
            g_task_return_error(task, error);
            goto cleanup;
        }
    }
    if (!c->save_config(c, NULL)) {
        lxc_container_put(c);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Failed to persist created container '%s' config", d->name);
        goto cleanup;
    }
    lxc_container_put(c);
    if (d->owner_sub && !pcv_lxc_stamp_owner(d->name, d->owner_sub)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Created container exists but owner metadata could not be persisted");
        goto cleanup;
    }
    g_task_return_boolean(task, TRUE);
cleanup:
    g_strfreev(parts);
    g_free(mountpoint);
    g_free(zfs_dataset);
}
                                                                                          
static void
_lxc_create_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    LxcCreateData *d = data;
    if (!_lock_container_op(d->name)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY,
                                "Container '%s' has a concurrent operation in progress", d->name);
        return;
    }
    _lxc_create_locked(task, source, data, cancel);
    _unlock_container_op(d->name);
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
                              network_bridge, -1, NULL,
                              cancellable, callback, user_data);
}

                                                          
                                                          
                                                           
                                                          
                                                                     
void
pcv_lxc_create_async_full(const gchar        *name,
                          const gchar        *image,
                          guint               memory_mb,
                          guint               vcpu_count,
                          const gchar        *network_bridge,
                          gint                rootless,
                          const gchar        *owner_sub,
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
    data->owner_sub     = g_strdup(owner_sub);
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

                         
    _ensure_zfs_mounts();
    PcvLxcStorage storage = {0};
    if (!pcv_lxc_storage_resolve_for_destroy(name, &storage, &error)) {
        g_clear_error(&error);
        if (!_container_stopped(name, &error) || !_btrfs_config_ready(name, FALSE, &error) ||
            !pcv_lxc_storage_recover(name, &error) ||
            !pcv_lxc_storage_resolve_for_destroy(name, &storage, &error)) {
            _unlock_container_op(name);
            g_task_return_error(task, error);
            return;
        }
    }
    struct lxc_container *c = lxc_container_new(name, PCV_LXC_PATH);
    if (c && c->is_defined(c) && c->is_running(c)) {
        if (!c->stop(c)) {
            lxc_container_put(c);
            pcv_lxc_storage_clear(&storage);
            _unlock_container_op(name);
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Failed to stop container '%s'; storage was preserved", name);
            return;
        }
    }
    if (c) lxc_container_put(c);
    if (storage.kind == PCV_LXC_STORAGE_BTRFS) {
        gboolean ok = pcv_lxc_storage_btrfs_destroy(name, &error);
        pcv_lxc_storage_clear(&storage);
        _unlock_container_op(name);
        if (ok) g_task_return_boolean(task, TRUE);
        else g_task_return_error(task, error);
        return;
    }

                                                   
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
                pcv_lxc_storage_clear(&storage);
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
        gchar *zfs_target = g_strdup(storage.dataset);
        const gchar *argv[] = { "zfs", "destroy", "-r", zfs_target, NULL };
        GError *zfs_err = nullptr;
        if (!_run_argv(argv, &zfs_err)) {
                                            
            g_warning("lxc_driver: ZFS destroy failed for '%s': %s",
                      name, zfs_err ? zfs_err->message : "unknown");
            if (zfs_err) g_error_free(zfs_err);
        }
        g_free(zfs_target);
    }

                                                     
    {
        gchar *container_dir = g_strdup_printf("%s/%s", PCV_LXC_PATH, name);
        if (g_file_test(container_dir, G_FILE_TEST_IS_DIR) && g_rmdir(container_dir) != 0) {
                                                               
            PCV_LOG_WARN(LXC_LOG_DOM,
                         "Post-destroy rmdir failed for '%s': %s",
                         container_dir, g_strerror(errno));
        }
        g_free(container_dir);
    }

                                                                     
                                       
    {
        gchar *container_dir = g_strdup_printf("%s/%s", PCV_LXC_PATH, name);
        if (g_file_test(container_dir, G_FILE_TEST_EXISTS)) {
            nftw(container_dir, _pcv_recursive_unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
        }
        g_free(container_dir);
    }

    pcv_lxc_storage_clear(&storage);
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
    gchar *owner_sub;
} CloneCtx;

                                                                    
                                                                   
                     
                                                
static void _clone_ctx_free(gpointer p) {
    CloneCtx *ctx = p;
    g_free(ctx->owner_sub);
    g_free(ctx->source);
    g_free(ctx->target);
    g_free(ctx);
}

                                                      
                                      
                                                           
                                                        
static void
_clone_worker(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    CloneCtx *ctx = data;
    GError *error = NULL;
    PcvLxcStorage storage = {0};
    if (!_snap_storage_begin(ctx->source, &storage, &error)) {
        g_task_return_error(task, error);
        return;
    }
    if (!_lock_container_op(ctx->target)) {
        _snap_storage_end(ctx->source, &storage);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY,
                                "target container has a concurrent operation in progress");
        return;
    }
    gboolean ok = FALSE;
    if (!pcv_lxc_storage_prepare_create(ctx->target, storage.kind, &error)) goto done;
    if (storage.kind == PCV_LXC_STORAGE_BTRFS &&
        !pcv_lxc_storage_btrfs_validate_copy(ctx->source, &error)) goto done;
    const gchar *btrfs_argv[] = {
        "lxc-copy", "-P", PCV_LXC_PATH, "-n", ctx->source,
        "-N", ctx->target, "-B", "btrfs", "-s", NULL
    };
    const gchar *zfs_argv[] = {
        "lxc-copy", "-P", PCV_LXC_PATH, "-n", ctx->source,
        "-N", ctx->target, "-B", "zfs", NULL
    };
    if (!_run_argv(storage.kind == PCV_LXC_STORAGE_BTRFS ? btrfs_argv : zfs_argv,
                   &error)) goto done;
    if (!pcv_lxc_storage_record(ctx->target, storage.kind, NULL, &error)) goto done;
    gchar *owner_path = g_build_filename(PCV_LXC_PATH, ctx->target, "purecvisor.owner", NULL);
    if (g_unlink(owner_path) != 0 && errno != ENOENT) {
        g_set_error(&error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "Failed to clear cloned owner metadata: %s", g_strerror(errno));
        g_free(owner_path);
        goto done;
    }
    g_free(owner_path);
    gchar *meta_from = g_build_filename(PCV_LXC_PATH, ctx->source, "purecvisor.meta", NULL);
    gchar *meta_to = g_build_filename(PCV_LXC_PATH, ctx->target, "purecvisor.meta", NULL);
    gchar *image = NULL;
    if (g_file_get_contents(meta_from, &image, NULL, &error))
        ok = g_file_set_contents_full(meta_to, image, -1, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, &error);
    else if (g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        g_clear_error(&error);
        ok = g_file_set_contents_full(meta_to, "unknown", -1, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, &error);
    }
    g_free(image);
    g_free(meta_from);
    g_free(meta_to);
    if (ok && ctx->owner_sub && !pcv_lxc_stamp_owner(ctx->target, ctx->owner_sub)) {
        ok = FALSE;
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Cloned container exists but owner metadata could not be persisted");
    }
done:
    _unlock_container_op(ctx->target);
    _snap_storage_end(ctx->source, &storage);
    if (ok) g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, error);
}

void
pcv_lxc_clone_async(const gchar *source, const gchar *target, const gchar *owner_sub,
                    GCancellable *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    GTask *task = pcv_drain_task_new(NULL, cancellable, callback, user_data);
    if (!source || !target || !*source || !*target) {                             
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "source and target are required");
        g_object_unref(task);
        return;
    }

    CloneCtx *ctx = g_new0(CloneCtx, 1);
    ctx->source = g_strdup(source);
    ctx->target = g_strdup(target);
    ctx->owner_sub = g_strdup(owner_sub);
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
    pcv_lxc_clone_async(source, target, NULL, NULL, NULL, NULL);
    return TRUE;
}

                                                                             
                                   
  
                                                  
                                                               
  
                                                         
                                 
  
                                                  
                                                                               

                                                                  
                                                                          
                                                                                 
static void
_apply_cgroup_limits(const gchar *name, gint cpu_percent, gint64 memory_mb)
{
    if (cpu_percent > 0 && cpu_percent <= 100) {                                
                                                                         
        gint quota = cpu_percent * 1000;                                       
        gchar *cpu_val  = g_strdup_printf("%d 100000", quota);
        gchar *cpu_path = g_strdup_printf("/sys/fs/cgroup/lxc.payload.%s/cpu.max", name);

        if (g_file_test(cpu_path, G_FILE_TEST_EXISTS)) {
            g_file_set_contents(cpu_path, cpu_val, -1, NULL);
            PCV_LOG_INFO(LXC_LOG_DOM, "Set CPU limit for '%s': %d%%", name, cpu_percent);
        }
        g_free(cpu_val);
        g_free(cpu_path);
    }

    if (memory_mb > 0) {
                                                       
        gchar *mem_val  = g_strdup_printf("%" G_GINT64_FORMAT, memory_mb * 1024 * 1024);
        gchar *mem_path = g_strdup_printf("/sys/fs/cgroup/lxc.payload.%s/memory.max", name);

        if (g_file_test(mem_path, G_FILE_TEST_EXISTS)) {
            g_file_set_contents(mem_path, mem_val, -1, NULL);
            PCV_LOG_INFO(LXC_LOG_DOM, "Set memory limit for '%s': %" G_GINT64_FORMAT "MB",
                         name, memory_mb);
        }
        g_free(mem_val);
        g_free(mem_path);
    }
}

                                                                             
                                                     
  
                                                     
                                                         
                                          
  
                                                  
                                            
                                                       
                                   
  
                                                    
                                                
                                     
  
           
                                                        
                                               
  
        
                                               
                                                                               

                                            
                                                            
                                                             
static void
_lxc_start_locked(GTask        *task,
                  gpointer      source_object __attribute__((unused)),
                  gpointer      task_data,
                  GCancellable *cancellable __attribute__((unused)))
{
    const gchar *name = (const gchar *)task_data;
    GError      *error = nullptr;
    if (!pcv_lxc_ensure_config_ready(name, NULL, &error)) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED) ||
            g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
            g_task_return_error(task, error);
            return;
        }
        g_clear_error(&error);
    }
    struct lxc_container *c = _lxc_get(name, &error);
    if (!c) { g_task_return_error(task, error); return; }                                
    PcvLxcStorage storage = {0};
    if (!pcv_lxc_storage_resolve(name, &storage, &error)) {
        if (c->is_running(c)) {
            lxc_container_put(c);
            g_task_return_error(task, error);
            return;
        }
        g_clear_error(&error);
        if (!_btrfs_config_ready(name, FALSE, &error) ||
            !pcv_lxc_storage_recover(name, &error) ||
            !pcv_lxc_storage_resolve(name, &storage, &error)) {
            lxc_container_put(c);
            g_task_return_error(task, error);
            return;
        }
    }
    if (storage.kind == PCV_LXC_STORAGE_BTRFS && !_btrfs_config_ready(name, FALSE, &error)) {
        pcv_lxc_storage_clear(&storage);
        lxc_container_put(c);
        g_task_return_error(task, error);
        return;
    }
    pcv_lxc_storage_clear(&storage);
    if (c->is_running(c)) {                                     
        g_message("lxc_driver: container '%s' is already running", name);
        lxc_container_put(c);
        g_task_return_boolean(task, TRUE);
        return;
    }
    lxc_container_put(c);                                        
    const gchar *start_argv[] = {
        "lxc-start", "-P", PCV_LXC_PATH, "-n", name, "-d", NULL
    };
    gchar *start_out = nullptr;
    gchar *start_err = nullptr;
    if (!pcv_spawn_sync(start_argv, &start_out, &start_err, &error)) {
        const gchar *detail = (start_err && *start_err) ? g_strstrip(start_err)
                            : (start_out && *start_out) ? g_strstrip(start_out)
                            : (error ? error->message : "unknown");
        GError *rich = nullptr;
        g_set_error(&rich, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "lxc-start failed for '%s': %s", name, detail);
        if (error) g_error_free(error);
        g_task_return_error(task, rich);
    } else {
        struct lxc_container *cc = lxc_container_new(name, PCV_LXC_PATH);
        if (cc && cc->is_defined(cc)) {
            char mem_buf[64] = {0};
            gint64 mem_mb = 0;
            if (cc->get_config_item(cc, "lxc.cgroup2.memory.max", mem_buf, sizeof(mem_buf)) > 0) {
                gint64 val = g_ascii_strtoll(mem_buf, NULL, 10);
                if (val > 0) mem_mb = val;                                            
            }
            char cpu_buf[64] = {0};
            gint cpu_pct = 0;
            if (cc->get_config_item(cc, "lxc.cgroup.cpu.shares", cpu_buf, sizeof(cpu_buf)) > 0) {
                gint shares = (gint)g_ascii_strtoll(cpu_buf, NULL, 10);
                if (shares > 0) cpu_pct = (shares * 100) / 1024;
                if (cpu_pct > 100) cpu_pct = 100;
            }
            lxc_container_put(cc);
            if (cpu_pct > 0 || mem_mb > 0)
                _apply_cgroup_limits(name, cpu_pct, mem_mb);
        } else {
            if (cc) lxc_container_put(cc);
        }
        g_task_return_boolean(task, TRUE);
    }
    g_free(start_out);
    g_free(start_err);
}
                                                    
static void
_lxc_start_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    const gchar *name = data;
    if (!_lock_container_op(name)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY,
                                "Container '%s' has a concurrent operation in progress", name);
        return;
    }
    _lxc_start_locked(task, source, data, cancel);
    _unlock_container_op(name);
}

void
pcv_lxc_start_async(const gchar        *name,
                    GCancellable       *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer            user_data)
{
    GTask *task = pcv_drain_task_new(NULL, cancellable, callback, user_data);
    g_task_set_task_data(task, g_strdup(name), (GDestroyNotify)g_free);
    g_task_run_in_thread(task, _lxc_start_thread);
    g_object_unref(task);
}

                                                                         
gboolean pcv_lxc_start_finish(GAsyncResult *result, GError **error)
{ return g_task_propagate_boolean(G_TASK(result), error); }

                                                                             
                                                         
  
                                                        
                                                    
                            
  
                   
                                                                    
                                                    
                                                          
  
        
                                            
                                                                               

   
                                  
  
                                                      
                                                                       
                                                     
                                                   
                          
   
typedef struct { gchar *name; gboolean force; } LxcStopData;                           
static void _lxc_stop_data_free(gpointer p) { LxcStopData *d = p; g_free(d->name); g_free(d); }

                                           
                                                        
                                                                    
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

    struct lxc_container *c = _lxc_get(d->name, &error);
    if (!c) { _unlock_container_op(d->name); g_task_return_error(task, error); return; }

    if (!c->is_running(c)) {
        g_message("lxc_driver: container '%s' is already stopped", d->name);
        lxc_container_put(c);
        _unlock_container_op(d->name);
        g_task_return_boolean(task, TRUE);
        return;
    }

    gboolean ok;
    if (d->force) {
                                              
        ok = c->stop(c);
    } else {
                                                         
        ok = c->shutdown(c, 30);
        if (!ok) {
            g_warning("lxc_driver: graceful shutdown timed out for '%s', forcing stop",
                      d->name);
            ok = c->stop(c);                             
        }
    }

    if (!ok) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "liblxc: stop failed for '%s': %s",
                    d->name, c->error_string);
        g_task_return_error(task, error);
    } else {
        g_task_return_boolean(task, TRUE);
    }
    lxc_container_put(c);
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

                                                                             
                                          
  
                                                  
                                                 
                                                     
  
                                            
                                                      
                                                   
                                             
  
                                                         
                                              
  
          
                                               
                           
                                                                               

                                                                    
static gboolean
_cancel_on_timeout(gpointer user_data)
{
    g_cancellable_cancel(G_CANCELLABLE(user_data));
    return G_SOURCE_REMOVE;
}

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

                                                      
                                                                
                                       
    gchar *q_name = g_shell_quote(d->name);
    GString *cmd = g_string_new("lxc-attach --clear-env");                                   
    g_string_append_printf(cmd, " -P %s -n %s --", PCV_LXC_PATH, q_name);
    g_free(q_name);
    for (int i = 0; d->argv[i]; i++) {                                    
        gchar *q_arg = g_shell_quote(d->argv[i]);
        g_string_append_printf(cmd, " %s", q_arg);
        g_free(q_arg);
    }

                                         
                                                        
    const gchar *exec_argv[] = {"/bin/sh", "-c", cmd->str, NULL};
    gchar *stdout_out = nullptr;
    gchar *stderr_out = nullptr;

    GSubprocess *proc = pcv_spawn_newv_profile(
        exec_argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
        PCV_CHILD_CAP_RUNTIME,
        &error);
    g_string_free(cmd, TRUE);

    if (!proc) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "exec spawn failed: %s",
                                error ? error->message : "unknown");
        if (error) g_error_free(error);
        return;
    }

                                                                   
    GCancellable *cancel = g_cancellable_new();
    guint timer_id = g_timeout_add_seconds(60, _cancel_on_timeout, cancel);
    gboolean ok = g_subprocess_communicate_utf8(
        proc, NULL, cancel, &stdout_out, &stderr_out, &error);
    g_source_remove(timer_id);                               
    g_object_unref(cancel);

    if (!ok) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "exec failed (timeout or error): %s",
                                error ? error->message : "unknown");
        if (error) g_error_free(error);
        g_free(stdout_out); g_free(stderr_out);
        g_object_unref(proc);
        return;
    }

    gboolean success = g_subprocess_get_successful(proc);
    g_object_unref(proc);

    if (!success) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "exec failed: %s",
                                (stderr_out && *stderr_out) ? stderr_out : "non-zero exit");
        g_free(stdout_out); g_free(stderr_out);
    } else {
        g_free(stderr_out);
        g_task_return_pointer(task,
                              stdout_out ? stdout_out : g_strdup(""),
                              (GDestroyNotify)g_free);
    }
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

                                                                      
static gchar *
_container_config_value(struct lxc_container *c, const gchar *key, GError **error)
{
    int length = c->get_config_item(c, key, NULL, 0);
    if (length < 0 || length > 1048576) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot read container setting %s", key);
        return NULL;
    }
    gchar *value = g_malloc0((gsize)length + 1);
    if (length && c->get_config_item(c, key, value, length + 1) != length) {
        g_free(value);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Container setting changed while reading %s", key);
        return NULL;
    }
    return value;
}

static gboolean
_btrfs_config_ready(const gchar *name, gboolean copying, GError **error)
{
    struct lxc_container *c = _lxc_get(name, error);
    if (!c) return FALSE;
    gchar *root = _container_config_value(c, "lxc.rootfs.path", error);
    gchar *expected = g_build_filename(PCV_LXC_PATH, name, "rootfs", NULL);
    gboolean ok = FALSE;
    if (!root) goto done;
    const gchar *path = root;
    if (g_str_has_prefix(path, "btrfs:")) path += 6;
    else if (g_str_has_prefix(path, "dir:")) path += 4;
    if (g_strcmp0(path, expected)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "LXC rootfs.path differs from the managed Btrfs rootfs");
        goto done;
    }
    gchar *idmap = _container_config_value(c, "lxc.idmap", error);
    if (!idmap) goto done;
    gboolean rootless = *g_strstrip(idmap) != '\0';
    g_free(idmap);
    if (rootless) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Btrfs rootless containers are not supported");
        goto done;
    }
    if (copying) {
        gchar *fstab = _container_config_value(c, "lxc.mount.fstab", error);
        if (!fstab) goto done;
        gboolean has_fstab = *g_strstrip(fstab) != '\0';
        g_free(fstab);
        if (has_fstab) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "Btrfs snapshot/clone does not support lxc.mount.fstab");
            goto done;
        }
        gchar *entries = _container_config_value(c, "lxc.mount.entry", error);
        if (!entries) goto done;
        gchar **lines = g_strsplit(entries, "\n", -1);
        gboolean external = FALSE;
        for (guint i = 0; lines[i] && !external; i++) {
            gchar *line = g_strstrip(lines[i]);
            if (!*line) continue;
            gchar **words = g_strsplit_set(line, " \t", -1);
            const gchar *fields[4] = {0};
            guint count = 0;
            for (guint j = 0; words[j] && count < 4; j++)
                if (*words[j]) fields[count++] = words[j];
            gboolean fuse = count == 4 && g_str_equal(fields[0], "/sys/fs/fuse/connections") &&
                g_str_equal(fields[1], "sys/fs/fuse/connections") && g_str_equal(fields[2], "none") &&
                g_str_equal(fields[3], "bind,optional");
            gboolean pseudo = count == 4 &&
                (g_str_equal(fields[2], "proc") || g_str_equal(fields[2], "sysfs") ||
                 g_str_equal(fields[2], "tmpfs") || g_str_equal(fields[2], "devpts") ||
                 g_str_equal(fields[2], "mqueue") || g_str_equal(fields[2], "cgroup") ||
                 g_str_equal(fields[2], "cgroup2") || g_str_equal(fields[2], "hugetlbfs")) &&
                !strstr(fields[3], "bind");
            external = !fuse && !pseudo;
            g_strfreev(words);
        }
        g_strfreev(lines);
        g_free(entries);
        if (external) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "Btrfs snapshot/clone excludes configured external mounts; detach them first");
            goto done;
        }
    }
    ok = TRUE;
done:
    g_free(root);
    g_free(expected);
    lxc_container_put(c);
    return ok;
}

static gboolean
_container_stopped(const gchar *name, GError **error)
{
    struct lxc_container *c = _lxc_get(name, error);
    if (!c) return FALSE;
    gboolean stopped = !c->is_running(c);
    lxc_container_put(c);
    if (!stopped)
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Stop container '%s' before this Btrfs storage operation", name);
    return stopped;
}

static gboolean
_storage_prepare_operation(const gchar *name, PcvLxcStorage *storage, GError **error)
{
    if (!_container_stopped(name, error) || !_btrfs_config_ready(name, TRUE, error)) return FALSE;
    if (!pcv_lxc_storage_recover(name, error)) return FALSE;
    return pcv_lxc_storage_resolve(name, storage, error);
}

static gboolean
_snap_storage_begin(const gchar *name, PcvLxcStorage *storage, GError **error)
{
    if (!_lock_container_op(name)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                    "Container '%s' has a concurrent operation in progress", name);
        return FALSE;
    }
    _ensure_zfs_mounts();
    if (!pcv_lxc_storage_resolve(name, storage, error)) {
        g_clear_error(error);
        if (!_storage_prepare_operation(name, storage, error)) {
            _unlock_container_op(name);
            return FALSE;
        }
    }
    if (storage->kind == PCV_LXC_STORAGE_BTRFS &&
        (!_container_stopped(name, error) || !_btrfs_config_ready(name, TRUE, error) ||
         !pcv_lxc_storage_recover(name, error))) {
        pcv_lxc_storage_clear(storage);
        _unlock_container_op(name);
        return FALSE;
    }
    return TRUE;
}

static void
_snap_storage_end(const gchar *name, PcvLxcStorage *storage)
{
    pcv_lxc_storage_clear(storage);
    _unlock_container_op(name);
}

static void
_snap_create_thread(GTask *task, gpointer src __attribute__((unused)),
                     gpointer td, GCancellable *c __attribute__((unused)))
{
    LxcSnapData *d = td;
    GError *error = NULL;
    PcvLxcStorage storage = {0};
    if (!_snap_storage_begin(d->name, &storage, &error)) {
        g_task_return_error(task, error);
        return;
    }
    gboolean ok;
    if (storage.kind == PCV_LXC_STORAGE_BTRFS) {
        ok = pcv_lxc_storage_btrfs_snapshot_create(d->name, d->snap, &error);
    } else {
        gchar *target = g_strdup_printf("%s@%s", storage.dataset, d->snap);
        const gchar *argv[] = { "zfs", "snapshot", target, NULL };
        ok = _run_argv(argv, &error);
        g_free(target);
    }
    _snap_storage_end(d->name, &storage);
    if (ok) g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, error);
}

void pcv_lxc_snapshot_create_async(const gchar *name, const gchar *snap_name,
                                    GCancellable *c, GAsyncReadyCallback cb,
                                    gpointer user_data)
{
    GTask *task     = pcv_drain_task_new(NULL, c, cb, user_data);
    LxcSnapData *d  = g_new0(LxcSnapData, 1);
    d->name = g_strdup(name); d->snap = g_strdup(snap_name);
    g_task_set_task_data(task, d, (GDestroyNotify)_snap_data_free);
    g_task_run_in_thread(task, _snap_create_thread);
    g_object_unref(task);
}

                                               
gboolean pcv_lxc_snapshot_create_finish(GAsyncResult *r, GError **e)
{ return g_task_propagate_boolean(G_TASK(r), e); }

                                                                                
static void
_snap_rollback_thread(GTask *task, gpointer src __attribute__((unused)),
                     gpointer td, GCancellable *c __attribute__((unused)))
{
    LxcSnapData *d = td;
    GError *error = NULL;
    PcvLxcStorage storage = {0};
    if (!_snap_storage_begin(d->name, &storage, &error)) {
        g_task_return_error(task, error);
        return;
    }
    gboolean ok;
    if (storage.kind == PCV_LXC_STORAGE_BTRFS) {
        ok = pcv_lxc_storage_btrfs_snapshot_rollback(d->name, d->snap, &error);
    } else {
        gchar *target = g_strdup_printf("%s@%s", storage.dataset, d->snap);
        const gchar *argv[] = { "zfs", "rollback", "-r", target, NULL };
        ok = _run_argv(argv, &error);
        g_free(target);
    }
    _snap_storage_end(d->name, &storage);
    if (ok) g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, error);
}

void pcv_lxc_snapshot_rollback_async(const gchar *name, const gchar *snap_name,
                                      GCancellable *c, GAsyncReadyCallback cb,
                                      gpointer user_data)
{
    GTask *task     = pcv_drain_task_new(NULL, c, cb, user_data);
    LxcSnapData *d  = g_new0(LxcSnapData, 1);
    d->name = g_strdup(name); d->snap = g_strdup(snap_name);
    g_task_set_task_data(task, d, (GDestroyNotify)_snap_data_free);
    g_task_run_in_thread(task, _snap_rollback_thread);
    g_object_unref(task);
}

                                           
gboolean pcv_lxc_snapshot_rollback_finish(GAsyncResult *r, GError **e)
{ return g_task_propagate_boolean(G_TASK(r), e); }

                                                               
static void
_snap_delete_thread(GTask *task, gpointer src __attribute__((unused)),
                     gpointer td, GCancellable *c __attribute__((unused)))
{
    LxcSnapData *d = td;
    GError *error = NULL;
    PcvLxcStorage storage = {0};
    if (!_snap_storage_begin(d->name, &storage, &error)) {
        g_task_return_error(task, error);
        return;
    }
    gboolean ok;
    if (storage.kind == PCV_LXC_STORAGE_BTRFS) {
        ok = pcv_lxc_storage_btrfs_snapshot_delete(d->name, d->snap, &error);
    } else {
        gchar *target = g_strdup_printf("%s@%s", storage.dataset, d->snap);
        const gchar *argv[] = { "zfs", "destroy", target, NULL };
        ok = _run_argv(argv, &error);
        g_free(target);
    }
    _snap_storage_end(d->name, &storage);
    if (ok) g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, error);
}

void pcv_lxc_snapshot_delete_async(const gchar *name, const gchar *snap_name,
                                    GCancellable *c, GAsyncReadyCallback cb,
                                    gpointer user_data)
{
    GTask *task     = pcv_drain_task_new(NULL, c, cb, user_data);
    LxcSnapData *d  = g_new0(LxcSnapData, 1);
    d->name = g_strdup(name); d->snap = g_strdup(snap_name);
    g_task_set_task_data(task, d, (GDestroyNotify)_snap_data_free);
    g_task_run_in_thread(task, _snap_delete_thread);
    g_object_unref(task);
}

                                           
gboolean pcv_lxc_snapshot_delete_finish(GAsyncResult *r, GError **e)
{ return g_task_propagate_boolean(G_TASK(r), e); }

                                                  
static void
_snap_list_thread(GTask *task, gpointer src __attribute__((unused)),
                  gpointer td, GCancellable *c __attribute__((unused)))
{
    const gchar *name = td;
    GError *error = NULL;
    PcvLxcStorage storage = {0};
    if (!_lock_container_op(name)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_BUSY,
                                "Container '%s' has a concurrent operation in progress", name);
        return;
    }
    if (!pcv_lxc_storage_resolve(name, &storage, &error)) {
        _unlock_container_op(name);
        g_task_return_error(task, error);
        return;
    }
    GPtrArray *result = NULL;
    if (storage.kind == PCV_LXC_STORAGE_BTRFS) {
        result = pcv_lxc_storage_btrfs_snapshot_list(name, &error);
    } else {
        const gchar *argv[] = {
            "zfs", "list", "-H", "-t", "snapshot", "-o", "name", storage.dataset, NULL
        };
        gchar *output = _run_argv_capture(argv, &error);
        if (output) {
            result = g_ptr_array_new_with_free_func(g_free);
            gchar **lines = g_strsplit(output, "\n", -1);
            for (guint i = 0; lines[i]; i++) {
                gchar *at = strchr(lines[i], '@');
                if (at && (gsize)(at - lines[i]) == strlen(storage.dataset) &&
                    strncmp(lines[i], storage.dataset, (gsize)(at - lines[i])) == 0)
                    g_ptr_array_add(result, g_strdup(at + 1));
            }
            g_strfreev(lines);
            g_free(output);
        }
    }
    _snap_storage_end(name, &storage);
    if (result) g_task_return_pointer(task, result, (GDestroyNotify)g_ptr_array_unref);
    else g_task_return_error(task, error);
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
        if (g_file_set_contents(path_v2, val_v2, -1, error))
            return TRUE;
                                       
        return FALSE;
    }

                                                                  
    if (path_v1 && g_file_test(path_v1, G_FILE_TEST_EXISTS)) {
        if (g_file_set_contents(path_v1, val_v1 ? val_v1 : val_v2, -1, error))
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
    struct lxc_container *c = _lxc_get(name, error);
    if (!c) return FALSE;

    gboolean ok = TRUE;

    if (cpu_percent > 0) {                                                      
        gchar *v2_val = g_strdup_printf("%d 100000", cpu_percent * 1000);
        gchar *v1_val = g_strdup_printf("%d", cpu_percent * 1000);
        if (!c->set_config_item(c, "lxc.cgroup2.cpu.max", v2_val)) {
                                           
            if (!c->set_config_item(c, "lxc.cgroup.cpu.cfs_quota_us", v1_val)) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to set CPU config for '%s'", name);
                ok = FALSE;
            }
        }
        g_free(v2_val);
        g_free(v1_val);
    }

    if (ok && cpu_weight > 0) {
        gchar *val = g_strdup_printf("%d", cpu_weight);
        if (!c->set_config_item(c, "lxc.cgroup2.cpu.weight", val)) {
            g_warning("Failed to set cpu.weight config for '%s' (v2 only)", name);
        }
        g_free(val);
    }

    if (ok && memory_mb > 0) {
        gchar *val = g_strdup_printf("%" G_GINT64_FORMAT,
                                     (gint64)memory_mb * 1024 * 1024);
        if (!c->set_config_item(c, "lxc.cgroup2.memory.max", val)) {
            if (!c->set_config_item(c, "lxc.cgroup.memory.limit_in_bytes", val)) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to set memory config for '%s'", name);
                ok = FALSE;
            }
        }
        g_free(val);
    }

    if (ok && memory_low_mb > 0) {
        gchar *val = g_strdup_printf("%" G_GINT64_FORMAT,
                                     (gint64)memory_low_mb * 1024 * 1024);
        if (!c->set_config_item(c, "lxc.cgroup2.memory.low", val)) {
            g_warning("Failed to set memory.low config for '%s' (v2 only)", name);
        }
        g_free(val);
    }

    if (ok && memory_high_mb > 0) {
        gchar *val = g_strdup_printf("%" G_GINT64_FORMAT,
                                     (gint64)memory_high_mb * 1024 * 1024);
        if (!c->set_config_item(c, "lxc.cgroup2.memory.high", val)) {
            g_warning("Failed to set memory.high config for '%s' (v2 only)", name);
        }
        g_free(val);
    }

    if (ok && io_read_bps > 0) {
        gchar *val = g_strdup_printf("8:0 rbps=%" G_GINT64_FORMAT, io_read_bps);
        if (!c->set_config_item(c, "lxc.cgroup2.io.max", val)) {
            g_warning("Failed to set io.max config for '%s' (v2 only)", name);
        }
        g_free(val);
    }

    if (ok && pids_max > 0) {
        gchar *val = g_strdup_printf("%d", pids_max);
        if (!c->set_config_item(c, "lxc.cgroup2.pids.max", val)) {
            g_warning("Failed to set pids.max config for '%s' (v2 only)", name);
        }
        g_free(val);
    }

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
    g_autoptr(PcvLxcOperationGuard) guard = pcv_lxc_operation_guard_acquire(name, error);
    if (!guard) return FALSE;

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
        gchar *v2_val = g_strdup_printf("%d 100000", cpu_percent * 1000);
        gchar *v1_val = g_strdup_printf("%d", cpu_percent * 1000);

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

                                                                 
    if (c->is_running(c) && nics->len > 0) {
        gchar *cmd_line = g_strdup_printf(
            "lxc-attach -P %s -n %s -- ip -4 -o addr show 2>/dev/null",
            PCV_LXC_PATH, name);
        gchar *out = nullptr;
        const gchar *argv[] = {"/bin/sh", "-c", cmd_line, NULL};
        if (pcv_spawn_sync_profile(argv, PCV_CHILD_CAP_RUNTIME,
                                   &out, NULL, NULL) && out) {
                                                                          
            gchar **lines = g_strsplit(out, "\n", -1);
            for (int i = 0; lines[i]; i++) {
                for (guint n = 0; n < nics->len; n++) {
                    PcvLxcNicInfo *ni = g_ptr_array_index(nics, n);
                    if (g_strstr_len(lines[i], -1, ni->name)) {                        
                                                      
                        const gchar *inet = g_strstr_len(lines[i], -1, "inet ");
                        if (inet) {
                            inet += 5;                                  
                            const gchar *sp = strchr(inet, ' ');
                            g_free(ni->ipv4);
                            ni->ipv4 = sp ? g_strndup(inet, (gsize)(sp - inet))
                                          : g_strdup(inet);
                        }
                        break;
                    }
                }
            }
            g_strfreev(lines);
        }
        g_free(out);
        g_free(cmd_line);
    }

    lxc_container_put(c);
    return nics;
}

   
                                                      
  
                                                    
                                     
  
                                                                      
                                                        
   
gboolean
pcv_lxc_nic_attach(const gchar *name, const gchar *bridge,
                     const gchar *hwaddr, GError **error)
{
    g_autoptr(PcvLxcOperationGuard) guard = pcv_lxc_operation_guard_acquire(name, error);
    if (!guard) return FALSE;

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

    gchar k1[64], k2[64], k3[64], k4[64];
    g_snprintf(k1, sizeof(k1), "lxc.net.%d.type",   idx);
    g_snprintf(k2, sizeof(k2), "lxc.net.%d.link",   idx);
    g_snprintf(k3, sizeof(k3), "lxc.net.%d.flags",  idx);
    g_snprintf(k4, sizeof(k4), "lxc.net.%d.name",   idx);

    c->set_config_item(c, k1, "veth");
    c->set_config_item(c, k2, br);
    c->set_config_item(c, k3, "up");

    gchar ethname[16];
    g_snprintf(ethname, sizeof(ethname), "eth%d", idx);
    c->set_config_item(c, k4, ethname);

    if (hwaddr && *hwaddr) {
        gchar k5[64];
        g_snprintf(k5, sizeof(k5), "lxc.net.%d.hwaddr", idx);
        c->set_config_item(c, k5, hwaddr);
    }

    c->save_config(c, NULL);

                                                          
                                                      
                                                                       
                                                                
    if (c->is_running(c)) {
        gchar *cmd = g_strdup_printf(
            "ip link add %s-host type veth peer name %s && "
            "ip link set %s-host master %s && "
            "ip link set %s-host up && "
            "ip link set %s netns $(cat /sys/fs/cgroup/lxc.payload.%s/cgroup.procs | head -1) && "
            "nsenter -t $(cat /sys/fs/cgroup/lxc.payload.%s/cgroup.procs | head -1) -n ip link set %s up",
            ethname, ethname, ethname, br, ethname, ethname, name, name, ethname);
        const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
        pcv_spawn_sync(argv, NULL, NULL, NULL);
        g_free(cmd);
    }

    lxc_container_put(c);
    return TRUE;
}

   
                                        
  
                                                          
                 
  
                                                                        
                                                             
   
gboolean
pcv_lxc_nic_detach(const gchar *name, const gchar *nic_name, GError **error)
{
    g_autoptr(PcvLxcOperationGuard) guard = pcv_lxc_operation_guard_acquire(name, error);
    if (!guard) return FALSE;

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

                            
    if (c->is_running(c)) {
        gchar *cmd = g_strdup_printf(
            "lxc-attach -P %s -n %s -- ip link del %s 2>/dev/null",
            PCV_LXC_PATH, name, nic_name);
        const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
        pcv_spawn_sync_profile(argv, PCV_CHILD_CAP_RUNTIME, NULL, NULL, NULL);
        g_free(cmd);
    }

                                  
    const gchar *keys[] = {"type", "link", "flags", "name", "hwaddr",
                            "ipv4.address", "ipv4.gateway", NULL};
    for (int i = 0; keys[i]; i++) {
        gchar key[64];
        g_snprintf(key, sizeof(key), "lxc.net.%d.%s", target, keys[i]);
        c->clear_config_item(c, key);
    }
    c->save_config(c, NULL);

    lxc_container_put(c);
    return TRUE;
}

   
                                    
  
                                                    
                                         
  
                                                                                  
                                                               
                                                                   
   
gboolean
pcv_lxc_set_bandwidth(const gchar *name, const gchar *nic_name,
                        guint inbound_kbps, guint outbound_kbps,
                        GError **error)
{
    if (!name || !*name) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Container name required");
        return FALSE;
    }

                                                                  
                                                               
    const gchar *iface = nic_name ? nic_name : "eth0";                          
    gchar *find_cmd = g_strdup_printf(
        "ip link show type veth 2>/dev/null | "
        "awk -F'[@:]' '/%s/{gsub(/ /,\"\",$$2); print $$2}'",
        name);
    gchar *host_veth = nullptr;
    const gchar *argv_find[] = {"/bin/sh", "-c", find_cmd, NULL};
    pcv_spawn_sync(argv_find, &host_veth, NULL, NULL);
    g_free(find_cmd);

    if (!host_veth || !*host_veth) {
                                                            
        g_free(host_veth);
        if (inbound_kbps > 0) {
            gchar *cmd = g_strdup_printf(
                "lxc-attach -P %s -n %s -- sh -c '"
                "tc qdisc del dev %s root 2>/dev/null; "
                "tc qdisc add dev %s root tbf rate %dkbit burst 32kbit latency 50ms"
                "'", PCV_LXC_PATH, name, iface, iface, inbound_kbps);
            const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
            pcv_spawn_sync_profile(argv, PCV_CHILD_CAP_RUNTIME, NULL, NULL, NULL);
            g_free(cmd);
        }
        return TRUE;
    }

    g_strstrip(host_veth);

                                 
    if (outbound_kbps > 0) {
        gchar *cmd = g_strdup_printf(
            "tc qdisc del dev %s root 2>/dev/null; "
            "tc qdisc add dev %s root tbf rate %dkbit burst 32kbit latency 50ms",
            host_veth, host_veth, outbound_kbps);
        const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
        pcv_spawn_sync(argv, NULL, NULL, NULL);
        g_free(cmd);
    }

                                                                            
    if (inbound_kbps > 0) {
        gchar *cmd = g_strdup_printf(
            "lxc-attach -P %s -n %s -- sh -c '"
            "tc qdisc del dev %s root 2>/dev/null; "
            "tc qdisc add dev %s root tbf rate %dkbit burst 32kbit latency 50ms"
            "'", PCV_LXC_PATH, name, iface, iface, inbound_kbps);
        const gchar *argv[] = {"/bin/sh", "-c", cmd, NULL};
        pcv_spawn_sync_profile(argv, PCV_CHILD_CAP_RUNTIME, NULL, NULL, NULL);
        g_free(cmd);
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

                                                                             
                        
  
                                                   
                                                           
  
                               
                                                               
                                                                               

#define PCV_SECCOMP_DIR "/etc/purecvisor/seccomp"                        

   
                                                      
  
                                                
                                        
  
                                              
                                   
  
                                                                              
                                                                 
   
gboolean
pcv_lxc_set_seccomp_profile(const gchar *name, const gchar *profile_name)
{
    g_autoptr(PcvLxcOperationGuard) guard = pcv_lxc_operation_guard_acquire(name, NULL);
    if (!guard) return FALSE;

    if (!name || !profile_name) return FALSE;

    gchar *profile_path = g_strdup_printf("%s/%s.seccomp", PCV_SECCOMP_DIR, profile_name);

                                                           
    if (!g_file_test(profile_path, G_FILE_TEST_EXISTS)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "Seccomp profile not found: %s", profile_path);
        g_free(profile_path);
        return FALSE;
    }

                                       
    gchar *config_path = g_strdup_printf("%s/%s/config", PCV_LXC_PATH, name);

                                  
    gchar *contents = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(config_path, &contents, &len, NULL)) {
        PCV_LOG_WARN(LXC_LOG_DOM, "Cannot read container config: %s", config_path);
        g_free(profile_path);
        g_free(config_path);
        return FALSE;
    }

                                                                 
                                 
    GString *new_config = g_string_new("");
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        if (!g_str_has_prefix(*l, "lxc.seccomp.profile"))                         
            g_string_append_printf(new_config, "%s\n", *l);
    }
    g_string_append_printf(new_config, "lxc.seccomp.profile = %s\n", profile_path);
    g_strfreev(lines);
    g_free(contents);

    gboolean ok = g_file_set_contents(config_path, new_config->str, -1, NULL);
    g_string_free(new_config, TRUE);
    g_free(config_path);

    if (ok)
        PCV_LOG_INFO(LXC_LOG_DOM, "Set seccomp profile '%s' for container '%s'",
                     profile_name, name);
    else
        PCV_LOG_WARN(LXC_LOG_DOM, "Failed to write seccomp config for '%s'", name);

    g_free(profile_path);
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
