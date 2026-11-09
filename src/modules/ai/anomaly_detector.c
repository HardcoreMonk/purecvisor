   
                           
                                              
  
                           
                                                   
                                                    
                                        
  
                                                  
                                                   
                                                   
                                    
  
                                                      
                                        
                                                  
                                                       
  
          
                                                     
                                                  
                                                     
  
            
                   
                                                     
                             
                                                                       
                                                         
  
                           
                                                    
                                                  
                                                                
                                                 
  
                                                       
                                                           
                                                           
                                                                  
                                                             
  
                                                 
                                                       
  
                          
                                
                                          
                                                               
  
          
                                           
                                                 
                                             
  
          
                                                                         
                                                          
                                       
  
             
                                                   
                            
  
           
                                    
  
                                     
   
#include "anomaly_detector.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "modules/daemons/prometheus_exporter.h"
#include "modules/daemons/cgroup_psi.h"                                                            
#include "modules/audit/pcv_audit.h"
#include "modules/ai/self_healing.h"                                   
#include "utils/pcv_log.h"

  
                                                                               
                                    
                                                                               
  
             
                                           
                           
                                                    
  
                                   
                                                     
                                                 
  
               
                                     
                                 
                                                    
                                                
                                           
  
        
                                            
                                      
                                                                               
   

#define ANOMALY_LOG_DOM   "anomaly"
constexpr int ANOMALY_WINDOW    = 60;                                                
constexpr int ANOMALY_COOLDOWN  = 30;                                 
constexpr int MAX_RECENT_EVENTS = 20;                                     

                                                                       
  
                                                     
                                                
  
                                                              
                                                     
                                                    
                                                         
                                                           
  
             
                                                                           
                                                   
                                                    
                                         
  
                                                               
                                                       
                                                          
                                               
                                                         
                                                             
                                                         
  
                                                            
   
constexpr int MAX_AUTO_WATCHED      = PCV_CGROUP_PSI_MAX_ENTRIES;                     
constexpr int ANOMALY_FIXED_RESERVE = 16;                                
constexpr int MAX_WATCHED           = MAX_AUTO_WATCHED + ANOMALY_FIXED_RESERVE;                     

                                                 
                                                      
                             
constexpr int AUTO_RECLAIM_MISSES = 3;

                                                        
constexpr int AUTO_DROP_WARN_INTERVAL_SEC = 300;

                                                                   
  
                                                  
                                                     
                             
  
                                                  
                                         
                                                         
                                                                       
                                         
                                                        
                                                      
                                                              
  
                                                                    
                                                                      
                                                         
                                                         
                                                             
                                                    
                                                        
                                                     
                                                        
                                                    
   
constexpr int ANOMALY_LABELS_BUF = 160;
static_assert(ANOMALY_LABELS_BUF >= (PCV_CGROUP_PSI_NAME_MAX - 1) + 18 + 9 + 1,
              "label buffer must fit the longest cgroup_psi label (name + kind)");

                                                          
                                                      
                                                           
static_assert(ANOMALY_WINDOW >= 10, "Window too small for meaningful Z-Score");
static_assert(MAX_WATCHED >= 1);
                                             
                                                           
static_assert(MAX_AUTO_WATCHED + ANOMALY_FIXED_RESERVE <= MAX_WATCHED,
              "auto watch budget must not encroach on the fixed-watch reserve");
static_assert(AUTO_RECLAIM_MISSES >= 1);

                                                          

   
                                       
  
                                 
                                               
                                                    
                                                
                                   
   
typedef struct {
    gchar    name[128];                                                                       
                                                                 
    gchar    labels[ANOMALY_LABELS_BUF];                                                       
    gdouble  ring[ANOMALY_WINDOW];                          
    gint     pos;                                                    
    gint     count;                                                  
    gdouble  sum;                                       
    gdouble  sum_sq;                                     
    gint64   last_alert_us;                                        
    gdouble  threshold;                                            
    gdouble  last_zscore;                                                   
                                                                
                                                        
                                                       
                                              
    gboolean auto_registered;                                   
    gboolean missing_now;                                                 
    gint     miss_streak;                                                      
                                                                               
    gboolean has_prev_raw;                                                 
    gdouble  prev_raw;                         
} AnomalyMetric;

                                                       

   
                                  
  
                                             
                                        
   
typedef struct {
    gchar    metric[128];                           
                                                                       
                                                      
    gchar    labels[ANOMALY_LABELS_BUF];                 
    gdouble  value;                                     
    gdouble  zscore;                                   
    gdouble  mean;                                 
    gint64   timestamp_us;                                        
} AnomalyEvent;

                                                             

static struct {
    AnomalyMetric  watched[MAX_WATCHED];                                   
    gint           watch_count;                                             
    gint           auto_count;                                                             
    gint           auto_dropped;                                                      
    gint64         last_drop_warn_us;                                        
    AnomalyEvent   recent[MAX_RECENT_EVENTS];                  
    gint           recent_pos;                                      
    gint           recent_count;                               
    GMutex         mu;                                         
    gboolean       initialized;                              
    guint64        total_alerts;                                            
    gint           active_anomalies;                                         
} G = {0};

                                                      

   
         
             
  
                              
                                
  
                                    
                           
   
static gdouble
_mean(const AnomalyMetric *m)
{
    if (m->count == 0) return 0.0;                 
    return m->sum / (gdouble)m->count;
}

   
           
             
  
                                                  
                                     
  
                                
  
                                           
                             
   
static gdouble
_stddev(const AnomalyMetric *m)
{
    if (m->count < 2) return 0.0;
    gdouble n = (gdouble)m->count;
                                                           
    gdouble variance = (m->sum_sq / n) - (m->sum / n) * (m->sum / n);
                                                          
    return variance > 0.0 ? sqrt(variance) : 0.0;
}

   
           
                               
                 
  
                                              
                                           
  
                                                 
                                           
   
static gdouble
_zscore(const AnomalyMetric *m, gdouble value)
{
    gdouble sd = _stddev(m);
    if (sd < 1e-9) return 0.0;                                         
    return fabs(value - _mean(m)) / sd;                                  
}

   
                         
                
  
                                                                      
                                                                               
  
                                        
                       
   
static gboolean
_metric_is_cumulative(const gchar *name)
{
    return g_str_has_suffix(name, "_total") || g_str_has_prefix(name, "node_vmstat_");
}

   
         
                 
                
  
                                 
  
         
                                                
                                       
                                                                                      
                                                
  
                                                   
                                             
   
static gboolean
_push(AnomalyMetric *m, gdouble value)
{
                                         
                                                        
                                                         
    if (m->count >= ANOMALY_WINDOW) {
        gdouble old = m->ring[m->pos];
        m->sum -= old;
        m->sum_sq -= old * old;
    } else {
        m->count++;                                       
    }

                                     
    m->ring[m->pos] = value;
    m->sum += value;
    m->sum_sq += value * value;
    m->pos = (m->pos + 1) % ANOMALY_WINDOW;                            

                                                        
    if (m->count < 10) {
        m->last_zscore = 0.0;                                  
        return FALSE;
    }

    gdouble z = _zscore(m, value);
    if (_metric_is_cumulative(m->name) && value < _mean(m))
        z = 0.0;                                                
    m->last_zscore = z;                                                        

    if (z > m->threshold) {
        gint64 now = g_get_monotonic_time();
                                                      
                                                         
                                                           
        if (now - m->last_alert_us < ANOMALY_COOLDOWN * G_USEC_PER_SEC)
            return FALSE;
        m->last_alert_us = now;                          
        return TRUE;
    }
    return FALSE;
}

   
                  
                  
                          
                           
  
                                                   
                                                                       
                                                   
                                                                
  
                                                                   
                                 
   
static gboolean
_metric_sample(AnomalyMetric *m, gdouble raw, gdouble *sample)
{
    if (!_metric_is_cumulative(m->name)) {
        *sample = raw;
        return TRUE;
    }
    const gboolean usable = m->has_prev_raw && raw >= m->prev_raw;
    *sample = usable ? raw - m->prev_raw : 0.0;
    m->prev_raw = raw;
    m->has_prev_raw = TRUE;
    return usable;
}

                                                             

   
              
                                
                                    
                                            
  
                                 
  
                                                                
                                                        
                                                       
                                                   
                                 
  
                                                    
                                                    
   
static void
_add_watch(const gchar *name, const gchar *labels, gdouble threshold)
{
    if (G.watch_count >= ANOMALY_FIXED_RESERVE) {
        PCV_LOG_WARN(ANOMALY_LOG_DOM,
            "fixed watch reserve exhausted (%d) — '%s' NOT watched "
            "(raise ANOMALY_FIXED_RESERVE together with MAX_WATCHED)",
            ANOMALY_FIXED_RESERVE, name);
        return;
    }
    AnomalyMetric *m = &G.watched[G.watch_count++];
    memset(m, 0, sizeof(*m));                                  
    g_strlcpy(m->name, name, sizeof(m->name));
    if (labels) g_strlcpy(m->labels, labels, sizeof(m->labels));
    m->threshold = threshold;
}

                                                                             
                                      
                                                                             
  
                   
                                                    
                                                
                                                  
                                         
  
                          
                                                    
                                                  
                                                      
                                                    
   

   
                                       
  
                                                        
                                                         
   
typedef struct {
    const gchar *name;                                       
    gdouble      threshold;                                 
} AutoWatchSpec;

               
  
                                                                 
                                                    
                                                       
                                                 
  
                                                          
static const AutoWatchSpec AUTO_WATCH_SPECS[] = {
    { PCV_ANOMALY_AUTO_METRIC_MEM_FULL, 2.5 },
};

   
                                                                
  
                                               
  
                                                 
                                                      
   
gboolean
pcv_anomaly_extract_vm_name(const gchar *labels, gchar *out, gsize out_sz)
{
    if (out && out_sz > 0) out[0] = '\0';                               
    if (!labels || !out || out_sz == 0) return FALSE;

    const gchar *p = labels;
    while (*p) {
                                
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;

                        
        const gchar *key = p;
        while (*p && *p != '=' && *p != ',' && *p != ' ' && *p != '\t') p++;
        const gsize key_len = (gsize)(p - key);
        while (*p == ' ' || *p == '\t') p++;

        if (*p != '=') {
                                                       
                                                           
                                        
            while (*p && *p != ',') p++;
            continue;
        }
        p++;                                               
        while (*p == ' ' || *p == '\t') p++;

        if (*p != '"') {
                                                            
                                                      
                                                    
                                                      
            while (*p && *p != ',') p++;
            continue;
        }
        p++;                                                  

                                                             
                                                         
        const gchar *val = p;
        gboolean has_escape = FALSE;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) { has_escape = TRUE; p += 2; continue; }
            p++;
        }
        if (*p != '"') return FALSE;                                           
        const gsize val_len = (gsize)(p - val);
        p++;                                                  

        if (key_len != 7 || strncmp(key, "vm_name", 7) != 0)
            continue;                                                                    

        if (val_len == 0) return FALSE;                                    
        if (has_escape) {
                                                        
                                                           
                                                     
                                                        
            return FALSE;
        }
        if (val_len >= out_sz) return FALSE;                                      
        memcpy(out, val, val_len);
        out[val_len] = '\0';
        return TRUE;
    }
    return FALSE;
}

   
                                                   
  
                                               
                                             
                                                    
  
                                              
   
gboolean
pcv_anomaly_series_labels(const gchar *line, const gchar *metric_name,
                          gchar *out, gsize out_sz)
{
    if (out && out_sz > 0) out[0] = '\0';
    if (!line || !metric_name || !out || out_sz == 0) return FALSE;

    const gsize nlen = strlen(metric_name);
    if (nlen == 0) return FALSE;
    if (strncmp(line, metric_name, nlen) != 0) return FALSE;
                                                              
                                        
    if (line[nlen] != '{') return FALSE;

    const gchar *lb = line + nlen + 1;                             
    const gchar *rb = strchr(lb, '}');
    if (!rb) return FALSE;

                                                   
                                                      
    const gchar *nl = strchr(lb, '\n');
    if (nl && nl < rb) return FALSE;

    const gsize len = (gsize)(rb - lb);
    if (len == 0) return FALSE;                                                    
    if (len >= out_sz) {
                                                          
                                                             
                                                                 
                                                             
                                                                
                                               
        static gboolean warned = FALSE;
        if (!warned) {
            warned = TRUE;
            PCV_LOG_WARN(ANOMALY_LOG_DOM,
                "Label set too long (%zu >= %zu) for metric '%s' — this series is "
                "skipped for anomaly auto-watch (no purecvisor_anomaly_score will be "
                "emitted for it). Shorten the guest name. head=%.64s",
                len, out_sz, metric_name, lb);
        }
        return FALSE;
    }
    memcpy(out, lb, len);
    out[len] = '\0';
    return TRUE;
}

   
                                                       
                                        
  
                                                 
                                               
  
                                                             
   
static gboolean
_touch_auto_watch(const gchar *name, const gchar *labels, gdouble threshold)
{
    for (gint i = 0; i < G.watch_count; i++) {
        AnomalyMetric *m = &G.watched[i];
        if (g_strcmp0(m->name, name) != 0) continue;
        if (g_strcmp0(m->labels, labels) != 0) continue;
                                                  
                                                
                                                         
                                                         
        m->missing_now = FALSE;
        return TRUE;
    }

                                                  
                                                           
                                              
    if (G.auto_count >= MAX_AUTO_WATCHED) return FALSE;
    if (G.watch_count >= MAX_WATCHED) return FALSE;

    AnomalyMetric *m = &G.watched[G.watch_count++];
    memset(m, 0, sizeof(*m));                                           
    g_strlcpy(m->name, name, sizeof(m->name));
    g_strlcpy(m->labels, labels, sizeof(m->labels));
    m->threshold = threshold;
    m->auto_registered = TRUE;
    m->missing_now = FALSE;
    G.auto_count++;
    return TRUE;
}

   
                                                           
                           
  
                                                      
                                               
  
                                                   
                                              
  
                                                                     
                                                     
                             
   
static gint
_sync_auto_watches_locked(const gchar *render_text, gint *out_dropped)
{
    gint dropped = 0;

                                                     
    for (gint i = 0; i < G.watch_count; i++)
        G.watched[i].missing_now = TRUE;

                                                     
    for (const gchar *line = render_text; line && *line; ) {
        const gchar *eol = strchr(line, '\n');

                                                       
                                                    
        if (*line != '#') {
            for (gsize k = 0; k < G_N_ELEMENTS(AUTO_WATCH_SPECS); k++) {
                                                         
                                                         
                gchar labels[sizeof(G.watched[0].labels)];
                if (!pcv_anomaly_series_labels(line, AUTO_WATCH_SPECS[k].name,
                                               labels, sizeof(labels)))
                    continue;
                if (!_touch_auto_watch(AUTO_WATCH_SPECS[k].name, labels,
                                       AUTO_WATCH_SPECS[k].threshold))
                    dropped++;
                break;                            
            }
        }

        if (!eol) break;
        line = eol + 1;
    }

                                                      
                                                          
    gint kept = 0;
    for (gint i = 0; i < G.watch_count; i++) {
        AnomalyMetric *m = &G.watched[i];

        if (m->auto_registered) {
            if (m->missing_now) {
                m->miss_streak++;
                if (m->miss_streak >= AUTO_RECLAIM_MISSES) {
                    G.auto_count--;
                    continue;                                     
                }
            } else {
                m->miss_streak = 0;                                      
            }
        }

        if (kept != i) G.watched[kept] = *m;                       
        kept++;
    }
    G.watch_count = kept;

    G.auto_dropped = dropped;
    if (out_dropped) *out_dropped = dropped;
    return G.auto_count;
}

   
                                                                    
                                         
  
                                                      
                                                         
                                                           
                                                 
                                              
            
  
                                                        
gint
pcv_anomaly_sync_auto_watches(const gchar *render_text, gint *out_dropped)
{
    if (out_dropped) *out_dropped = 0;
    if (!G.initialized) return 0;

    g_mutex_lock(&G.mu);
    gint n = _sync_auto_watches_locked(render_text, out_dropped);
    g_mutex_unlock(&G.mu);
    return n;
}

   
                                                 
                                         
  
                                                              
                                                    
                                                
  
                                                        
gint
pcv_anomaly_watch_count(void)
{
    if (!G.initialized) return 0;
    g_mutex_lock(&G.mu);
    gint n = G.watch_count;
    g_mutex_unlock(&G.mu);
    return n;
}

   
                                                                
  
                                                
  
                                                          
                                                   
                                                  
                                             
  
                                                  
                                                              
                                    
  
                                                             
                                                                   
                                    
   
static void
_score_labels(const AnomalyMetric *m, gchar *out, gsize out_sz)
{
    if (m->auto_registered && m->labels[0])
        g_snprintf(out, out_sz, "metric=\"%s\",%s", m->name, m->labels);
    else
        g_snprintf(out, out_sz, "metric=\"%s\"", m->name);
}

                                               
                                                                       
                                                           
  
                                                           
                                                               
                                                
                                                                                      
                                                 
                                          
  
                                                                 
                                                      
                                                            
                                                      
constexpr int ANOMALY_SCORE_LABEL_BUF = 320;
static_assert(ANOMALY_SCORE_LABEL_BUF > PCV_PROM_LABELS_MAX,
              "score label buffer must not truncate before the registry can reject");
static_assert(ANOMALY_SCORE_LABEL_BUF >= 9 + 127 + 2 + (ANOMALY_LABELS_BUF - 1) + 1,
              "score label buffer must fit metric name + discovered labels");

                                                  
                                                                    
                                                           
                                                       
                                                        
                                                                   
                                                        
                                                        
constexpr int ANOMALY_ALERT_PAYLOAD_BUF = 640;
static_assert(ANOMALY_ALERT_PAYLOAD_BUF >= 65 + 127 + 2 * (ANOMALY_LABELS_BUF - 1) + 4 * 32 + 1,
              "alert payload must fit metric + escaped labels + numeric fields");

                                                                
                                                                   
                                                   
                                                  
                                                         
constexpr int ANOMALY_SEARCH_KEY_BUF = 320;
static_assert(ANOMALY_SEARCH_KEY_BUF >= 127 + 1 + (ANOMALY_LABELS_BUF - 1) + 1 + 1,
              "search key must fit `name{labels}` without truncation");

                                                         

   
               
                      
                         
  
                             
                                                                        
                                                  
                                              
  
                                                     
  
                                                    
                                        
   
static void
_emit_alert(AnomalyMetric *m, gdouble value)
{
    gdouble z = m->last_zscore;                                          
    gdouble mean = _mean(m);

    G.total_alerts++;                                 

                                                                  
    AnomalyEvent *ev = &G.recent[G.recent_pos];
    g_strlcpy(ev->metric, m->name, sizeof(ev->metric));
    g_strlcpy(ev->labels, m->labels, sizeof(ev->labels));
    ev->value = value;
    ev->zscore = z;
    ev->mean = mean;
    ev->timestamp_us = g_get_real_time();                                   
    G.recent_pos = (G.recent_pos + 1) % MAX_RECENT_EVENTS;              
    if (G.recent_count < MAX_RECENT_EVENTS) G.recent_count++;

                                           
                                                      
                                                         
    gchar vm_buf[PCV_ANOMALY_VM_NAME_MAX];
    const gchar *target_vm =
        pcv_anomaly_extract_vm_name(m->labels, vm_buf, sizeof(vm_buf)) ? vm_buf : NULL;

                                                                   
    gchar lbl[ANOMALY_SCORE_LABEL_BUF];
    _score_labels(m, lbl, sizeof(lbl));
    pcv_prom_gauge_set_labels("purecvisor_anomaly_score", lbl, z);

                                                                          
    {
        extern void pcv_ws_broadcast(const gchar *type, const gchar *payload);
        extern gint pcv_ws_client_count(void);
        if (pcv_ws_client_count() > 0) {
                                                                   
                                                        
                                                     
                                                         
                                                        
                                                             
                                             
            gchar *labels_esc = pcv_json_escape(m->labels);
            gchar payload[ANOMALY_ALERT_PAYLOAD_BUF];
            g_snprintf(payload, sizeof(payload),
                "{\"metric\":\"%s\",\"labels\":\"%s\",\"value\":%.2f,"
                "\"zscore\":%.2f,\"mean\":%.2f,\"threshold\":%.1f}",
                m->name, labels_esc, value, z, mean, m->threshold);
            g_free(labels_esc);
            pcv_ws_broadcast("anomaly", payload);
        }
    }

                                                 
                                                               
                                                              
    {
        gchar detail[256];
        g_snprintf(detail, sizeof(detail),
            "Z=%.2f (threshold=%.1f) value=%.2f mean=%.2f%s%s",
            z, m->threshold, value, mean,
            target_vm ? " vm=" : "", target_vm ? target_vm : "");
        pcv_audit_log("ai-ops", "anomaly_detected", m->name, detail, 0, 0, "local");
    }

    PCV_LOG_WARN(ANOMALY_LOG_DOM,
        "ANOMALY: %s%s%s Z=%.2f (>%.1f) val=%.2f mean=%.2f",
        m->name, target_vm ? " vm=" : "", target_vm ? target_vm : "",
        z, m->threshold, value, mean);

                                                             
                                                                      
                                                      
      
                                                                 
                                                          
                                                                 
    pcv_healing_on_anomaly(m->name, value, z, m->threshold, target_vm);
}

                                                                  

   
                    
  
                                    
                                     
                                         
                                              
                                              
                                        
  
                                                        
                                       
  
                                                       
                                                             
                                                      
  
                                                 
   
void
pcv_anomaly_init(void)
{
    g_mutex_init(&G.mu);
    G.initialized = TRUE;

                                                                          
                                                               
    _add_watch("purecvisor_host_cpu_percent", "", 2.5);
    _add_watch("purecvisor_host_memory_percent", "", 2.5);
                                                                                    
                                                                     
    _add_watch("node_disk_io_time_seconds_total", "", 3.0);
    _add_watch("node_network_receive_errs_total", "", 2.0);
    _add_watch("purecvisor_rpc_duration_ms", "method=\"vm.list\"", 3.0);
    _add_watch("node_vmstat_pswpout", "", 2.5);
    _add_watch("node_pressure_cpu_some_seconds_total", "", 2.0);
    _add_watch("node_hwmon_temp_celsius", "chip=\"coretemp\",sensor=\"temp1\"", 2.0);
    _add_watch("node_hwmon_temp_celsius", "chip=\"k10temp\",sensor=\"temp1\"", 2.0);
    _add_watch("node_nf_conntrack_entries", "", 2.5);

    PCV_LOG_INFO(ANOMALY_LOG_DOM,
        "Anomaly detector initialized — %d metrics watched", G.watch_count);
}

                                                 
                            
void
pcv_anomaly_shutdown(void)
{
    if (!G.initialized) return;
    G.initialized = FALSE;
    g_mutex_clear(&G.mu);
}

static void _evaluate_render_locked(const gchar *last_render);

                                                           
                                                           
static gdouble
_line_value(const gchar *line)
{
    const gchar *eol = strchr(line, '\n');
    if (!eol) eol = line + strlen(line);
    const gchar *sp = eol;
    while (sp > line && sp[-1] != ' ') sp--;
    if (sp == line || sp == eol) return NAN;
    gchar *end = NULL;
    gdouble v = g_ascii_strtod(sp, &end);
                                                                   
    return (end == sp || !isfinite(v)) ? NAN : v;
}

                                         
                                                       
                              
gdouble
pcv_anomaly_read_value(const gchar *render_text, const gchar *name, const gchar *labels)
{
    if (!render_text || !name || !*name) return NAN;
    gsize name_len = strlen(name);
    gsize labels_len = labels ? strlen(labels) : 0;
    gdouble sum = 0.0;
    gboolean summed = FALSE;                              
                                                                            
                                                                        
    for (const gchar *line = render_text; line && *line; ) {
        const gchar *next = strchr(line, '\n');
        if (strncmp(line, name, name_len) == 0) {
            const gchar *after = line + name_len;
            if (labels_len) {
                                                                  
                if (*after == '{' && strncmp(after + 1, labels, labels_len) == 0
                    && after[1 + labels_len] == '}')
                    return _line_value(line);
            } else if (*after == ' ') {
                return _line_value(line);                             
            } else if (*after == '{') {
                                                                         
                                                                           
                                                                         
                gdouble v = _line_value(line);
                if (!isnan(v)) { sum += v; summed = TRUE; }                             
            }
        }
        line = next ? next + 1 : NULL;
    }
    return summed ? sum : NAN;
}

   
                        
  
                                  
                                         
  
         
                                          
                             
                                      
                                    
                                                    
                              
  
                                                   
                                               
   
void
pcv_anomaly_evaluate(void)
{
    if (!G.initialized) return;

    g_mutex_lock(&G.mu);

                                                               
                                                   
                                                           
                                                     
                                                
    static gchar *last_render = NULL;                                
    static gint64 last_render_time = 0;                            

                                                                                
                                                      
                                         
      
                                                       
                                                     
                                                            
    {
        gint64 now = g_get_monotonic_time();
        if (now - last_render_time > 2 * G_USEC_PER_SEC || !last_render) {
                                                                  
                                                                         
                                                                
                                                              
            gchar *new_render = pcv_prom_render();
            if (new_render) {
                g_free(last_render);
                last_render = new_render;
                last_render_time = now;
            }
        }
    }

    _evaluate_render_locked(last_render);
    g_mutex_unlock(&G.mu);
}

                                                                                  
void
pcv_anomaly_evaluate_text(const gchar *render_text)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.mu);
    _evaluate_render_locked(render_text);
    g_mutex_unlock(&G.mu);
}

                                                                
                                                         
                                                                 
static void
_evaluate_render_locked(const gchar *last_render)
{
                                   
      
                                                           
                                                     
                                                     
    if (last_render) {
        gint dropped = 0;
        gint auto_n = _sync_auto_watches_locked(last_render, &dropped);

                                                        
                                                                       
                                                       
        pcv_prom_gauge_set_labels("purecvisor_anomaly_auto_watched", "", (gdouble)auto_n);
        pcv_prom_gauge_set_labels("purecvisor_anomaly_auto_watch_dropped", "", (gdouble)dropped);

        if (dropped > 0) {
            gint64 now = g_get_monotonic_time();
            if (now - G.last_drop_warn_us >=
                (gint64)AUTO_DROP_WARN_INTERVAL_SEC * G_USEC_PER_SEC) {
                G.last_drop_warn_us = now;
                                                                     
                                                          
                                                                
                                         
                PCV_LOG_WARN(ANOMALY_LOG_DOM,
                    "auto watch budget exhausted — %d series unwatched "
                    "(auto=%d/%d; raise MAX_AUTO_WATCHED or reduce guest count)",
                    dropped, auto_n, MAX_AUTO_WATCHED);
            }
        }
    }

    gint active = 0;                                   

    for (gint i = 0; i < G.watch_count; i++) {
        AnomalyMetric *m = &G.watched[i];

        if (!last_render) continue;                                     

                                                          
        gdouble value = pcv_anomaly_read_value(last_render, m->name, m->labels);

        if (isnan(value)) {                                                        
            m->has_prev_raw = FALSE;
            continue;
        }
        gdouble sample = value;
        if (!_metric_sample(m, value, &sample)) continue;                              

        gboolean anomaly = _push(m, sample);                        

                                                
                                                               
                                                          
        gchar lbl[ANOMALY_SCORE_LABEL_BUF];
        _score_labels(m, lbl, sizeof(lbl));
        pcv_prom_gauge_set_labels("purecvisor_anomaly_score", lbl, m->last_zscore);

        if (anomaly) {
            _emit_alert(m, sample);                                        
            active++;
        } else if (m->last_zscore > m->threshold * 0.8) {
                                                                 
                                                  
            active++;                                      
        }
    }

    G.active_anomalies = active;

                                                                        
    pcv_prom_gauge_set_labels("purecvisor_anomaly_active", "", (gdouble)active);
    pcv_prom_gauge_set_labels("purecvisor_anomaly_alerts_total", "", (gdouble)G.total_alerts);
}

   
                              
  
                                    
                                          
                                
  
          
                                                          
                                         
                               
                             
                         
  
                                                   
                                            
   
void
pcv_anomaly_reset_baseline(void)
{
    if (!G.initialized) return;

    g_mutex_lock(&G.mu);

                                              
                                                             
                        
    for (gint i = 0; i < G.watch_count; i++) {
        AnomalyMetric *m = &G.watched[i];
        memset(m->ring, 0, sizeof(m->ring));
        m->pos = 0;
        m->count = 0;
        m->sum = 0.0;
        m->sum_sq = 0.0;
        m->last_alert_us = 0;
        m->last_zscore = 0.0;
                                                        
                                                       
        m->miss_streak = 0;
        m->missing_now = FALSE;
        m->has_prev_raw = FALSE;
    }

                         
    memset(G.recent, 0, sizeof(G.recent));
    G.recent_pos = 0;
    G.recent_count = 0;

                
    G.total_alerts = 0;
    G.active_anomalies = 0;

    g_mutex_unlock(&G.mu);

    g_message("[ANOMALY] Baseline statistics reset — all %d metrics cleared",
              G.watch_count);
}

   
                               
  
                                           
                                                     
  
                                                                                                
  
                                                       
                                                    
   
gchar *
pcv_anomaly_get_recent_json(void)
{
    g_mutex_lock(&G.mu);                

    GString *buf = g_string_new("[");
                                                         
                                                         
    gint start = (G.recent_count >= MAX_RECENT_EVENTS)
        ? G.recent_pos : 0;
    gint count = G.recent_count;

    for (gint i = 0; i < count; i++) {
                                                             
        gint idx = (start + count - 1 - i) % MAX_RECENT_EVENTS;                   
        AnomalyEvent *ev = &G.recent[idx];
        if (ev->timestamp_us == 0) continue;                           
        if (buf->len > 1) g_string_append_c(buf, ',');                     
                                                                   
        gchar *labels_esc = pcv_json_escape(ev->labels);
        g_string_append_printf(buf,
            "{\"metric\":\"%s\",\"labels\":\"%s\",\"value\":%.2f,"
            "\"zscore\":%.2f,\"mean\":%.2f,\"ts\":%ld}",
            ev->metric, labels_esc, ev->value,
            ev->zscore, ev->mean, (long)(ev->timestamp_us / G_USEC_PER_SEC));
        g_free(labels_esc);
    }
    g_string_append_c(buf, ']');

    g_mutex_unlock(&G.mu);
    return g_string_free(buf, FALSE);
}
