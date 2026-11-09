                                                                             
                               
                                                                                                 
  
                           
                                                                                            
                                                                                          
                                                                                 
                                  
                                                                                                  
                                                                                     
                                     
  
                                                                                 
                                                                                
#include "lxc_limits.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

                                                             
                                                 
gboolean
pcv_lxc_cgroup_write(const gchar *path, const gchar *value, GError **error)
{
    if (!path || !*path || !value) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "cgroup write: path and value are required");
        return FALSE;
    }
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "%s: %s", path, g_strerror(e));
        return FALSE;
    }
    const gsize len = strlen(value);
    ssize_t n = write(fd, value, len);
    int werr = (n < 0) ? errno : (gsize)n == len ? 0 : EIO;                                      
    int cerr = (close(fd) < 0) ? errno : 0;
    if (werr || cerr) {
        int e = werr ? werr : cerr;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(e), "%s: %s", path, g_strerror(e));
        return FALSE;
    }
    return TRUE;
}

                                             
static gboolean
_parse_uint64(const gchar *s, guint64 *out)
{
    if (!s || !g_ascii_isdigit(*s))
        return FALSE;
    gchar *end = NULL;
    errno = 0;
    guint64 v = g_ascii_strtoull(s, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return FALSE;
    *out = v;
    return TRUE;
}

                                                      
                                                                                  
gchar *
pcv_lxc_limits_cpu_max(const gchar *cfg_cpu_max, const gchar *cfg_shares)
{
    if (cfg_cpu_max) {
        g_autofree gchar *trimmed = g_strstrip(g_strdup(cfg_cpu_max));
        g_auto(GStrv) parts = g_strsplit_set(trimmed, " \t", -1);
        guint64 q = 0, p = 0;
        if (parts && parts[0] && parts[1] && !parts[2] &&
            (g_strcmp0(parts[0], "max") == 0 || _parse_uint64(parts[0], &q)) &&
            _parse_uint64(parts[1], &p) && p > 0)
            return g_strdup_printf("%s %" G_GUINT64_FORMAT, parts[0], p);
    }
    guint64 shares = 0;
    g_autofree gchar *s = cfg_shares ? g_strstrip(g_strdup(cfg_shares)) : NULL;
    if (_parse_uint64(s, &shares) && shares > 0 && shares <= G_MAXUINT32) {
        guint64 cores = shares / 1024;
        if (cores < 1)
            cores = 1;               
        return g_strdup_printf("%" G_GUINT64_FORMAT " %d", cores * PCV_LXC_CPU_PERIOD_US, PCV_LXC_CPU_PERIOD_US);
    }
    return NULL;
}

                                                                                    
                                                             
gboolean
pcv_lxc_limits_memory_bytes(const gchar *cfg_value, guint64 *bytes_out)
{
    if (!cfg_value || !bytes_out)
        return FALSE;
    g_autofree gchar *s = g_strstrip(g_strdup(cfg_value));
    gsize len = strlen(s);
    guint shift = 0;
    if (len > 0) {
        switch (g_ascii_toupper(s[len - 1])) {
        case 'K': shift = 10; break;
        case 'M': shift = 20; break;
        case 'G': shift = 30; break;
        case 'T': shift = 40; break;
        default: break;
        }
        if (shift)
            s[len - 1] = '\0';
    }
    guint64 v = 0;
    if (!_parse_uint64(s, &v) || v == 0 || v > (G_MAXUINT64 >> shift))
        return FALSE;
    *bytes_out = v << shift;
    return TRUE;
}

                                                                                      
static gchar *
_config_value(gchar **lines, const gchar *key)
{
    gchar *found = NULL;
    const gsize klen = strlen(key);
    for (gsize i = 0; lines[i]; i++) {
        const gchar *p = lines[i] + strspn(lines[i], " \t");
        if (strncmp(p, key, klen) != 0)
            continue;
        p += klen;
        p += strspn(p, " \t");
        if (*p != '=')
            continue;                    
        g_free(found);
        found = g_strstrip(g_strdup(p + 1));
    }
    return found;
}

                                                      
                                                                      
                                                                                                             
guint64
pcv_lxc_cgroup_read_u64(const gchar *cgroup_root, const gchar *name, const gchar *filename)
{
    if (!cgroup_root || !name || !filename)
        return 0;
    g_autofree gchar *path_v2 = g_strdup_printf("%s/lxc.payload.%s/%s", cgroup_root, name, filename);
    g_autofree gchar *path_v1 = g_strdup_printf("/sys/fs/cgroup/memory/lxc/%s/%s", name, filename);
    g_autofree gchar *content = NULL;
    if (g_file_get_contents(path_v2, &content, NULL, NULL) || g_file_get_contents(path_v1, &content, NULL, NULL))
        return (guint64)g_ascii_strtoull(g_strstrip(content), NULL, 10);
    return 0;
}

                                                                             
guint64
pcv_lxc_cgroup_cpu_usage_ns(const gchar *cgroup_root, const gchar *name)
{
    if (!cgroup_root || !name)
        return 0;
    g_autofree gchar *path = g_strdup_printf("%s/lxc.payload.%s/cpu.stat", cgroup_root, name);
    g_autofree gchar *content = NULL;
    if (!g_file_get_contents(path, &content, NULL, NULL))
        return 0;
    g_auto(GStrv) lines = g_strsplit(content, "\n", -1);
    for (gsize i = 0; lines[i]; i++) {
        if (g_str_has_prefix(lines[i], "usage_usec "))
            return g_ascii_strtoull(lines[i] + strlen("usage_usec "), NULL, 10) * 1000;
    }
    return 0;
}

                                                                            
                                                                                  
gboolean
pcv_lxc_limits_reapply(const gchar *lxc_path, const gchar *name, const gchar *cgroup_root, GError **error)
{
    if (!lxc_path || !name || !*name || !cgroup_root) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "limits reapply: path, name and cgroup root are required");
        return FALSE;
    }
    g_autofree gchar *cfg_path = g_strdup_printf("%s/%s/config", lxc_path, name);
    g_autofree gchar *text = NULL;
    if (!g_file_get_contents(cfg_path, &text, NULL, error))
        return FALSE;
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);

    g_autofree gchar *cfg_cpu = _config_value(lines, "lxc.cgroup2.cpu.max");
    g_autofree gchar *cfg_shares = _config_value(lines, "lxc.cgroup.cpu.shares");
    g_autofree gchar *cfg_mem = _config_value(lines, "lxc.cgroup2.memory.max");

    g_autofree gchar *cpu_val = pcv_lxc_limits_cpu_max(cfg_cpu, cfg_shares);
    if (cpu_val) {
        g_autofree gchar *path = g_strdup_printf("%s/lxc.payload.%s/cpu.max", cgroup_root, name);
        if (!pcv_lxc_cgroup_write(path, cpu_val, error))
            return FALSE;
    }
    guint64 bytes = 0;
    if (pcv_lxc_limits_memory_bytes(cfg_mem, &bytes)) {
        g_autofree gchar *path = g_strdup_printf("%s/lxc.payload.%s/memory.max", cgroup_root, name);
        g_autofree gchar *val = g_strdup_printf("%" G_GUINT64_FORMAT, bytes);
        if (!pcv_lxc_cgroup_write(path, val, error))
            return FALSE;
    }
    return TRUE;
}

                                                                                            
                                                                          
guint64
pcv_lxc_cgroup_cpu_time_ns(const gchar *cgroup_root, const gchar *name)
{
    guint64 ns = pcv_lxc_cgroup_read_u64(cgroup_root, name, "cpuacct.usage");
    return ns ? ns : pcv_lxc_cgroup_cpu_usage_ns(cgroup_root, name);
}

                                                            
                                                                                       
                                                                 
                                                                        
                                                                                
gdouble
pcv_lxc_cpu_percent_between(const PcvLxcCpuSample *prev, const PcvLxcCpuSample *cur)
{
    if (!prev || !cur || cur->init_pid != prev->init_pid || cur->cpu_time_ns < prev->cpu_time_ns || cur->mono_ns <= prev->mono_ns)
        return 0.0;
    return (gdouble)(cur->cpu_time_ns - prev->cpu_time_ns) / (gdouble)(cur->mono_ns - prev->mono_ns) * 100.0;
}

                                                                                                       
                                                                              
static GMutex      G_cpu_samples_mu;
static GHashTable *G_cpu_samples;

                                                                                  
                                                                                  
                                                                           
gdouble
pcv_lxc_cpu_sample_record(const gchar *name, gint init_pid, guint64 cpu_time_ns, gint64 mono_ns)
{
    if (!name)
        return 0.0;
    const PcvLxcCpuSample cur = { cpu_time_ns, mono_ns, init_pid };
    gdouble pct = 0.0;
    g_mutex_lock(&G_cpu_samples_mu);
    if (!G_cpu_samples)
        G_cpu_samples = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (cpu_time_ns == 0) {
        g_hash_table_remove(G_cpu_samples, name);                            
    } else {
        PcvLxcCpuSample *prev = g_hash_table_lookup(G_cpu_samples, name);
        pct = pcv_lxc_cpu_percent_between(prev, &cur);
        if (prev)
            *prev = cur;
        else
            g_hash_table_insert(G_cpu_samples, g_strdup(name), g_memdup2(&cur, sizeof cur));
    }
    g_mutex_unlock(&G_cpu_samples_mu);
    return pct;
}

                                                             
                                                                 
void
pcv_lxc_cpu_sample_forget(const gchar *name)
{
    if (!name)
        return;
    g_mutex_lock(&G_cpu_samples_mu);
    if (G_cpu_samples)
        g_hash_table_remove(G_cpu_samples, name);
    g_mutex_unlock(&G_cpu_samples_mu);
}

                                                                                             
static gchar *G_metrics_cgroup_root;

                                                                            
const gchar *
pcv_lxc_metrics_cgroup_root(void)
{
    return G_metrics_cgroup_root ? G_metrics_cgroup_root : PCV_LXC_CGROUP_ROOT;
}

                                                                                      
                                                            
void
pcv_lxc_metrics_set_cgroup_root_for_test(const gchar *root)
{
    g_free(G_metrics_cgroup_root);
    G_metrics_cgroup_root = g_strdup(root);
}
