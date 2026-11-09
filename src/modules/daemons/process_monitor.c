   
                          
                                                                  
  
                           
                                                   
                                                    
                                        
  
                                            
                                              
                                                
                                               
                                                   
                               
  
                                                 
                                  
                                                                 
                                               
                                           
                                       
                                                        
                                                              
                                                     
  
                                                                              
           
                                                                              
  
                    
                                                                     
                                                           
  
                           
                                               
                                                               
  
                             
                                                                   
  
                                                                              
                                
                                                                              
  
                                               
                                                      
                                              
  
            
                                                          
                                              
                                                      
                                                     
  
                                                                              
          
                                                                              
  
                                              
        
                              
                                                     
                                                               
                                                             
                                                    
                               
                                                 
                                                         
        
                                                    
  
                                     
        
                                        
                                                           
  
                                                             
  
                                                                              
        
                                                                              
  
                                                             
                                                               
                                                     
                                  
                                      
                                                         
   
#include "process_monitor.h"
#include "utils/pcv_log.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>

  
                                                                               
                               
                                                                               
  
                  
                                                      
                                            
                                                          
                                                 
  
                       
                                                        
                                     
  
                                              
                                   
  
                                                  
                                                     
                            
                                                                               
   

                                                 
#define PROC_LOG_DOM      "proc_mon"

                                                 
#define PROC_INTERVAL_SEC 20

                                              
#define PROC_MAX          512
#define PROC_SERVICE_SCAN_LIMIT   16384
#define PROC_SERVICE_RESULT_LIMIT  8192

   
                 
                                               
   
typedef enum {
    PROC_HOST      = 0,                     
    PROC_VM        = 1,                              
    PROC_CONTAINER = 2,                       
    PROC_SYSTEM    = 3                           
} ProcType;

                                   
static const gchar *_proc_type_str[] = { "host", "vm", "container", "system" };

   
                   
                                     
  
                                                    
                                             
                           
   
typedef struct {
       
               
                                      
                                                          
       
    gint     pid;

       
                                      
                                                    
                                                  
      
                                          
                                                         
                                    
       
    gchar    comm[64];

       
                  
                                               
                                   
                                          
                                              
                                         
                              
                                      
                                        
       
    gchar    state;

       
                            
                                         
                                                              
      
                                                                
       
    guint64  utime;

       
                           
                                         
                                      
       
    guint64  stime;

       
                                                    
                                 
      
                                                  
                                                   
                                            
      
                                                          
       
    glong    rss_pages;

       
                       
                                            
      
                                             
                                                   
       
    guint64  io_rd_bytes;

       
                       
                                             
                                          
       
    guint64  io_wr_bytes;

       
                   
                                         
      
                                                          
                                                             
      
                                  
                                                     
       
    gdouble  cpu_percent;

       
                                           
                                           
                                                                  
       
    gchar    cgroup[256];

       
                             
                                  
                                                                    
       
    ProcType type;
} ProcInfo;

                                            
typedef struct {
    gint  pid;
    gchar state;
    gchar cgroup[256];
} ServiceProcInfo;

   
                                 
  
                                
                                        
   
static struct {
                                                        
    GThread    *thread;

       
                                                          
                                  
       
    gboolean    running;

       
                                                
                                                   
                                                                  
       
    GMutex      wait_mu;
    GCond       wait_cond;

                                          
    gboolean    initialized;

       
                       
                                               
                                                       
       
    GMutex      mu;

       
                                        
                                                  
       
    ProcInfo    procs[PROC_MAX];

                                   
    gint        count;

                                                                    
    gint64      observed_at;

                                                        
    gboolean    snapshot_truncated;

                                                                            
    GArray     *service_procs;

                                                  
    gint        service_source_count;

                                                
    gint64      service_observed_at;

                                           
    gboolean    service_snapshot_truncated;

       
                        
                                           
                              
                                                   
       
    glong       page_size;

       
                       
                                                
                                                        
      
                                            
                                                          
       
    glong       clk_tck;

       
                                 
                                   
      
                                                
                                             
       
    gint64      prev_time_us;

       
                                               
      
                                                         
                                                                
                                                     
                                                     
      
                                                              
                                                             
                                                           
      
                                              
                                          
      
                                           
                                             
       
    GHashTable *prev_ticks;
} G = {0};

   
                                                  
  
                                                 
                                                          
                        
  
                                                
                                                
  
                            
                            
                       
                                                         
   
static gboolean
_get_process_cgroup(gint pid, gchar *buf, gsize bufsize)
{
    gchar path[64];
    g_snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);

    gchar *content = NULL;
    if (!g_file_get_contents(path, &content, NULL, NULL)) {
        g_strlcpy(buf, "/", bufsize);
        return FALSE;
    }

                                          
    gchar **lines = g_strsplit(content, "\n", -1);
    gboolean found = FALSE;
    for (gint i = 0; lines[i]; i++) {
        if (g_str_has_prefix(lines[i], "0::")) {
            g_strlcpy(buf, lines[i] + 3, bufsize);                   
            found = TRUE;
            break;
        }
    }

                                       
    if (!found && lines[0]) {
        const gchar *last_colon = strrchr(lines[0], ':');
        if (last_colon) {
            g_strlcpy(buf, last_colon + 1, bufsize);
            found = TRUE;
        } else {
            g_strlcpy(buf, "/", bufsize);
        }
    } else if (!found) {
        g_strlcpy(buf, "/", bufsize);
    }

    g_strfreev(lines);
    g_free(content);
    return found;
}

   
                                             
  
                  
                                                              
                                                                 
                                           
                       
  
                                            
                                         
  
                               
                                                     
                         
   
static ProcType
_classify_process(const gchar *cgroup, const gchar *comm)
{
    if (strstr(cgroup, "/lxc/") || strstr(cgroup, "/lxc.payload."))
        return PROC_CONTAINER;
    if (strstr(cgroup, "/machine.slice/") || strstr(comm, "qemu"))
        return PROC_VM;
    if (strstr(cgroup, ".service"))
        return PROC_SYSTEM;
    return PROC_HOST;
}

   
                                                  
  
                                                                              
                                          
                                                                              
  
                                    
  
                                                            
                                                              
                                                                    
  
                     
                                 
                                           
                                               
                                    
                                    
                               
                                
                                          
                                
                                       
                                       
                                       
                                       
                                                     
                                                     
                                          
                                          
                                   
                                
                               
                                        
                                                 
                                         
                                               
  
                                                                              
                       
                                                                              
  
                                   
                                                            
  
                                        
         
                                           
                                          
                           
  
                                             
                                  
  
                                               
                                                                  
                                       
  
                                             
                                               
                                                 
  
                         
                                                      
   
static void
_parse_proc_stat(gint pid, ProcInfo *p)
{
    gchar path[64];
    g_snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return;

                                                                             
    gchar buf[1024];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return; }
    fclose(f);

      
                                          
                                      
                                    
                                       
       
    gchar *start = strchr(buf, '(');
    gchar *end = strrchr(buf, ')');
    if (!start || !end || end <= start) return;

                                       
    gsize clen = (gsize)(end - start - 1);
    if (clen >= sizeof(p->comm)) clen = sizeof(p->comm) - 1;
    memcpy(p->comm, start + 1, clen);
    p->comm[clen] = '\0';

      
                                
                                                
      
                                 
                                         
                                                       
                                
                                   
                                  
                                 
                                 
                                  
                                   
                                  
                                   
                                               
                                              
                                  
                                  
                                    
                                
                                       
                                       
                                     
                                 
                                           
      
                                                               
       
    gchar *rest = end + 2;                
    gchar state;
    gint ppid;
    unsigned long utime, stime;
    long rss;

                                                                     
                                                             
                                                                      
                                                                         
    int n = sscanf(rest,
        "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
        &state, &ppid, &utime, &stime, &rss);
    if (n >= 5) {
        p->pid = pid;
        p->state = state;
        p->utime = utime;
        p->stime = stime;
        p->rss_pages = rss;
    }
}

   
                                                  
  
                                                                              
                                        
                                                                              
  
                             
  
                                                                
                                                                
                                                 
                                                  
                                                                
                                                                 
                                            
  
                                                   
                                                          
  
                                                                              
               
                                                                              
  
                                              
                            
                                            
  
                                    
                                                     
  
                                              
                                                   
                         
  
                         
                                                                
   
static void
_parse_proc_io(gint pid, ProcInfo *p)
{
    gchar path[64];
    g_snprintf(path, sizeof(path), "/proc/%d/io", pid);
    FILE *f = fopen(path, "r");
    if (!f) return;

    gchar line[128];
    while (fgets(line, sizeof(line), f)) {
        guint64 val;
        if (sscanf(line, "read_bytes: %lu", &val) == 1)
            p->io_rd_bytes = val;
        else if (sscanf(line, "write_bytes: %lu", &val) == 1)
            p->io_wr_bytes = val;
    }
    fclose(f);
}

   
                                    
  
                                  
                                          
  
                                                 
  
                                              
   
static gint
_sort_by_cpu(gconstpointer a, gconstpointer b)
{
    const ProcInfo *pa = a, *pb = b;
    if (pb->cpu_percent > pa->cpu_percent) return 1;
    if (pb->cpu_percent < pa->cpu_percent) return -1;
    return 0;
}

   
                                                                        
  
                                                                    
                                                             
                                                       
   
static GArray *
_collect_service_processes(gint64 *observed_at,
                           gint *source_count,
                           gboolean *truncated)
{
    GArray *items = g_array_sized_new(FALSE, FALSE, sizeof(ServiceProcInfo), 64);
    DIR *dir = opendir("/proc");
    if (!dir) {
        if (observed_at) *observed_at = 0;
        if (source_count) *source_count = 0;
        if (truncated) *truncated = TRUE;
        return items;
    }

    guint scanned = 0;
    guint readable = 0;
    gboolean bounded = FALSE;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)entry->d_name[0])) continue;
        if (scanned >= PROC_SERVICE_SCAN_LIMIT) {
            bounded = TRUE;
            break;
        }
        scanned++;

        const gint pid = atoi(entry->d_name);
        if (pid <= 0) continue;
        gchar cgroup[256] = {0};
        if (!_get_process_cgroup(pid, cgroup, sizeof(cgroup))) continue;
        readable++;
        if (!strstr(cgroup, ".service")) continue;

        ProcInfo process = {0};
        _parse_proc_stat(pid, &process);
        if (process.pid == 0) continue;
        process.type = _classify_process(cgroup, process.comm);
        if (process.type != PROC_SYSTEM) continue;
        if (items->len >= PROC_SERVICE_RESULT_LIMIT) {
            bounded = TRUE;
            continue;
        }

        ServiceProcInfo item = {0};
        item.pid = process.pid;
        item.state = process.state;
        g_strlcpy(item.cgroup, cgroup, sizeof(item.cgroup));
        g_array_append_val(items, item);
    }
    closedir(dir);

    if (observed_at) *observed_at = readable > 0 ? (gint64)time(NULL) : 0;
    if (source_count) *source_count = (gint)scanned;
    if (truncated) *truncated = bounded;
    return items;
}

   
                                            
  
                                                                              
                 
                                                                              
  
                                                   
                                                                   
  
                                                
  
                                
                                                              
                                                                     
  
                                                                 
                                                                 
  
                                                       
  
                              
                                                            
                                            
  
                                      
                                                     
  
                                                                              
                               
                                                                              
  
                                                    
  
        
                                      
                                                               
                                                                
  
        
                                                                            
                                                               
  
                                             
                                             
  
                                                                              
         
                                                                              
  
                                      
                                     
                  
                                                                  
                                                            
                                                              
                                          
                             
                                         
                                           
  
                                               
                                                 
                                                    
   
static void
_collect_processes(void)
{
    DIR *d = opendir("/proc");
    if (!d) return;

    ProcInfo tmp[PROC_MAX];
    gint count = 0;

                                                             
    gint64 now_us = g_get_monotonic_time();

    gint64 service_observed_at = 0;
    gint service_source_count = 0;
    gboolean service_truncated = FALSE;
    GArray *service_tmp = _collect_service_processes(
        &service_observed_at, &service_source_count, &service_truncated);

      
                               
                                                             
       
    gdouble dt_sec = (G.prev_time_us > 0)
        ? (gdouble)(now_us - G.prev_time_us) / G_USEC_PER_SEC
        : 0.0;

                                                       
                                                        
    g_autoptr(GHashTable) seen = g_hash_table_new(g_direct_hash, g_direct_equal);

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < PROC_MAX) {
                                                
        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        gint pid = atoi(ent->d_name);
        if (pid <= 0) continue;

        ProcInfo *p = &tmp[count];
        memset(p, 0, sizeof(*p));

                                                                  
        _parse_proc_stat(pid, p);
        if (p->pid == 0) continue;                             

                                                                      
        _parse_proc_io(pid, p);

                                                            
        _get_process_cgroup(pid, p->cgroup, sizeof(p->cgroup));
        p->type = _classify_process(p->cgroup, p->comm);

          
                      
                                                         
                                                                  
                                                                      
                                                           
          
                                                              
          
                                                         
                                                
           
        guint64 total_ticks = p->utime + p->stime;
        gpointer prev_val = g_hash_table_lookup(G.prev_ticks, GINT_TO_POINTER(pid));
        if (prev_val && dt_sec > 0.0) {
            guint64 prev_ticks = GPOINTER_TO_SIZE(prev_val);
            guint64 delta = (total_ticks >= prev_ticks) ? total_ticks - prev_ticks : 0;
            p->cpu_percent = 100.0 * (gdouble)delta / ((gdouble)G.clk_tck * dt_sec);
        }
                                                
        g_hash_table_insert(G.prev_ticks, GINT_TO_POINTER(pid), GSIZE_TO_POINTER(total_ticks));
        g_hash_table_add(seen, GINT_TO_POINTER(pid));                          
        count++;
    }
    closedir(d);

                                                           
                                                         
                                                                   
    if (count < PROC_MAX) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, G.prev_ticks);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            if (!g_hash_table_contains(seen, k))
                g_hash_table_iter_remove(&it);
        }
    }

      
                                                   
                                
                                                 
                                                        
      
                  
                                                              
                                 
            
                                                      
       
    qsort(tmp, count, sizeof(ProcInfo), _sort_by_cpu);

      
                                  
                                             
                                  
       
    g_mutex_lock(&G.mu);
    memcpy(G.procs, tmp, count * sizeof(ProcInfo));
    G.count = count;
    G.observed_at = (gint64)time(NULL);
    G.snapshot_truncated = count >= PROC_MAX;
    g_array_set_size(G.service_procs, 0);
    if (service_tmp->len > 0)
        g_array_append_vals(G.service_procs, service_tmp->data, service_tmp->len);
    G.service_source_count = service_source_count;
    G.service_observed_at = service_observed_at;
    G.service_snapshot_truncated = service_truncated;
    g_mutex_unlock(&G.mu);
    g_array_unref(service_tmp);

                                       
    G.prev_time_us = now_us;
}

   
                                                      
  
                                                           
                                           
  
                                                                  
                                                                    
                                                 
  
                                             
                                                 
                 
  
                          
               
   
static gpointer
_proc_thread(gpointer data)
{
    (void)data;
    PCV_LOG_INFO(PROC_LOG_DOM, "Process monitor started (interval=%ds)", PROC_INTERVAL_SEC);

    g_mutex_lock(&G.wait_mu);
    while (G.running) {
        g_mutex_unlock(&G.wait_mu);
        _collect_processes();
        g_mutex_lock(&G.wait_mu);

        gint64 deadline = g_get_monotonic_time() + (gint64)PROC_INTERVAL_SEC * G_USEC_PER_SEC;
        while (G.running && g_cond_wait_until(&G.wait_cond, &G.wait_mu, deadline)) {
                                                                
        }
    }
    g_mutex_unlock(&G.wait_mu);

    PCV_LOG_INFO(PROC_LOG_DOM, "Process monitor stopped");
    return NULL;
}

                                                                 

   
                                      
  
                                                  
                                                   
  
                                        
  
              
      
                   
                              
                    
                           
                       
                       
                              
                             
      
  
                                                                              
                       
                                                                              
  
                                                    
  
               
                                                     
  
                                         
                                              
  
               
                                              
  
                                         
                                 
  
                                                                 
                                   
  
                             
                                         
   
static JsonObject *
_proc_to_json(const ProcInfo *p)
{
    JsonObject *obj = json_object_new();
    json_object_set_int_member   (obj, "pid",       p->pid);
    json_object_set_string_member(obj, "comm",      p->comm);
    gchar state_str[2] = {p->state, '\0'};
    json_object_set_string_member(obj, "state",     state_str);
    json_object_set_double_member(obj, "cpu_percent",p->cpu_percent);
    json_object_set_double_member(obj, "mem_mb",
        (gdouble)p->rss_pages * G.page_size / (1024.0 * 1024.0));
    json_object_set_int_member   (obj, "rss_kb",    p->rss_pages * (G.page_size / 1024));
    json_object_set_int_member   (obj, "io_rd_bytes",p->io_rd_bytes);
    json_object_set_int_member   (obj, "io_wr_bytes",p->io_wr_bytes);
    json_object_set_string_member(obj, "type",      _proc_type_str[p->type]);
    json_object_set_string_member(obj, "cgroup",    p->cgroup);
    return obj;
}

                                                                 

   
                                           
  
         
                  
                                                     
                                                                      
                                                                   
                                                
  
                               
  
                                                   
                                
   
void
pcv_process_monitor_init(void)
{
    g_mutex_init(&G.mu);
    g_mutex_init(&G.wait_mu);
    g_cond_init(&G.wait_cond);
    G.page_size = sysconf(_SC_PAGESIZE);
    G.clk_tck = sysconf(_SC_CLK_TCK);
    if (G.clk_tck <= 0) G.clk_tck = 100;
    G.count = 0;
    G.observed_at = 0;
    G.snapshot_truncated = FALSE;
    G.service_procs = g_array_sized_new(FALSE, FALSE, sizeof(ServiceProcInfo), 64);
    G.service_source_count = 0;
    G.service_observed_at = 0;
    G.service_snapshot_truncated = FALSE;
    G.prev_ticks = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_mutex_lock(&G.wait_mu);
    G.running = TRUE;
    g_mutex_unlock(&G.wait_mu);
    G.initialized = TRUE;
    G.thread = g_thread_new("proc-monitor", _proc_thread, NULL);
}

   
                                      
  
         
                                                     
                                                                                   
                                
                                   
                                              
  
                         
                                                       
  
                                            
               
   
void
pcv_process_monitor_shutdown(void)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.wait_mu);
    G.running = FALSE;
    g_cond_signal(&G.wait_cond);
    g_mutex_unlock(&G.wait_mu);
    if (G.thread) {
        g_thread_join(G.thread);
        G.thread = NULL;
    }
    if (G.prev_ticks) {
        g_hash_table_destroy(G.prev_ticks);
        G.prev_ticks = NULL;
    }
    if (G.service_procs) {
        g_array_unref(G.service_procs);
        G.service_procs = NULL;
    }
    g_mutex_clear(&G.mu);
    g_cond_clear(&G.wait_cond);
    g_mutex_clear(&G.wait_mu);
    G.initialized = FALSE;
}

   
                                           
  
                                                
  
                                                       
                                              
  
                                       
                          
  
                                                      
                                                                
   
JsonArray *
pcv_process_monitor_get_top(gint n)
{
    JsonArray *arr = json_array_new();
    g_mutex_lock(&G.mu);
    gint limit = (n > 0 && n < G.count) ? n : G.count;
    for (gint i = 0; i < limit; i++)
        json_array_add_object_element(arr, _proc_to_json(&G.procs[i]));
    g_mutex_unlock(&G.mu);
    return arr;
}

   
                                       
  
                                                 
  
                                                    
                             
  
                                                                
   
JsonArray *
pcv_process_monitor_get_all(void)
{
    return pcv_process_monitor_get_top(G.count);
}

   
                                           
  
                                                      
                                      
  
                                                            
                                    
   
static gint
_parse_type_filter(const gchar *type_str)
{
    if (!type_str || !*type_str) return -1;
    if (g_strcmp0(type_str, "host") == 0)      return PROC_HOST;
    if (g_strcmp0(type_str, "vm") == 0)        return PROC_VM;
    if (g_strcmp0(type_str, "container") == 0) return PROC_CONTAINER;
    if (g_strcmp0(type_str, "system") == 0)    return PROC_SYSTEM;
    return -1;
}

   
                                                     
  
                                                
  
                                                       
                           
                                        
  
                                             
                                             
                                                    
                                                          
                                              
                                                          
   
JsonArray *
pcv_process_monitor_get_filtered_snapshot(gint n,
                                          const gchar *type_str,
                                          gint64 *observed_at,
                                          gint *source_count,
                                          gboolean *truncated)
{
    gint type_filter = _parse_type_filter(type_str);
    JsonArray *arr = json_array_new();
    g_mutex_lock(&G.mu);
    const gint64 snapshot_time = G.observed_at;
    if (observed_at) *observed_at = snapshot_time;
    if (source_count) *source_count = G.count;
    if (truncated) *truncated = G.snapshot_truncated;
    gint added = 0;
    for (gint i = 0; i < G.count; i++) {
        if (type_filter >= 0 && (gint)G.procs[i].type != type_filter) continue;
        json_array_add_object_element(arr, _proc_to_json(&G.procs[i]));
        added++;
        if (n > 0 && added >= n) break;
    }
    g_mutex_unlock(&G.mu);
    return arr;
}

   
                                                                       
  
                                                                      
                                                                
                                                                    
                                                         
   
JsonArray *
pcv_process_monitor_get_service_snapshot(gint64 *observed_at,
                                         gint *source_count,
                                         gboolean *truncated)
{
    JsonArray *arr = json_array_new();
    g_mutex_lock(&G.mu);
    if (observed_at) *observed_at = G.service_observed_at;
    if (source_count) *source_count = G.service_source_count;
    if (truncated) *truncated = G.service_snapshot_truncated;
    for (guint i = 0; i < G.service_procs->len; i++) {
        const ServiceProcInfo *process = &g_array_index(
            G.service_procs, ServiceProcInfo, i);
        JsonObject *item = json_object_new();
        json_object_set_int_member(item, "pid", process->pid);
        json_object_set_string_member(item, "cgroup", process->cgroup);
        gchar state[2] = {process->state, '\0'};
        json_object_set_string_member(item, "state", state);
        json_array_add_object_element(arr, item);
    }
    g_mutex_unlock(&G.mu);
    return arr;
}

JsonArray *
pcv_process_monitor_get_filtered(gint n, const gchar *type_str)
{
    const gint type_filter = _parse_type_filter(type_str);
    if (type_filter < 0)
        return n > 0 ? pcv_process_monitor_get_top(n)
                     : pcv_process_monitor_get_all();
                                                             
    return pcv_process_monitor_get_filtered_snapshot(
        n, type_str, NULL, NULL, NULL);
}
