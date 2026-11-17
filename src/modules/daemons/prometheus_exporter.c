   
                              
                                                                
  
                           
                                                   
                                                    
                                        
  
                                                     
                                               
                                                     
                                                      
                                                           
                                                                                
                              
  
                                                                               
         
                                                                               
                                                    
                                                       
  
                                                               
                                                       
  
                                                                               
           
                                                                               
  
                                
                                                            
                                                              
  
                                
                                                                
  
                                        
                                                                
  
                                                                               
                                
                                                                               
                                                                       
                                                                         
                                                                             
  
                                                                               
           
                                                                               
                                     
                                          
                                             
                                                  
  
                                                                               
           
                                                                               
                                                     
                                                                                                       
                                                                    
                                                                 
  
                                                                               
                                
                                                                               
                    
                                         
                 
                          
  
                                                            
                                                            
  
                                                                               
       
                                                                               
                                                           
                                     
   
#include "prometheus_exporter.h"
#include "purecvisor/version.h"
#include "utils/pcv_log.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <errno.h>                                                

  
                                                                               
                                           
                                                                               
  
                     
                                                    
                                             
  
                   
                                                    
                                          
                                             
  
                    
                                                               
                                                   
                                                                               
   

                                                     
#define PROM_LOG_DOM "prom_export"

   
                     
                                    
  
                                 
                                                                     
   
typedef struct {
    gchar  name[128];                                                         
                                                               
                                                     
    gchar  labels[PCV_PROM_LABELS_MAX];                                                         
                                                                          
                                                                                       
    gdouble value;                                                
                                                                                           
                                                                                                      
                                                                      
    PcvPromKind kind;                                   
    gboolean kind_warned;                                           
    gint64  last_update;                                                         
} PromMetric;

                                                                        
static const gchar *
_kind_type(PcvPromKind k)
{
    return k == PCV_PROM_KIND_GAUGE ? "gauge" : "counter";
}

                 
static const gchar *
_kind_name(PcvPromKind k)
{
    return k == PCV_PROM_KIND_GAUGE ? "gauge" : k == PCV_PROM_KIND_COUNTER ? "counter" : "source-counter";
}

   
                                        
  
                                                               
  
                                                                       
                                                           
                                 
   
static struct {
    PromMetric  metrics[PCV_PROM_MAX_METRICS];                         
    gint        count;                                      
    GMutex      mu;                                               
    gboolean    initialized;                                             
    GHashTable *index;                                                                           
} G = {0};

                                                            
                                                    
static_assert(sizeof(((PromMetric *)0)->labels) == PCV_PROM_LABELS_MAX,
              "PromMetric.labels must match PCV_PROM_LABELS_MAX");

   
                                                    
  
                                                   
                                        
  
                                                              
                                                            
                                                       
                                      
  
                                                             
                                                 
  
                                          
                                                
                                                      
   
static gboolean
_labels_over_budget(const gchar *name, const gchar *labels)
{
                                                         
                                                                       
                                                                 
                                                              
    if (pcv_prom_labels_fit(labels)) return FALSE;

    static gboolean warned = FALSE;
    if (!warned) {
        warned = TRUE;
        PCV_LOG_WARN(PROM_LOG_DOM,
            "Label string too long (%d >= %d) for metric '%s' — rejecting "
            "registration. Truncating would emit a malformed /metrics line "
            "(unbalanced quote). Shorten the guest/label name. head=%.64s",
            (gint)strlen(labels), PCV_PROM_LABELS_MAX,
            name ? name : "(null)", labels);
    }
    return TRUE;
}

   
                                                       
  
                                                    
                                                     
  
                                                  
                                                
                                                  
                                                    
  
                                         
  
                                                                 
                                                        
                                           
  
                                                                 
                                                       
                                                                                 
                                                               
  
                                            
   
static gint
_find_or_create(const gchar *name, const gchar *labels, PcvPromKind kind)
{
                                                    
                                                
                                                
    if (_labels_over_budget(name, labels)) return -1;

                                                                           
                                                                          
                                               
    gint64 now = g_get_monotonic_time();
    if (G.index) {
                                                   
        gchar *key = g_strdup_printf("%s\x01%s", name, labels ? labels : "");
        gpointer stored;
        gboolean found = g_hash_table_lookup_extended(G.index, key, NULL, &stored);
        if (found) {
            gint idx = GPOINTER_TO_INT(stored);
            G.metrics[idx].last_update = now;                     
                                                                                
                                                                    
            if (G.metrics[idx].kind != kind && !G.metrics[idx].kind_warned) {
                G.metrics[idx].kind_warned = TRUE;
                PCV_LOG_WARN(PROM_LOG_DOM, "Metric kind mismatch: %s{%s} registered as %s, now %s — keeping first",
                             name, labels ? labels : "", _kind_name(G.metrics[idx].kind), _kind_name(kind));
            }
            g_free(key);
            return idx;
        }
                                         
        if (G.count >= PCV_PROM_MAX_METRICS) {
                                                       
            static gboolean warned = FALSE;
            if (!warned) {
                warned = TRUE;
                PCV_LOG_WARN(PROM_LOG_DOM,
                    "Metric pool saturated (%d) — dropping new labels. "
                    "Consider increasing MAX_METRICS or reducing label cardinality",
                    PCV_PROM_MAX_METRICS);
            }
            g_free(key);
            return -1;
        }
        gint idx = G.count++;
        g_strlcpy(G.metrics[idx].name,   name,             sizeof(G.metrics[idx].name));
        g_strlcpy(G.metrics[idx].labels, labels ? labels : "", sizeof(G.metrics[idx].labels));
        G.metrics[idx].value      = 0;
        G.metrics[idx].kind       = kind;
        G.metrics[idx].kind_warned = FALSE;
        G.metrics[idx].last_update = now;
                                                                       
        g_hash_table_insert(G.index, key, GINT_TO_POINTER(idx));
        return idx;
    }

                                                      
                                                                
                                                                           
                                                                  
    for (gint i = 0; i < G.count; i++) {
        if (g_strcmp0(G.metrics[i].name, name) == 0 &&
            g_strcmp0(G.metrics[i].labels, labels) == 0) {
            G.metrics[i].last_update = now;                     
            return i;                                                  
        }
    }
    if (G.count >= PCV_PROM_MAX_METRICS) {
        static gboolean warned = FALSE;
        if (!warned) {
            warned = TRUE;
            PCV_LOG_WARN(PROM_LOG_DOM,
                "Metric pool saturated (%d) — dropping new labels. "
                "Consider increasing MAX_METRICS or reducing label cardinality",
                PCV_PROM_MAX_METRICS);
        }
        return -1;
    }
    gint idx = G.count++;
    g_strlcpy(G.metrics[idx].name,   name,             sizeof(G.metrics[idx].name));
    g_strlcpy(G.metrics[idx].labels, labels ? labels : "", sizeof(G.metrics[idx].labels));
    G.metrics[idx].value      = 0;
    G.metrics[idx].kind       = kind;
    G.metrics[idx].kind_warned = FALSE;
    G.metrics[idx].last_update = now;
    return idx;
}

   
                                         
                                                                
                                          
   
#define PCV_PROM_STALE_TTL_US (5 * 60 * G_USEC_PER_SEC)          

                                      
#define PCV_PROM_CHECKPOINT_PATH "/var/lib/purecvisor/prom_counters.json"
#define PCV_PROM_CHECKPOINT_INTERVAL_SEC 60

   
                           
                                                
                                                 
                                  
                                              
                        
                             
   
static void
_counters_save_unlocked(void)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "saved_at");
    json_builder_add_int_value(b, (gint64)g_get_real_time() / G_USEC_PER_SEC);
    json_builder_set_member_name(b, "counters");
    json_builder_begin_array(b);
    for (gint i = 0; i < G.count; i++) {
                                                                                                 
        if (G.metrics[i].kind != PCV_PROM_KIND_COUNTER) continue;
        if (G.metrics[i].value <= 0.0) continue;
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "name");
        json_builder_add_string_value(b, G.metrics[i].name);
        json_builder_set_member_name(b, "labels");
        json_builder_add_string_value(b, G.metrics[i].labels);
        json_builder_set_member_name(b, "value");
        json_builder_add_double_value(b, G.metrics[i].value);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);

    JsonNode *root = json_builder_get_root(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *data = json_generator_to_data(gen, NULL);
    json_node_free(root);
    g_object_unref(gen);
    g_object_unref(b);

                                                        
                                                 
                                    
                                                                           
    if (g_mkdir_with_parents("/var/lib/purecvisor", 0755) != 0) {
        PCV_LOG_WARN(PROM_LOG_DOM,
            "Cannot create checkpoint dir /var/lib/purecvisor: %s",
            g_strerror(errno));
        g_free(data);
        return;
    }
    gchar *tmp_path = g_strdup_printf("%s.tmp", PCV_PROM_CHECKPOINT_PATH);
    GError *werr = NULL;
    if (g_file_set_contents(tmp_path, data, -1, &werr)) {
        if (g_rename(tmp_path, PCV_PROM_CHECKPOINT_PATH) != 0) {
            PCV_LOG_WARN(PROM_LOG_DOM, "checkpoint rename failed: %s",
                         g_strerror(errno));
        }
    } else {
        PCV_LOG_WARN(PROM_LOG_DOM, "checkpoint write failed: %s",
                     werr ? werr->message : "unknown");
        g_clear_error(&werr);
    }
    g_free(tmp_path);
    g_free(data);
}

   
                  
                                                   
                             
                                      
   
static void
_counters_load(void)
{
    gchar *content = NULL;
    gsize len = 0;
    if (!g_file_get_contents(PCV_PROM_CHECKPOINT_PATH, &content, &len, NULL)) {
        return;                          
    }

    JsonParser *jp = json_parser_new();
    if (!json_parser_load_from_data(jp, content, (gssize)len, NULL)) {
        g_object_unref(jp);
        g_free(content);
        PCV_LOG_WARN(PROM_LOG_DOM, "checkpoint parse failed — discarding");
        return;
    }

                                                         
    JsonNode *root_n = json_parser_get_root(jp);
    if (!root_n || !JSON_NODE_HOLDS_OBJECT(root_n)) {
        PCV_LOG_WARN(PROM_LOG_DOM,
            "checkpoint root is not an object — discarding");
        g_object_unref(jp);
        g_free(content);
        return;
    }
    JsonObject *root = json_node_get_object(root_n);
    if (!json_object_has_member(root, "counters")) {
        g_object_unref(jp);
        g_free(content);
        return;
    }

    JsonArray *arr = json_object_get_array_member(root, "counters");
    guint n = json_array_get_length(arr);
    gint restored = 0;
                                          
    for (guint i = 0; i < n; i++) {
        JsonObject *o = json_array_get_object_element(arr, i);
        if (!o) continue;
        const gchar *name = json_object_get_string_member(o, "name");
        const gchar *labels = json_object_get_string_member(o, "labels");
        gdouble value = json_object_get_double_member(o, "value");
        if (!name || !labels) continue;
        gint idx = _find_or_create(name, labels, PCV_PROM_KIND_COUNTER);                          
        if (idx >= 0) {
            G.metrics[idx].value = value;
            restored++;
        }
    }

    g_object_unref(jp);
    g_free(content);
    PCV_LOG_INFO(PROM_LOG_DOM, "Counter checkpoint restored (%d entries)",
                 restored);
}

   
                        
                                                    
                                 
   
static gboolean
_checkpoint_timer_cb(gpointer data __attribute__((unused)))
{
                                                                                
    if (!g_atomic_int_get(&G.initialized)) return G_SOURCE_REMOVE;
    g_mutex_lock(&G.mu);
    _counters_save_unlocked();
    g_mutex_unlock(&G.mu);
    return G_SOURCE_CONTINUE;
}

   
                       
                                                     
                                                      
                                                   
  
                                                        
                                                             
                                                       
                                                        
                                              
  
                                                
                     
   
static gint
_sweep_stale_gauges(void)
{
    gint64 now = g_get_monotonic_time();
    gint removed = 0;
                              
    for (gint i = G.count - 1; i >= 0; i--) {
                                                                                              
                                                                   
        if (G.metrics[i].kind == PCV_PROM_KIND_COUNTER) continue;
        if (now - G.metrics[i].last_update < PCV_PROM_STALE_TTL_US) continue;
                                                              
                                                                    
                                                        
                                              
        if (!pcv_prom_labels_are_high_cardinality(G.metrics[i].labels)) continue;

                                                     
        if (G.index) {
            gchar *del_key = g_strdup_printf("%s\x01%s",
                                             G.metrics[i].name,
                                             G.metrics[i].labels);
            g_hash_table_remove(G.index, del_key);
            g_free(del_key);
        }

                                                          
                                                             
        if (i < G.count - 1) {
                                                                                     
            if (G.index) {
                gchar *moved_key = g_strdup_printf("%s\x01%s",
                                                   G.metrics[G.count - 1].name,
                                                   G.metrics[G.count - 1].labels);
                g_hash_table_insert(G.index, moved_key, GINT_TO_POINTER(i));
                                                                                    
            }
            G.metrics[i] = G.metrics[G.count - 1];
        }
        G.count--;
        removed++;
    }
    return removed;
}

   
                                  
  
                                                    
                                                     
                                                 
  
                                                         
                          
  
                                                
   
void pcv_prom_init(void)
{
    g_mutex_init(&G.mu);
    G.count = 0;
                                                                             
    g_atomic_int_set(&G.initialized, TRUE);

                                                                        
                                                                         
                                                                        
                                                                               
                                                                           
                                                                          
    G.index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

                       
    g_mutex_lock(&G.mu);
    gchar info_labels[PCV_PROM_LABELS_MAX];
    gint idx = pcv_prom_labels_build(info_labels, sizeof(info_labels), "version", PCV_PRODUCT_VERSION, NULL)
        ? _find_or_create("purecvisor_info", info_labels, PCV_PROM_KIND_GAUGE) : -1;
    if (idx >= 0) G.metrics[idx].value = 1;

                                                            
                                              
    _counters_load();
    g_mutex_unlock(&G.mu);

                                                                       
    (void)g_timeout_add_seconds(PCV_PROM_CHECKPOINT_INTERVAL_SEC, _checkpoint_timer_cb, NULL);

    PCV_LOG_INFO(PROM_LOG_DOM,
                 "Prometheus exporter initialized (counter checkpoint=%ds)",
                 PCV_PROM_CHECKPOINT_INTERVAL_SEC);
}

   
                                                                    
  
                                                             
                                                           
  
                                                                                            
                                                                                    
                                               
   
void pcv_prom_flush(void)
{
    if (!g_atomic_int_get(&G.initialized)) return;
    g_mutex_lock(&G.mu);
    _counters_save_unlocked();
    g_mutex_unlock(&G.mu);
}

   
                               
  
                                                          
  
                                                                   
                                              
  
        
                                                                        
                                                           
  
                           
                                         
                                         
  
                           
   
void pcv_prom_inc(const gchar *name, const gchar *label_key, const gchar *label_val)
{
    if (!G.initialized) return;
    gchar labels[PCV_PROM_LABELS_MAX];                                                   
    if (!pcv_prom_labels_build(labels, sizeof(labels), (label_key && *label_key) ? label_key : NULL, label_val, NULL)) return;

    g_mutex_lock(&G.mu);
    gint idx = _find_or_create(name, labels, PCV_PROM_KIND_COUNTER);
    if (idx >= 0) G.metrics[idx].value += 1.0;
    g_mutex_unlock(&G.mu);
}

   
                                            
  
                                                          
  
                                                   
  
                           
                                         
                                         
                          
  
                           
   
void pcv_prom_gauge_set(const gchar *name, const gchar *label_key,
                         const gchar *label_val, gdouble value)
{
    if (!G.initialized) return;
    gchar labels[PCV_PROM_LABELS_MAX];                                                   
    if (!pcv_prom_labels_build(labels, sizeof(labels), (label_key && *label_key) ? label_key : NULL, label_val, NULL)) return;

    g_mutex_lock(&G.mu);
    gint idx = _find_or_create(name, labels, PCV_PROM_KIND_GAUGE);
    if (idx >= 0) G.metrics[idx].value = value;
    g_mutex_unlock(&G.mu);
}

   
                                             
  
                                                           
                                                   
  
                                                       
                                                     
  
        
                                                                                             
  
                        
                                                                         
                       
  
                                                                 
                                                            
  
                           
   
void pcv_prom_gauge_set_labels(const gchar *name, const gchar *labels,
                                gdouble value)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.mu);
    gint idx = _find_or_create(name, labels ? labels : "", PCV_PROM_KIND_GAUGE);
    if (idx >= 0) G.metrics[idx].value = value;
    g_mutex_unlock(&G.mu);
}

                                                  
                                     
void pcv_prom_inc_labels(const gchar *name, const gchar *labels)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.mu);
    gint idx = _find_or_create(name, labels ? labels : "", PCV_PROM_KIND_COUNTER);
    if (idx >= 0) G.metrics[idx].value += 1.0;
    g_mutex_unlock(&G.mu);
}

                                                          
                                                                               
void pcv_prom_source_counter_set_labels(const gchar *name, const gchar *labels, gdouble value)
{
    if (!G.initialized) return;
    g_mutex_lock(&G.mu);
    gint idx = _find_or_create(name, labels ? labels : "", PCV_PROM_KIND_SOURCE_COUNTER);
    if (idx >= 0) G.metrics[idx].value = value;
    g_mutex_unlock(&G.mu);
}

   
                                                     
  
                                                              
  
                                     
                                                 
                           
  
                                         
   
void pcv_prom_rpc_start(const gchar *method)
{
    if (!G.initialized || !method) return;
                                       
}

   
                                                 
  
                                                       
                      
  
                                     
                      
                                                                  
                                                              
  
                                             
                                                                
                                    
  
                           
   
void pcv_prom_rpc_end(const gchar *method, gboolean success, gdouble duration_ms)
{
    if (!G.initialized || !method) return;

                                                                                              
    gchar labels[PCV_PROM_LABELS_MAX], dur_labels[PCV_PROM_LABELS_MAX];
    if (!pcv_prom_labels_build(labels, sizeof(labels), "method", method, "status", success ? "ok" : "error", NULL) ||
        !pcv_prom_labels_build(dur_labels, sizeof(dur_labels), "method", method, NULL)) return;

    g_mutex_lock(&G.mu);
    gint idx = _find_or_create("purecvisor_rpc_requests_total", labels, PCV_PROM_KIND_COUNTER);
    if (idx >= 0) G.metrics[idx].value += 1.0;

                                        
    gint didx = _find_or_create("purecvisor_rpc_duration_ms", dur_labels, PCV_PROM_KIND_GAUGE);
    if (didx >= 0) G.metrics[didx].value = duration_ms;
    g_mutex_unlock(&G.mu);
}

   
                                                              
  
                                                    
                                                          
                                
  
                                                                    
                                                     
   
void pcv_prom_zfs_inflight_lock_observe(const gchar *pool_name,
                                        const gchar *op,
                                        const gchar *result,
                                        gdouble wait_ms)
{
    (void)pool_name;                                    
    if (!G.initialized || !result) return;
    const gchar *safe_op = (op && *op) ? op : "unknown";
    const gchar *safe_result = *result ? result : "unknown";
    if (wait_ms < 0.0) wait_ms = 0.0;

    gchar labels[PCV_PROM_LABELS_MAX];                                       
    if (!pcv_prom_labels_build(labels, sizeof(labels), "op", safe_op, "result", safe_result, NULL)) return;

    g_mutex_lock(&G.mu);
    gint idx = _find_or_create(
        "purecvisor_zfs_inflight_lock_acquired_total", labels, PCV_PROM_KIND_COUNTER);
    if (idx >= 0) G.metrics[idx].value += 1.0;

                                                           
                                                            
                                             
                                                              
                                                      
    static const gdouble buckets[] = {10.0, 50.0, 100.0, 500.0, 1000.0, 5000.0};
    for (guint i = 0; i < G_N_ELEMENTS(buckets); i++) {
        gchar bucket_labels[PCV_PROM_LABELS_MAX], le[16];
        g_snprintf(le, sizeof(le), "%.0f", buckets[i]);
        gint bidx = pcv_prom_labels_build(bucket_labels, sizeof(bucket_labels), "op", safe_op, "result", safe_result,
                                          "le", le, NULL)
            ? _find_or_create("purecvisor_zfs_inflight_lock_wait_ms_bucket", bucket_labels, PCV_PROM_KIND_COUNTER) : -1;
        if (bidx >= 0 && wait_ms <= buckets[i]) G.metrics[bidx].value += 1.0;
    }

                                                                
                                                             
    gchar inf_labels[PCV_PROM_LABELS_MAX];
    gint inf_idx = pcv_prom_labels_build(inf_labels, sizeof(inf_labels), "op", safe_op, "result", safe_result,
                                         "le", "+Inf", NULL)
        ? _find_or_create("purecvisor_zfs_inflight_lock_wait_ms_bucket", inf_labels, PCV_PROM_KIND_COUNTER) : -1;
    if (inf_idx >= 0) G.metrics[inf_idx].value += 1.0;

    gint sum_idx = _find_or_create(
        "purecvisor_zfs_inflight_lock_wait_ms_sum", labels, PCV_PROM_KIND_COUNTER);
    if (sum_idx >= 0) G.metrics[sum_idx].value += wait_ms;
    gint count_idx = _find_or_create(
        "purecvisor_zfs_inflight_lock_wait_ms_count", labels, PCV_PROM_KIND_COUNTER);
    if (count_idx >= 0) G.metrics[count_idx].value += 1.0;
    g_mutex_unlock(&G.mu);
}

   
                                                            
  
                                                  
                                                          
                                         
  
                                              
                                 
  
                    
                                           
                                
  
                                            
                                                                  
  
                                                   
                                                      
  
                           
   
gchar *pcv_prom_render(void)
{
    if (!G.initialized)
        return g_strdup("# purecvisor prometheus exporter not initialized\n");

    GString *buf = g_string_new("");

    g_mutex_lock(&G.mu);
                                                    
    gint sweeped = _sweep_stale_gauges();
    if (sweeped > 0) {
        PCV_LOG_INFO("prom_exporter",
            "Swept %d stale gauge metrics (VM labels, TTL=5min)", sweeped);
    }

                                                                                       
                                                                             
                                                                         
                                                                                
                                                                                    
                                                                                  
    GHashTable *by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_array_unref);
    GPtrArray *order = g_ptr_array_new();                                
    for (gint i = 0; i < G.count; i++) {
        GArray *slots = g_hash_table_lookup(by_name, G.metrics[i].name);
        if (!slots) {
            gchar *key = g_strdup(G.metrics[i].name);
            slots = g_array_new(FALSE, FALSE, sizeof(gint));
            g_hash_table_insert(by_name, key, slots);
            g_ptr_array_add(order, key);
        }
        g_array_append_val(slots, i);
    }
    for (guint n = 0; n < order->len; n++) {
        GArray *slots = g_hash_table_lookup(by_name, g_ptr_array_index(order, n));
        const PromMetric *first = &G.metrics[g_array_index(slots, gint, 0)];
        g_string_append_printf(buf, "# TYPE %s %s\n", first->name, _kind_type(first->kind));
        for (guint k = 0; k < slots->len; k++) {
            gint i = g_array_index(slots, gint, k);
            PromMetric *m = &G.metrics[i];
            if (m->kind != first->kind) {
                PCV_LOG_WARN(PROM_LOG_DOM,
                    "Metric type mismatch: %s (slot %d=%s, slot %d=%s) — using first",
                    m->name, g_array_index(slots, gint, 0), _kind_name(first->kind), i, _kind_name(m->kind));
            }
                                                                
                                                                      
            if (m->labels[0] && m->labels[0] != '=')
                g_string_append_printf(buf, "%s{%s} %.17g\n", m->name, m->labels, m->value);
            else
                g_string_append_printf(buf, "%s %.17g\n", m->name, m->value);
        }
    }
    g_ptr_array_unref(order);
    g_hash_table_destroy(by_name);
    g_mutex_unlock(&G.mu);

    return g_string_free(buf, FALSE);
}
