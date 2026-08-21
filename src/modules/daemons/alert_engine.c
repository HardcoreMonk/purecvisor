   
                       
                                                      
  
                           
                                                   
                                                    
                                        
  
                                                 
                                                 
                                                      
                                               
                                                    
                                                      
  
                                                                               
         
                                                                               
                                              
                                                   
                                                 
                                                    
  
                                                                               
           
                                                                               
                                                
                                                      
                                                    
  
                                                       
                                                      
                                            
                                                  
                                                
                                               
                                                  
                                                       
                                                       
                                                             
  
                                                                               
           
                                                                               
                                               
                                                                    
                                                       
                                                                     
  
                                                                               
                                     
                                                                               
                                           
  
             
                                     
                                                     
                                                                      
                                                              
  
                                    
                                                   
                                          
                                                                      
                                                    
  
             
                                                    
                                     
                                                      
  
                                                                               
                       
                                                                               
                                     
                                      
                                                                                             
                                        
  
                                           
                                        
                                                                                       
                                           
                                                                            
  
                                              
                                                   
                                                                                                                
                                       
  
                                                                               
                 
                                                                               
                                                    
                                                   
                                        
                       
                                     
                                                     
   
#include "alert_engine.h"
#include "alert_silence.h"
#include "alert_dlq.h"
#include "ebpf_telemetry.h"
#include "utils/pcv_config.h"
#include "utils/pcv_log.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_secure.h"                                               
#include "utils/pcv_ssrf.h"                              
#include "pcv_webpush.h"                                       
#if PCV_CLUSTER_ENABLED
#include "../cluster/cluster_manager.h"
#endif
#include <errno.h>
#include <libsoup/soup.h>
#include <string.h>
#include <time.h>
#include <sys/statvfs.h>

                                                               

                                              
  
                                                                               
                               
                                                                               
  
                                          
                                           
                                  
                                  
                                             
                                      
                                            
                                      
  
                                              
                                        
                                            
                                                
  
                    
                                                   
                                              
                                             
  
             
                                                 
                                  
  
                    
                                              
                                           
  
                                 
                                                           
                                                       
                                                         
                                         
                                                                               
   

#define ALERT_LOG_DOM      "alert_engine"

                                         
                                             
constexpr int ALERT_CHECK_SEC = 5;

                                                  
                                                          
constexpr int ALERT_HISTORY_MAX = 1000;

                                                      
                                                        
constexpr int ALERT_DEDUP_WINDOW_SEC = 300;

                     
constexpr int MAX_COMPOSITE_RULES = 8;

                                                               

   
                   
                                       
  
                                                              
  
                                                    
                                                 
   
typedef enum {
    ALERT_NONE = 0,                         
    ALERT_WARN,                                             
    ALERT_CRIT                                                 
} AlertLevel;

   
                      
                            
  
                                                  
                   
  
                                          
                                                   
   
typedef struct {
    gchar      metric[16];                                                 
    AlertLevel level;                                                  
    gdouble    value;                                              
    gint64     fired_at;                                                     
    gchar      message[256];                             
                                                                                                      
    gint64     alert_id;                                       
    gboolean   acknowledged;                                    
    gboolean   escalated;                                  
} AlertRecord;

   
                      
                                             
  
                                                
                                                      
  
                                                       
                                               
                                            
  
                                    
                                                    
                                            
                                                         
                                                                            
                                                
                                                                 
   
typedef struct {
    gdouble    warn_thresh;                                                      
    gdouble    crit_thresh;                                                        
    gint64     warn_since;                               
                                                                 
                                                                          
    gint64     crit_since;                                                      
    gboolean   warn_fired;                                        
                                                                      
    gboolean   crit_fired;                                           
    gint64     last_warn_fired_at;                                                 
    gint64     last_crit_fired_at;                                                 
} MetricWatch;

   
                    
                                      
   
typedef enum {
    COMPOSITE_OP_AND = 0,                         
    COMPOSITE_OP_OR  = 1                       
} CompositeOp;

   
                        
                                      
  
                                                   
  
                                                    
                                                    
   
typedef struct {
    gboolean    active;                          
    CompositeOp op;                              
    gchar       metric_a[16];                                             
    gdouble     thresh_a;                           
    gchar       metric_b[16];                   
    gdouble     thresh_b;                           
    AlertLevel  level;                                       
    gint64      since;                                   
    gboolean    fired;                                 
    gint64      last_fired_at;                            
} CompositeRule;

typedef struct {
    gint64 since;
    gboolean fired;
    gint64 last_fired_at;
} CompositeRuntime;

   
                                                          
  
                                                
                                                          
   
typedef struct {
    gboolean enabled;
    gint cpu_warn;
    gint cpu_crit;
    gint mem_warn;
    gint mem_crit;
    gint disk_warn;
    gint disk_crit;
    gint data_pool_warn;
    gint data_pool_crit;
    gint eval_period_sec;
    gint dedup_window_sec;
    gchar webhook_url[512];
    gchar webhook_secret[128];
    gchar webhook_crit_url[512];
    gchar webhook_format[16];
    gchar telegram_chat_id[64];
    CompositeRule composite_rules[MAX_COMPOSITE_RULES];
    gint n_composite_rules;
    gint64 config_revision;
    gboolean daemon_config_valid;
    gchar daemon_config_error[128];
} AlertConfigSnapshot;

extern void pcv_test_alert_snapshot_wipe_hook(
    const gchar *secret, gsize len) __attribute__((weak));

   
                                                      
  
                                                  
                                               
  
                                                       
                                                   
                                                                         
                                                     
                                 
  
                                                     
                                          
                                                  
                                                
                                              
                                                                        
                                                               
   
static void
_config_snapshot_clear(AlertConfigSnapshot *snapshot)
{
    if (!snapshot)
        return;
    pcv_secure_wipe(snapshot->webhook_secret,
                    sizeof(snapshot->webhook_secret));
    if (pcv_test_alert_snapshot_wipe_hook) {
        pcv_test_alert_snapshot_wipe_hook(
            snapshot->webhook_secret, sizeof(snapshot->webhook_secret));
    }
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(AlertConfigSnapshot,
                                 _config_snapshot_clear)

   
                                
  
                                                 
                                  
  
                                   
                                                   
  
           
                                                          
                                                              
                                                                               
                                                                         
   
static struct {
    GThread       *thread;                                       
                                                             
    gboolean       running;                           
                                                                        
    gboolean       enabled;                          
                                                                           
                                                                  
    gboolean       initialized;                                  
                                                                  

                                                 
    gchar          webhook_url[512];                                            
                                                                                                
    gchar          webhook_secret[128];                                                
    gchar          webhook_crit_url[512];                                                   
    gchar          webhook_format[16];                                                        
                                                             
    gchar          telegram_chat_id[64];                              
                                                                                  
    gint           eval_period_sec;                                  
                                                                            
                                                                  
    gint           dedup_window_sec;                                    
                                                                         
                                                                             

                          
    MetricWatch    cpu;                                              
    MetricWatch    mem;                       
    MetricWatch    disk;                                        
    MetricWatch    data_pool;                                              

                                 
    CompositeRule  composite_rules[MAX_COMPOSITE_RULES];                 
    CompositeRuntime composite_runtime[MAX_COMPOSITE_RULES];
    gint           n_composite_rules;                                              

                        
    AlertRecord    history[ALERT_HISTORY_MAX];                              
                                                                                      
    gint           hist_count;                                               
                                                                
    gint           hist_idx;                                            
                                                                
    gint64         config_revision;                                        
    gboolean       daemon_config_valid;                                 
    gchar          daemon_config_error[128];                              
    GRWLock        config_lock;                                                            
    GMutex         state_mu;                                                 
    GMutex         lifecycle_mu;                                        
    GCond          lifecycle_cond;                                
    GMutex         reload_mu;                                                    
    gint           worker_start_count;                                              
    GMutex         mu;                                   
} G = {0};

static gsize g_alert_locks_initialized = 0;

                                                             

static void
_ensure_alert_locks_initialized(void)
{
    if (g_once_init_enter(&g_alert_locks_initialized)) {
        g_rw_lock_init(&G.config_lock);
        g_mutex_init(&G.state_mu);
        g_mutex_init(&G.lifecycle_mu);
        g_cond_init(&G.lifecycle_cond);
        g_mutex_init(&G.reload_mu);
        g_mutex_init(&G.mu);
        g_once_init_leave(&g_alert_locks_initialized, 1);
    }
}

                                                        
                                                          
                                                                       
                          
static gboolean
_engine_is_initialized(void)
{
    gboolean initialized;
    _ensure_alert_locks_initialized();
    g_mutex_lock(&G.lifecycle_mu);
    initialized = G.initialized;
    g_mutex_unlock(&G.lifecycle_mu);
    return initialized;
}

   
                                                          
  
                                               
                                             
  
                                                    
                                              
                                          
  
                                                         
                                            
                                         
                                                        
                                             
   
static void
_config_snapshot_locked(AlertConfigSnapshot *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = G.enabled;
    out->cpu_warn = (gint)G.cpu.warn_thresh;
    out->cpu_crit = (gint)G.cpu.crit_thresh;
    out->mem_warn = (gint)G.mem.warn_thresh;
    out->mem_crit = (gint)G.mem.crit_thresh;
    out->disk_warn = (gint)G.disk.warn_thresh;
    out->disk_crit = (gint)G.disk.crit_thresh;
    out->data_pool_warn = (gint)G.data_pool.warn_thresh;
    out->data_pool_crit = (gint)G.data_pool.crit_thresh;
    out->eval_period_sec = G.eval_period_sec;
    out->dedup_window_sec = G.dedup_window_sec;
    g_strlcpy(out->webhook_url, G.webhook_url, sizeof(out->webhook_url));
    g_strlcpy(out->webhook_secret, G.webhook_secret,
              sizeof(out->webhook_secret));
    g_strlcpy(out->webhook_crit_url, G.webhook_crit_url,
              sizeof(out->webhook_crit_url));
    g_strlcpy(out->webhook_format, G.webhook_format,
              sizeof(out->webhook_format));
    g_strlcpy(out->telegram_chat_id, G.telegram_chat_id,
              sizeof(out->telegram_chat_id));
    memcpy(out->composite_rules, G.composite_rules,
           sizeof(out->composite_rules));
    out->n_composite_rules = G.n_composite_rules;
    out->config_revision = G.config_revision;
    out->daemon_config_valid = G.daemon_config_valid;
    g_strlcpy(out->daemon_config_error, G.daemon_config_error,
              sizeof(out->daemon_config_error));
}

   
                                                                  
  
                                                   
                                                 
  
                                                              
                                                          
                                                     
                                                                 
                                  
                     
   
static void
_config_snapshot(AlertConfigSnapshot *out)
{
    _ensure_alert_locks_initialized();
    g_rw_lock_reader_lock(&G.config_lock);
    _config_snapshot_locked(out);
    g_rw_lock_reader_unlock(&G.config_lock);
}

   
                                                    
  
                                                 
                                                 
  
                                                                        
                                                  
                       
  
                                            
                                                                     
                                                           
                        
                                                    
                                          
                                                      
                                                               
                                                       
   
static void
_config_commit_locked(const AlertConfigSnapshot *candidate)
{
    G.enabled = candidate->enabled;
    G.cpu.warn_thresh = candidate->cpu_warn;
    G.cpu.crit_thresh = candidate->cpu_crit;
    G.mem.warn_thresh = candidate->mem_warn;
    G.mem.crit_thresh = candidate->mem_crit;
    G.disk.warn_thresh = candidate->disk_warn;
    G.disk.crit_thresh = candidate->disk_crit;
    G.data_pool.warn_thresh = candidate->data_pool_warn;
    G.data_pool.crit_thresh = candidate->data_pool_crit;
    G.eval_period_sec = candidate->eval_period_sec;
    G.dedup_window_sec = candidate->dedup_window_sec;
    g_strlcpy(G.webhook_url, candidate->webhook_url, sizeof(G.webhook_url));
    pcv_secure_wipe(G.webhook_secret, sizeof(G.webhook_secret));
    g_strlcpy(G.webhook_secret, candidate->webhook_secret,
              sizeof(G.webhook_secret));
    g_strlcpy(G.webhook_crit_url, candidate->webhook_crit_url,
              sizeof(G.webhook_crit_url));
    g_strlcpy(G.webhook_format, candidate->webhook_format,
              sizeof(G.webhook_format));
    g_strlcpy(G.telegram_chat_id, candidate->telegram_chat_id,
              sizeof(G.telegram_chat_id));
    memcpy(G.composite_rules, candidate->composite_rules,
           sizeof(G.composite_rules));
    G.n_composite_rules = candidate->n_composite_rules;
}

                                                            
                                                  
                                                                  
                                                  
                                                     

                                                         
static gboolean
_json_node_is_integer(JsonNode *node)
{
    return node && JSON_NODE_HOLDS_VALUE(node)
        && json_node_get_value_type(node) == G_TYPE_INT64;
}

                                                     
static gboolean
_json_node_is_boolean(JsonNode *node)
{
    return node && JSON_NODE_HOLDS_VALUE(node)
        && json_node_get_value_type(node) == G_TYPE_BOOLEAN;
}

                                              
static gboolean
_json_node_is_string(JsonNode *node)
{
    return node && JSON_NODE_HOLDS_VALUE(node)
        && json_node_get_value_type(node) == G_TYPE_STRING;
}

                                                  
                                 
static gboolean
_json_node_is_number(JsonNode *node)
{
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) return FALSE;
    GType type = json_node_get_value_type(node);
    return type == G_TYPE_INT64 || type == G_TYPE_DOUBLE;
}

   
                                                        
  
                                                
                                                 
                                         
  
                                                     
                                                
  
                                                  
                                                    
                                                                  
                                              
   
static gboolean
_copy_checked_string(JsonNode *node, gchar *dst, gsize dst_size)
{
    if (!_json_node_is_string(node)) return FALSE;
    const gchar *value = json_node_get_string(node);
    if (!value || strlen(value) >= dst_size) return FALSE;
    g_strlcpy(dst, value, dst_size);
    return TRUE;
}

   
                                                                
  
                                             
                                             
  
                                                                
                                               
                    
  
                                                  
                                             
                                               
                                                           
                               
                                                                 
   
static gboolean
_webhook_url_valid(const gchar *url)
{
    if (!url || !*url) return TRUE;
    GError *error = NULL;
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, &error);
    if (!uri) {
        g_clear_error(&error);
        return FALSE;
    }
    const gchar *scheme = g_uri_get_scheme(uri);
    const gchar *host = g_uri_get_host(uri);
    gboolean valid = host && *host
        && (g_ascii_strcasecmp(scheme, "http") == 0
            || g_ascii_strcasecmp(scheme, "https") == 0);
    g_uri_unref(uri);
    return valid;
}

   
                                                                     
  
                                                           
                                              
  
                                                       
                                              
                                                
  
                                                    
                                                                           
   
static void
_webhook_url_canonicalize_scheme(gchar *url)
{
    if (!url || !*url)
        return;
    if (g_ascii_strncasecmp(url, "https://", 8) == 0) {
        memcpy(url, "https://", 8);
    } else if (g_ascii_strncasecmp(url, "http://", 7) == 0) {
        memcpy(url, "http://", 7);
    }
}

                                                        
                                                 
                                  
                                                   
static gboolean
_composite_metric_valid(const gchar *metric)
{
    return g_strcmp0(metric, "CPU") == 0
        || g_strcmp0(metric, "Memory") == 0
        || g_strcmp0(metric, "Disk") == 0;
}

   
                                                                     
  
                                                   
                                              
                                       
  
             
                                                        
                                          
                                                 
                                    
                                                          
                                                            
                                
  
                                                                     
                                   
                                           
                                                    
                                                        
                                 
                            
                                                      
                                                                             
                                     
   
static gboolean
_parse_composite_rules(JsonNode *node, AlertConfigSnapshot *candidate)
{
    if (!node || !JSON_NODE_HOLDS_ARRAY(node)) return FALSE;
    JsonArray *array = json_node_get_array(node);
    guint length = json_array_get_length(array);
    if (length > MAX_COMPOSITE_RULES) return FALSE;

    CompositeRule parsed[MAX_COMPOSITE_RULES] = {0};
    for (guint i = 0; i < length; i++) {
        JsonNode *element_node = json_array_get_element(array, i);
        if (!element_node || !JSON_NODE_HOLDS_OBJECT(element_node)) return FALSE;
        JsonObject *element = json_node_get_object(element_node);
        const gchar *required[] = {
            "metric_a", "thresh_a", "op", "metric_b", "thresh_b", "level"
        };
        for (guint r = 0; r < G_N_ELEMENTS(required); r++) {
            if (!json_object_has_member(element, required[r])) return FALSE;
        }

        GList *members = json_object_get_members(element);
        for (GList *it = members; it; it = it->next) {
            const gchar *key = it->data;
            if (g_strcmp0(key, "active") != 0
                && g_strcmp0(key, "metric_a") != 0
                && g_strcmp0(key, "thresh_a") != 0
                && g_strcmp0(key, "op") != 0
                && g_strcmp0(key, "metric_b") != 0
                && g_strcmp0(key, "thresh_b") != 0
                && g_strcmp0(key, "level") != 0) {
                g_list_free(members);
                return FALSE;
            }
        }
        g_list_free(members);

        CompositeRule *rule = &parsed[i];
        rule->active = TRUE;
        if (json_object_has_member(element, "active")) {
            JsonNode *active = json_object_get_member(element, "active");
            if (!_json_node_is_boolean(active)) return FALSE;
            rule->active = json_node_get_boolean(active);
        }

        JsonNode *metric_a = json_object_get_member(element, "metric_a");
        JsonNode *metric_b = json_object_get_member(element, "metric_b");
        JsonNode *threshold_a = json_object_get_member(element, "thresh_a");
        JsonNode *threshold_b = json_object_get_member(element, "thresh_b");
        JsonNode *op = json_object_get_member(element, "op");
        JsonNode *level = json_object_get_member(element, "level");
        if (!_copy_checked_string(metric_a, rule->metric_a,
                                  sizeof(rule->metric_a))
            || !_copy_checked_string(metric_b, rule->metric_b,
                                     sizeof(rule->metric_b))
            || !_json_node_is_number(threshold_a)
            || !_json_node_is_number(threshold_b)
            || !_copy_checked_string(op, (gchar[8]){0}, 8)
            || !_copy_checked_string(level, (gchar[8]){0}, 8)) {
            return FALSE;
        }
        rule->thresh_a = json_node_get_double(threshold_a);
        rule->thresh_b = json_node_get_double(threshold_b);
        if (rule->thresh_a < 0.0 || rule->thresh_a > 100.0
            || rule->thresh_b < 0.0 || rule->thresh_b > 100.0
            || !_composite_metric_valid(rule->metric_a)
            || !_composite_metric_valid(rule->metric_b)) {
            return FALSE;
        }
        const gchar *op_value = json_node_get_string(op);
        const gchar *level_value = json_node_get_string(level);
        if (g_strcmp0(op_value, "AND") == 0) {
            rule->op = COMPOSITE_OP_AND;
        } else if (g_strcmp0(op_value, "OR") == 0) {
            rule->op = COMPOSITE_OP_OR;
        } else {
            return FALSE;
        }
        if (g_strcmp0(level_value, "WARN") == 0) {
            rule->level = ALERT_WARN;
        } else if (g_strcmp0(level_value, "CRIT") == 0) {
            rule->level = ALERT_CRIT;
        } else {
            return FALSE;
        }
    }
    memset(candidate->composite_rules, 0, sizeof(candidate->composite_rules));
    memcpy(candidate->composite_rules, parsed, sizeof(parsed));
    candidate->n_composite_rules = (gint)length;
    return TRUE;
}

   
                                                             
  
                                               
                                                 
                         
  
                                                      
                                                                 
                                      
  
         
                                                 
                                            
                                                            
                             
                                                         
                 
                                                          
                                                           
                               
  
                                                         
                                                       
                                                       
                                            
                                                      
                                                             
                                               
                                                      
                                        
                            
   
static gboolean
_overlay_runtime_config(AlertConfigSnapshot *candidate, JsonObject *cfg)
{
    GList *members = json_object_get_members(cfg);
    for (GList *it = members; it; it = it->next) {
        const gchar *key = it->data;
        JsonNode *node = json_object_get_member(cfg, key);
        gint *integer_target = NULL;
        gchar *string_target = NULL;
        gsize string_size = 0;

        if (g_strcmp0(key, "enabled") == 0) {
            if (!_json_node_is_boolean(node)) goto invalid;
            candidate->enabled = json_node_get_boolean(node);
            continue;
        } else if (g_strcmp0(key, "cpu_warn") == 0) {
            integer_target = &candidate->cpu_warn;
        } else if (g_strcmp0(key, "cpu_crit") == 0) {
            integer_target = &candidate->cpu_crit;
        } else if (g_strcmp0(key, "mem_warn") == 0) {
            integer_target = &candidate->mem_warn;
        } else if (g_strcmp0(key, "mem_crit") == 0) {
            integer_target = &candidate->mem_crit;
        } else if (g_strcmp0(key, "disk_warn") == 0) {
            integer_target = &candidate->disk_warn;
        } else if (g_strcmp0(key, "disk_crit") == 0) {
            integer_target = &candidate->disk_crit;
        } else if (g_strcmp0(key, "data_pool_warn") == 0) {
            integer_target = &candidate->data_pool_warn;
        } else if (g_strcmp0(key, "data_pool_crit") == 0) {
            integer_target = &candidate->data_pool_crit;
        } else if (g_strcmp0(key, "eval_period") == 0) {
            integer_target = &candidate->eval_period_sec;
        } else if (g_strcmp0(key, "dedup_window") == 0) {
            integer_target = &candidate->dedup_window_sec;
        } else if (g_strcmp0(key, "webhook_url") == 0) {
            string_target = candidate->webhook_url;
            string_size = sizeof(candidate->webhook_url);
        } else if (g_strcmp0(key, "webhook_secret") == 0) {
            string_target = candidate->webhook_secret;
            string_size = sizeof(candidate->webhook_secret);
        } else if (g_strcmp0(key, "webhook_crit_url") == 0) {
            string_target = candidate->webhook_crit_url;
            string_size = sizeof(candidate->webhook_crit_url);
        } else if (g_strcmp0(key, "webhook_format") == 0) {
            string_target = candidate->webhook_format;
            string_size = sizeof(candidate->webhook_format);
        } else if (g_strcmp0(key, "telegram_chat_id") == 0) {
            string_target = candidate->telegram_chat_id;
            string_size = sizeof(candidate->telegram_chat_id);
        } else if (g_strcmp0(key, "composite_rules") == 0) {
            if (!_parse_composite_rules(node, candidate)) goto invalid;
            continue;
        } else {
                                                 
            goto invalid;
        }

        if (integer_target) {
            if (!_json_node_is_integer(node)) goto invalid;
            gint64 value = json_node_get_int(node);
            if (value < G_MININT || value > G_MAXINT) goto invalid;
            *integer_target = (gint)value;
        } else if (string_target
                   && !_copy_checked_string(node, string_target, string_size)) {
            goto invalid;
        }
    }
    g_list_free(members);

    if (candidate->cpu_warn < 0 || candidate->cpu_warn > 100
        || candidate->cpu_crit < 0 || candidate->cpu_crit > 100
        || candidate->mem_warn < 0 || candidate->mem_warn > 100
        || candidate->mem_crit < 0 || candidate->mem_crit > 100
        || candidate->disk_warn < 0 || candidate->disk_warn > 100
        || candidate->disk_crit < 0 || candidate->disk_crit > 100
        || candidate->data_pool_warn < 0 || candidate->data_pool_warn > 100
        || candidate->data_pool_crit < 0 || candidate->data_pool_crit > 100
        || candidate->cpu_warn >= candidate->cpu_crit
        || candidate->mem_warn >= candidate->mem_crit
        || candidate->disk_warn >= candidate->disk_crit
        || candidate->data_pool_warn >= candidate->data_pool_crit
        || candidate->eval_period_sec < 5
        || candidate->eval_period_sec > 600
        || candidate->dedup_window_sec < 0
        || (g_strcmp0(candidate->webhook_format, "slack") != 0
            && g_strcmp0(candidate->webhook_format, "telegram") != 0
            && g_strcmp0(candidate->webhook_format, "generic") != 0)
        || !_webhook_url_valid(candidate->webhook_url)
        || !_webhook_url_valid(candidate->webhook_crit_url)) {
        return FALSE;
    }
      
                                                           
                                                      
       
    _webhook_url_canonicalize_scheme(candidate->webhook_url);
    _webhook_url_canonicalize_scheme(candidate->webhook_crit_url);
    return TRUE;

invalid:
    g_list_free(members);
    return FALSE;
}

   
                                                          
  
                                              
                                                   
                                            
  
                                                             
                                                    
                         
  
                                                  
                           
                                                              
                                    
   
static void
_safe_config_defaults(AlertConfigSnapshot *config)
{
    memset(config, 0, sizeof(*config));
    config->enabled = FALSE;
    config->cpu_warn = 80;
    config->cpu_crit = 95;
    config->mem_warn = 85;
    config->mem_crit = 95;
    config->disk_warn = 80;
    config->disk_crit = 90;
    config->data_pool_warn = 80;
    config->data_pool_crit = 90;
    config->eval_period_sec = 30;
    config->dedup_window_sec = ALERT_DEDUP_WINDOW_SEC;
    g_strlcpy(config->webhook_format, "generic",
              sizeof(config->webhook_format));
    config->daemon_config_valid = TRUE;
}

   
                                                                  
  
                                                    
                                        
  
                                                            
                                                 
                                           
  
                                                                    
                                                                      
                                                           
                             
                                                               
   
static gboolean
_daemon_config_is_complete(JsonObject *cfg,
                           gboolean secret_supplied_externally)
{
    static const gchar *required[] = {
        "enabled",
        "cpu_warn", "cpu_crit",
        "mem_warn", "mem_crit",
        "disk_warn", "disk_crit",
        "data_pool_warn", "data_pool_crit",
        "eval_period", "dedup_window",
        "webhook_url", "webhook_crit_url",
        "webhook_format", "telegram_chat_id"
    };
    if (!cfg) return FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(required); i++) {
        if (!json_object_has_member(cfg, required[i])) return FALSE;
    }
    return secret_supplied_externally
        || json_object_has_member(cfg, "webhook_secret");
}

static PcvAlertConfigSetResult
_apply_daemon_config(JsonObject *cfg,
                     PcvAlertConfigSourceMode mode,
                     const gchar *source_secret,
                     gboolean secret_supplied_externally);
static gboolean
_build_daemon_config_candidate(JsonObject *cfg,
                               const gchar *source_secret,
                               gboolean secret_supplied_externally,
                               AlertConfigSnapshot *candidate);

   
                                                                  
  
                                                 
                                                     
  
                                                             
                                                         
                                               
          
  
                                                                
                                                            
                                                                  
                                                  
                                                     
   
static void
_reset_metric_watch_state_locked(void)
{
    MetricWatch *watches[] = {&G.cpu, &G.mem, &G.disk, &G.data_pool};
    for (guint i = 0; i < G_N_ELEMENTS(watches); i++) {
        watches[i]->warn_since = watches[i]->crit_since = 0;
        watches[i]->warn_fired = watches[i]->crit_fired = FALSE;
        watches[i]->last_warn_fired_at = watches[i]->last_crit_fired_at = 0;
    }
    memset(G.composite_runtime, 0, sizeof(G.composite_runtime));
}

   
                                     
  
                                               
                                                     
              
  
                                                                  
                                                
                            
  
                                                          
  
                                                    
   
static gint64
_mono_now(void)
{
    return g_get_monotonic_time() / G_USEC_PER_SEC;
}

   
                                
  
                                                   
                             
  
                                                  
                                               
  
                                                      
                                                   
                                    
                                       
  
                                                   
                                                       
   
                                   
static volatile gint g_next_alert_id = 1;

                                                            
                                                    
                                        
extern void pcv_test_alert_record_security_event_hook(
    const gchar *event_id,
    const gchar *severity,
    const gchar *summary) __attribute__((weak));
extern void pcv_test_alert_config_commit_hook(void) __attribute__((weak));

static void
_record_alert(const gchar *metric, AlertLevel level, gdouble value, const gchar *msg)
{
    g_mutex_lock(&G.mu);
    AlertRecord *r = &G.history[G.hist_idx];
    g_strlcpy(r->metric, metric, sizeof(r->metric));
    r->level = level;
    r->value = value;
    r->fired_at = (gint64)time(NULL);
    g_strlcpy(r->message, msg, sizeof(r->message));
    r->alert_id = (gint64)g_atomic_int_add(&g_next_alert_id, 1);                                       
    r->acknowledged = FALSE;                                         
    r->escalated = FALSE;                         
    G.hist_idx = (G.hist_idx + 1) % ALERT_HISTORY_MAX;                                      
    if (G.hist_count < ALERT_HISTORY_MAX) G.hist_count++;                                     
    g_mutex_unlock(&G.mu);
}

   
                                                                 
  
                                                 
                                               
  
                                                           
                                          
                                                 
                                                        
                                                   
                                                 
                                       
   
void
pcv_alert_record_security_event(const gchar *event_id,
                                const gchar *severity,
                                const gchar *summary)
{
    if (pcv_test_alert_record_security_event_hook) {
        pcv_test_alert_record_security_event_hook(event_id, severity, summary);
    }
    if (!_engine_is_initialized()) {
        return;                                           
    }

    AlertLevel level = g_strcmp0(severity, "crit") == 0
        ? ALERT_CRIT
        : ALERT_WARN;
    gchar msg[256];
    g_snprintf(msg, sizeof msg, "[%s] Security event %s: %s",
               severity ? severity : "warn",
               event_id ? event_id : "",
               summary ? summary : "");
    _record_alert("Security", level, 0.0, msg);
}

   
                                                 
  
                                      
                                              
                         
  
                                                             
                                             
                                         
  
                                                    
  
                                                   
   
                                                                    
                                                          
                                                                  
                                                        
                                                              

constexpr int WEBHOOK_MAX_RETRIES = 3;

                                                          
static_assert(ALERT_HISTORY_MAX >= 100, "History buffer too small");
static_assert(MAX_COMPOSITE_RULES <= 16, "Composite rules exceed limit");
static_assert(WEBHOOK_MAX_RETRIES >= 1, "Must retry at least once");

   
                                          
  
                                                
                                                  
  
                         
                            
                                              
                                                            
   
static gboolean
_webhook_post_once(const gchar *url, const gchar *payload, const gchar *secret)
{
    if (!url || !url[0]) return FALSE;

                                                                
    if (!_webhook_url_valid(url)) {
        PCV_LOG_WARN(ALERT_LOG_DOM,
                     "Webhook URL rejected (invalid scheme): %.100s", url);
        return FALSE;
    }

                                                                  
                                                          
                                                      
    GError *ssrf_err = NULL;
    if (!pcv_url_target_allowed(url, &ssrf_err)) {
        PCV_LOG_WARN(ALERT_LOG_DOM,
                     "Webhook URL rejected (SSRF guard): %.100s — %s",
                     url, ssrf_err ? ssrf_err->message : "blocked");
        g_clear_error(&ssrf_err);
        return FALSE;
    }

    SoupSession *sess = soup_session_new();
                                        
    g_object_set(sess, "timeout", 10, NULL);
    SoupMessage *msg = soup_message_new("POST", url);
    if (!msg) { g_object_unref(sess); return FALSE; }
                                                             
    soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);

    GBytes *body = g_bytes_new(payload, strlen(payload));
    soup_message_set_request_body_from_bytes(msg, "application/json", body);

                                                   
                                                  
                                             
    if (secret && secret[0]) {
        GHmac *hmac = g_hmac_new(G_CHECKSUM_SHA256,
                                 (const guchar *)secret, strlen(secret));
        g_hmac_update(hmac, (const guchar *)payload, strlen(payload));
        gchar *sig = g_strdup_printf("sha256=%s", g_hmac_get_string(hmac));
        SoupMessageHeaders *hdrs = soup_message_get_request_headers(msg);
        soup_message_headers_replace(hdrs, "X-PureCVisor-Signature", sig);
        g_free(sig);
        g_hmac_unref(hmac);
    }

                                                         
    GBytes *resp = soup_session_send_and_read(sess, msg, NULL, NULL);
                                                          
    gboolean ok = (resp != nullptr && soup_message_get_status(msg) >= 200
                   && soup_message_get_status(msg) < 300);

    if (resp) g_bytes_unref(resp);
    g_bytes_unref(body);
    g_object_unref(msg);
    g_object_unref(sess);
    return ok;
}

  
                                                  
                                                 
                                               
   
static gboolean
_webhook_post(const gchar *url, const gchar *payload)
{
    g_auto(AlertConfigSnapshot) config = {0};
    _config_snapshot(&config);
    const gchar *target_url = (url && url[0]) ? url : config.webhook_url;
    return _webhook_post_once(target_url, payload, config.webhook_secret);
}

   
                                                 
  
                                                   
                                                    
  
                             
                                
                                     
                                       
                                             
   
static gboolean
_webhook_post_with_retry(const gchar *url, const gchar *payload,
                         const gchar *secret, gint max_retries)
{
    if (!url || !url[0]) return FALSE;

                                                                     
    for (gint attempt = 0; attempt <= max_retries; attempt++) {
        if (attempt > 0) {
            guint delay_ms = 1000 * (1 << (attempt - 1));                                    
            g_usleep((guint64)delay_ms * 1000);                                     
        }
        if (_webhook_post_once(url, payload, secret)) return TRUE;                          
        PCV_LOG_WARN(ALERT_LOG_DOM, "Webhook retry %d/%d failed for %.100s",
                     attempt + 1, max_retries, url);
    }
                                            
    pcv_alert_dlq_store(url, payload);
    return FALSE;
}

                                                            
                                             
                                                
                                        

typedef struct {
    gchar *url;                                             
    gchar *payload;                              
    gchar *format;                                     
    gchar *chat_id;                            
    gchar *secret;                                    
} WebhookAsyncCtx;

   
                                                                    
  
                                                  
                                     
  
                                                             
                                              
                          
  
                                               
                                                       
                                                
                                                
                                                             
   
static void _webhook_async_ctx_free(gpointer p) {
    WebhookAsyncCtx *ctx = p;
    g_free(ctx->url);
    g_free(ctx->payload);
    g_free(ctx->format);
    g_free(ctx->chat_id);
    pcv_secure_free_str(&ctx->secret);
    g_free(ctx);
}

                                              
                                               
static void
_webhook_async_worker(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c; (void)task;                                    
    WebhookAsyncCtx *ctx = data;
    _webhook_post_with_retry(ctx->url, ctx->payload, ctx->secret,
                             WEBHOOK_MAX_RETRIES);
}

   
                                              
  
                                      
   
static void
_webhook_post_async(const gchar *url, const gchar *payload,
                    const AlertConfigSnapshot *config)
{
    if (!url || !url[0]) return;
    WebhookAsyncCtx *ctx = g_new0(WebhookAsyncCtx, 1);
    ctx->url = g_strdup(url);
    ctx->payload = g_strdup(payload);
    ctx->format = g_strdup(config->webhook_format);
    ctx->chat_id = g_strdup(config->telegram_chat_id);
    ctx->secret = g_strdup(config->webhook_secret);

    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, ctx, _webhook_async_ctx_free);
    g_task_run_in_thread(task, _webhook_async_worker);
    g_object_unref(task);
}

   
                                                       
  
                                                   
                                                           
                          
  
                                          
                                     
                                
                                                              
  
                       
                                                                              
                                                            
                                                                 
                                                               
                                                                                               
                                                     
  
                                                  
                                                   
                                          
  
                                                    
   

                                                              

static GHashTable *g_vm_webhook_map = nullptr;                              
static GMutex g_vm_webhook_mu;

   
                                                     
  
                                                        
  
                              
                                                          
   
void
pcv_alert_set_vm_webhook(const gchar *vm_name, const gchar *webhook_url)
{
    if (!vm_name) return;                             
    g_mutex_lock(&g_vm_webhook_mu);
    if (!g_vm_webhook_map)                                          
        g_vm_webhook_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (webhook_url && *webhook_url)                                
        g_hash_table_insert(g_vm_webhook_map, g_strdup(vm_name), g_strdup(webhook_url));
    else                                               
        g_hash_table_remove(g_vm_webhook_map, vm_name);
    g_mutex_unlock(&g_vm_webhook_mu);
}

   
                                                                
  
                                                      
  
                                              
                                             
                                                           
                              
   
static gchar *
_get_vm_webhook_dup(const gchar *metric)
{
    if (!metric) return NULL;
    g_mutex_lock(&g_vm_webhook_mu);
    const gchar *url = g_vm_webhook_map
        ? g_hash_table_lookup(g_vm_webhook_map, metric)
        : NULL;
    gchar *copy = g_strdup(url);
    g_mutex_unlock(&g_vm_webhook_mu);
    return copy;
}

                                                           
                                                                       
gchar *
pcv_alert_engine_test_dup_vm_webhook(const gchar *vm_name)
{
    return _get_vm_webhook_dup(vm_name);
}

static void
_fire_alert(const gchar *metric, AlertLevel level, gdouble value)
{
                                                                     
                                                           
                                                 
                                              
                                                                            
    if (pcv_alert_is_silenced(metric)) {
        PCV_LOG_INFO(ALERT_LOG_DOM, "Alert suppressed (silenced): metric=%s", metric);
        return;
    }

    g_auto(AlertConfigSnapshot) config = {0};
    _config_snapshot(&config);
    const gchar *sev = (level == ALERT_CRIT) ? "CRIT" : "WARN";
    gchar hostname[64] = "unknown";
    gethostname(hostname, sizeof(hostname));

                               
    gchar ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

                            
    gchar msg[256];
    g_snprintf(msg, sizeof(msg), "[%s] %s %.1f%% on %s at %s",
               sev, metric, value, hostname, ts);

    PCV_LOG_WARN(ALERT_LOG_DOM, "%s", msg);
    _record_alert(metric, level, value, msg);

                                                     
                                                   
                                               
    GString *escaped_msg = g_string_new("");
    for (const char *p = msg; *p; p++) {
        if (*p == '"')       g_string_append(escaped_msg, "\\\"");               
        else if (*p == '\\') g_string_append(escaped_msg, "\\\\");               
        else if (*p == '\n') g_string_append(escaped_msg, "\\n");                 
        else                 g_string_append_c(escaped_msg, *p);                      
    }

    GString *escaped_host = g_string_new("");
    for (const char *p = hostname; *p; p++) {
        if (*p == '"')       g_string_append(escaped_host, "\\\"");
        else if (*p == '\\') g_string_append(escaped_host, "\\\\");
        else                 g_string_append_c(escaped_host, *p);
    }

                                               
    gchar payload[1024];
    if (g_strcmp0(config.webhook_format, "slack") == 0) {
                                                            
        g_snprintf(payload, sizeof(payload),
            "{\"text\":\"PureCVisor Alert: %s\"}", escaped_msg->str);
    } else if (g_strcmp0(config.webhook_format, "telegram") == 0) {
                                                                               
        g_snprintf(payload, sizeof(payload),
            "{\"chat_id\":\"%s\",\"text\":\"PureCVisor Alert: %s\"}",
            config.telegram_chat_id, escaped_msg->str);
    } else {
                                                                            
        g_snprintf(payload, sizeof(payload),
            "{\"severity\":\"%s\",\"metric\":\"%s\",\"value\":%.1f,"
            "\"host\":\"%s\",\"timestamp\":\"%s\"}",
            sev, metric, value, escaped_host->str, ts);
    }
    g_string_free(escaped_msg, TRUE);
    g_string_free(escaped_host, TRUE);

                                    
                                                   
    if (config.webhook_url[0]) {                                    
                                                             
        gchar *vm_wh = _get_vm_webhook_dup(metric);
                                                    
                                           
        const gchar *url = vm_wh ? vm_wh
                         : (level == ALERT_CRIT && config.webhook_crit_url[0])
                           ? config.webhook_crit_url : config.webhook_url;
        _webhook_post_async(url, payload, &config);
        g_free(vm_wh);
    }

                                                              
                                                
                                                  
                                                 
    pcv_webpush_notify(metric, level == ALERT_CRIT, msg);
}

   
                                                      
  
                                                   
                                                   
                            
  
                                                          
                                                       
                                                     
                                                  
   
void
pcv_alert_fire_event(const gchar *source, gboolean is_crit,
                     gdouble value, const gchar *message)
{
    if (!_engine_is_initialized()) return;
    if (!source)  source  = "event";
    if (!message) message = "";

                                      
    if (pcv_alert_is_silenced(source)) {
        PCV_LOG_INFO(ALERT_LOG_DOM, "Alert suppressed (silenced): source=%s", source);
        return;
    }

    AlertLevel level = is_crit ? ALERT_CRIT : ALERT_WARN;
    const gchar *sev = is_crit ? "CRIT" : "WARN";
    g_auto(AlertConfigSnapshot) config = {0};
    _config_snapshot(&config);

    gchar hostname[64] = "unknown";
    gethostname(hostname, sizeof(hostname));

    gchar ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    PCV_LOG_WARN(ALERT_LOG_DOM, "[%s] %s: %s", sev, source, message);
    _record_alert(source, level, value, message);

                                                             
                                                 
    GString *emsg = g_string_new("");
    for (const char *p = message; *p; p++) {
        if (*p == '"')       g_string_append(emsg, "\\\"");
        else if (*p == '\\') g_string_append(emsg, "\\\\");
        else if (*p == '\n') g_string_append(emsg, "\\n");
        else                 g_string_append_c(emsg, *p);
    }
    GString *ehost = g_string_new("");
    for (const char *p = hostname; *p; p++) {
        if (*p == '"')       g_string_append(ehost, "\\\"");
        else if (*p == '\\') g_string_append(ehost, "\\\\");
        else                 g_string_append_c(ehost, *p);
    }

    gchar payload[1024];
    if (g_strcmp0(config.webhook_format, "slack") == 0) {
        g_snprintf(payload, sizeof(payload),
            "{\"text\":\"PureCVisor Alert: [%s] %s\"}", sev, emsg->str);
    } else if (g_strcmp0(config.webhook_format, "telegram") == 0) {
        g_snprintf(payload, sizeof(payload),
            "{\"chat_id\":\"%s\",\"text\":\"PureCVisor Alert: [%s] %s\"}",
            config.telegram_chat_id, sev, emsg->str);
    } else {
        g_snprintf(payload, sizeof(payload),
            "{\"severity\":\"%s\",\"source\":\"%s\",\"value\":%.1f,"
            "\"message\":\"%s\",\"host\":\"%s\",\"timestamp\":\"%s\"}",
            sev, source, value, emsg->str, ehost->str, ts);
    }
    g_string_free(emsg, TRUE);
    g_string_free(ehost, TRUE);

    if (config.webhook_url[0]) {
        const gchar *url = (is_crit && config.webhook_crit_url[0])
            ? config.webhook_crit_url : config.webhook_url;
        _webhook_post_async(url, payload, &config);
    }

                                                                  
                           
    pcv_webpush_notify(source, is_crit, message);
}

   
                               
  
                                                    
                                                          
                                         
  
                                  
                                              
                                    
  
           
  
                                                                
                                                                               
                                        
                                       
                                                                   
      
                                                                        
                                                                                   
                                        
                                       
                                                                   
                                       
  
                                    
                                                                     
  
                                                                   
                                                                        
                                                 
  
                                                    
                                          
   
static void
_eval_metric(MetricWatch *w, const gchar *name, gdouble current_pct,
             gdouble warn_thresh, gdouble crit_thresh,
             gint eval_period_sec, gint dedup_window_sec)
{
    gint64 now = _mono_now();

                        
                                                  
    if (current_pct >= crit_thresh) {
                                                        
        if (w->crit_since == 0) w->crit_since = now;
                                                     
        if (!w->crit_fired && (now - w->crit_since) >= eval_period_sec) {
                                                       
                                                            
            if ((now - w->last_crit_fired_at) >= dedup_window_sec) {
                _fire_alert(name, ALERT_CRIT, current_pct);
                w->last_crit_fired_at = now;                          
            }
            w->crit_fired = TRUE;                                             
        }
    } else {
                                                   
        w->crit_since = 0;
        w->crit_fired = FALSE;
    }

                                              
                                             
                                                            
    if (current_pct >= warn_thresh && current_pct < crit_thresh) {
        if (w->warn_since == 0) w->warn_since = now;                   
        if (!w->warn_fired && (now - w->warn_since) >= eval_period_sec) {
                                                          
            if ((now - w->last_warn_fired_at) >= dedup_window_sec) {
                _fire_alert(name, ALERT_WARN, current_pct);
                w->last_warn_fired_at = now;
            }
            w->warn_fired = TRUE;
        }
    } else {
                                                       
        w->warn_since = 0;
        w->warn_fired = FALSE;
    }
}

                                                                   
void
pcv_alert_engine_test_seed_cpu_warn_elapsed(gint64 elapsed_sec)
{
    _ensure_alert_locks_initialized();
    g_mutex_lock(&G.state_mu);
    G.cpu.warn_since = _mono_now() - elapsed_sec;
    G.cpu.warn_fired = FALSE;
    G.cpu.last_warn_fired_at = 0;
    g_mutex_unlock(&G.state_mu);
}

                                                                         
void
pcv_alert_engine_test_eval_cpu_once(gdouble current_pct)
{
    g_auto(AlertConfigSnapshot) config = {0};
    _ensure_alert_locks_initialized();
    g_mutex_lock(&G.state_mu);
    _config_snapshot(&config);
    _eval_metric(&G.cpu, "CPU", current_pct,
                 config.cpu_warn, config.cpu_crit,
                 config.eval_period_sec, config.dedup_window_sec);
    g_mutex_unlock(&G.state_mu);
}

                                                                    
gboolean
pcv_alert_engine_test_secret_buffer_tail_zero(void)
{
    gboolean zero = TRUE;
    _ensure_alert_locks_initialized();
    g_rw_lock_reader_lock(&G.config_lock);
    gsize length = strnlen(G.webhook_secret, sizeof(G.webhook_secret));
    if (length == sizeof(G.webhook_secret)) {
        zero = FALSE;
    } else {
        for (gsize i = length; i < sizeof(G.webhook_secret); i++) {
            if (G.webhook_secret[i] != '\0') {
                zero = FALSE;
                break;
            }
        }
    }
    g_rw_lock_reader_unlock(&G.config_lock);
    return zero;
}

   
                                 
  
                                                               
  
                                            
                              
                              
                              
                                                
   
static gdouble
_get_metric_value(const gchar *name, gdouble cpu, gdouble mem, gdouble disk)
{
    if (g_strcmp0(name, "CPU") == 0)    return cpu;
    if (g_strcmp0(name, "Memory") == 0) return mem;
    if (g_strcmp0(name, "Disk") == 0)   return disk;
    return 0.0;
}

   
                                      
  
                                                         
                                                       
  
                                                 
                                                    
  
                                  
                                  
                                  
   
static void
_eval_composite_rules(const AlertConfigSnapshot *config,
                      gdouble cpu_pct, gdouble mem_pct, gdouble disk_pct)
{
    gint64 now = _mono_now();

    for (gint i = 0; i < config->n_composite_rules; i++) {
        const CompositeRule *r = &config->composite_rules[i];
        CompositeRuntime *runtime = &G.composite_runtime[i];
        if (!r->active) continue;

        gdouble val_a = _get_metric_value(r->metric_a, cpu_pct, mem_pct, disk_pct);
        gdouble val_b = _get_metric_value(r->metric_b, cpu_pct, mem_pct, disk_pct);

        gboolean cond_a = (val_a >= r->thresh_a);                        
        gboolean cond_b = (val_b >= r->thresh_b);                        
                                          
        gboolean triggered = (r->op == COMPOSITE_OP_AND)
                              ? (cond_a && cond_b) : (cond_a || cond_b);

        if (triggered) {
            if (runtime->since == 0) runtime->since = now;                      
                                                                  
            if (!runtime->fired
                && (now - runtime->since) >= config->eval_period_sec
                && (now - runtime->last_fired_at) >= config->dedup_window_sec) {
                const gchar *op_str = (r->op == COMPOSITE_OP_AND) ? "AND" : "OR";
                gchar *desc = g_strdup_printf("Composite: %s>=%.0f %s %s>=%.0f",
                    r->metric_a, r->thresh_a, op_str,
                    r->metric_b, r->thresh_b);
                gdouble report_val = (val_a > val_b) ? val_a : val_b;                          
                _fire_alert(desc, r->level, report_val);
                g_free(desc);
                runtime->fired = TRUE;
                runtime->last_fired_at = now;
            }
        } else {
            runtime->since = 0;
            runtime->fired = FALSE;
        }
    }
}

   
                                          
  
                                                          
                                                          
                                               
  
                                          
                                     
  
                                                                 
  
                                                       
  
                                                
                                              
   
static gdouble
_get_disk_percent_path(const gchar *path)
{
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) return 0.0;                                   
    guint64 total = (guint64)vfs.f_blocks * vfs.f_frsize;                            
    guint64 free_b = (guint64)vfs.f_bfree * vfs.f_frsize;                             
    if (total == 0) return 0.0;                    
    return 100.0 * (1.0 - (gdouble)free_b / (gdouble)total);                                 
}

                                                              
                                             
static gdouble
_get_disk_percent(void)
{
    return _get_disk_percent_path("/");
}

   
                                                 
  
                                                        
                                          
  
                                                        
                                       
   
static gdouble
_get_data_pool_disk_percent(void)
{
    const gchar *pool_path = pcv_config_get_string("storage", "image_dir", "/pcvpool");
    if (!pool_path || !*pool_path) pool_path = "/pcvpool";
                     
    struct statvfs vfs;
    if (statvfs(pool_path, &vfs) != 0) return 0.0;
    return _get_disk_percent_path(pool_path);
}

                                                            

#define SLA_CHECK_INTERVAL  60                      
static GHashTable *g_vm_uptime   = nullptr;                                              
static GHashTable *g_vm_downtime = nullptr;                                                
static GMutex g_sla_mu;

   
                                                
  
                                                      
                                                    
  
                                                          
                                                      
  
                                        
   
static void
_sla_check_vms(void)
{
    g_mutex_lock(&g_sla_mu);
    if (!g_vm_uptime) {
        g_vm_uptime   = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_vm_downtime = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    }
    g_mutex_unlock(&g_sla_mu);

                                           
    const gchar *argv[] = {"virsh", "list", "--all", "--name", NULL};
    gchar *out = nullptr;
    if (!pcv_spawn_sync(argv, &out, NULL, NULL) || !out) { g_free(out); return; }                          

    gchar **vms = g_strsplit(g_strstrip(out), "\n", -1);                        
    for (gchar **v = vms; *v; v++) {
        if (!**v) continue;
        const gchar *vm = *v;
        const gchar *state_argv[] = {"virsh", "domstate", vm, NULL};
        gchar *state = nullptr;
        gboolean state_ok = pcv_spawn_sync(state_argv, &state, nullptr, nullptr);
        if (!state_ok) {
            PCV_LOG_WARN("ALERT", "SLA: virsh domstate failed for '%s' — skipping this interval", vm);
            g_free(state);
            continue;
        }

        g_mutex_lock(&g_sla_mu);
        gint64 *up   = g_hash_table_lookup(g_vm_uptime, vm);
        gint64 *down = g_hash_table_lookup(g_vm_downtime, vm);
        if (!up)   { up   = g_new0(gint64, 1); g_hash_table_insert(g_vm_uptime,   g_strdup(vm), up); }
        if (!down) { down = g_new0(gint64, 1); g_hash_table_insert(g_vm_downtime, g_strdup(vm), down); }

                                                                        
        if (state && strstr(state, "running"))
            *up += SLA_CHECK_INTERVAL;                   
        else
            *down += SLA_CHECK_INTERVAL;                 
        g_mutex_unlock(&g_sla_mu);
        g_free(state);
    }
    g_strfreev(vms);
    g_free(out);
}

   
                                             
  
                                                  
                                      
  
                            
                                                                         
                                        
   
JsonObject *
pcv_alert_get_sla(const gchar *vm_name)
{
    JsonObject *obj = json_object_new();
    g_mutex_lock(&g_sla_mu);
    if (g_vm_uptime && vm_name) {
        gint64 *up   = g_hash_table_lookup(g_vm_uptime, vm_name);
        gint64 *down = g_hash_table_lookup(g_vm_downtime, vm_name);
        gint64 u = up ? *up : 0, d = down ? *down : 0;
        gint64 total = u + d;                           
                                                          
        gdouble pct = total > 0 ? (100.0 * (gdouble)u / (gdouble)total) : 100.0;
        json_object_set_double_member(obj, "uptime_percent", pct);
        json_object_set_int_member(obj, "uptime_seconds", u);
        json_object_set_int_member(obj, "downtime_seconds", d);
    }
    g_mutex_unlock(&g_sla_mu);
    return obj;
}

                                                            

   
                              
  
                                                    
                                                      
                                              
  
                                                   
                                                      
                                       
                                                    
                               
  
                                                        
  
                                                       
                                                       
  
                                     
                                                                 
   
static gpointer
_alert_thread(gpointer data)
{
    (void)data;
    g_auto(AlertConfigSnapshot) startup_config = {0};
    _config_snapshot(&startup_config);
    gint64 next_sla_check = _mono_now() + SLA_CHECK_INTERVAL;
    PCV_LOG_INFO(ALERT_LOG_DOM, "Alert engine started (eval=%ds, webhook=%s, format=%s)",
                 startup_config.eval_period_sec,
                 startup_config.webhook_url[0]
                    ? startup_config.webhook_url : "(none)",
                 startup_config.webhook_format);
    _config_snapshot_clear(&startup_config);

    for (;;) {
        g_mutex_lock(&G.lifecycle_mu);
        gboolean running = G.running;
        g_mutex_unlock(&G.lifecycle_mu);
        if (!running) break;

        g_auto(AlertConfigSnapshot) loop_config = {0};
        _config_snapshot(&loop_config);
        gboolean evaluate_metrics = loop_config.enabled;

          
                                  
          
                                                                  
                                                 
          
                                                                           
                                                                      
                                                               
           
                              
                                                      
                                                 
#if PCV_CLUSTER_ENABLED
        if (pcv_cluster_is_maintenance()) {
            evaluate_metrics = FALSE;
        }
#endif

        JsonObject *host = evaluate_metrics
            ? pcv_ebpf_telemetry_get_host()
            : NULL;
        if (evaluate_metrics && host) {
            gdouble cpu = json_object_get_double_member(host, "cpu_percent");
            gdouble mem = json_object_get_double_member(host, "mem_percent");
            gdouble disk = _get_disk_percent();

              
                                                                        
                                                                 
                                                            
               
            g_mutex_lock(&G.state_mu);
            g_auto(AlertConfigSnapshot) evaluation_config = {0};
            _config_snapshot(&evaluation_config);
            if (evaluation_config.enabled) {
                _eval_metric(&G.cpu, "CPU", cpu,
                             evaluation_config.cpu_warn,
                             evaluation_config.cpu_crit,
                             evaluation_config.eval_period_sec,
                             evaluation_config.dedup_window_sec);
                _eval_metric(&G.mem, "Memory", mem,
                             evaluation_config.mem_warn,
                             evaluation_config.mem_crit,
                             evaluation_config.eval_period_sec,
                             evaluation_config.dedup_window_sec);
                _eval_metric(&G.disk, "Disk", disk,
                             evaluation_config.disk_warn,
                             evaluation_config.disk_crit,
                             evaluation_config.eval_period_sec,
                             evaluation_config.dedup_window_sec);

                                                 
                gdouble data_pool_pct = _get_data_pool_disk_percent();
                if (data_pool_pct > 0.0) {
                    _eval_metric(&G.data_pool, "DataPool", data_pool_pct,
                                 evaluation_config.data_pool_warn,
                                 evaluation_config.data_pool_crit,
                                 evaluation_config.eval_period_sec,
                                 evaluation_config.dedup_window_sec);
                }

                                          
                _eval_composite_rules(&evaluation_config, cpu, mem, disk);
            }
            g_mutex_unlock(&G.state_mu);

            json_object_unref(host);
        }

                                                       
                                                              
                                                            
#define ESCALATION_INTERVAL_SEC  600           
        {
            gint64 esc_now = (gint64)time(NULL);
            g_mutex_lock(&G.mu);
            for (gint i = 0; i < G.hist_count && i < ALERT_HISTORY_MAX; i++) {
                AlertRecord *r = &G.history[i];
                                                                          
                if (r->level == ALERT_CRIT && !r->acknowledged &&
                    r->fired_at > 0 &&
                    (esc_now - r->fired_at) >= ESCALATION_INTERVAL_SEC &&
                    !r->escalated) {
                    r->escalated = TRUE;                                
                    gchar esc_msg[512];
                    g_snprintf(esc_msg, sizeof(esc_msg),
                               "[ESCALATION] Unacknowledged CRIT (id=%" G_GINT64_FORMAT "): %s",
                               r->alert_id, r->message);
                                                       
                                                       
                    g_mutex_unlock(&G.mu);
                                                                       
                                                          
                                                                           
                                                                 
                                                           
                    GString *esc_esc = g_string_new("");
                    for (const char *p = esc_msg; *p; p++) {
                        if (*p == '"')       g_string_append(esc_esc, "\\\"");
                        else if (*p == '\\') g_string_append(esc_esc, "\\\\");
                        else if (*p == '\n') g_string_append(esc_esc, "\\n");
                        else                 g_string_append_c(esc_esc, *p);
                    }
                    gchar esc_payload[768];
                    if (g_strcmp0(loop_config.webhook_format, "slack") == 0) {
                        g_snprintf(esc_payload, sizeof(esc_payload),
                            "{\"text\":\"%s\"}", esc_esc->str);
                    } else if (g_strcmp0(loop_config.webhook_format, "telegram") == 0) {
                        g_snprintf(esc_payload, sizeof(esc_payload),
                            "{\"chat_id\":\"%s\",\"text\":\"%s\"}",
                            loop_config.telegram_chat_id, esc_esc->str);
                    } else {
                        g_snprintf(esc_payload, sizeof(esc_payload),
                            "{\"severity\":\"CRIT\",\"event\":\"escalation\","
                            "\"text\":\"%s\"}", esc_esc->str);
                    }
                    g_string_free(esc_esc, TRUE);
                                                                    
                    const gchar *esc_url = loop_config.webhook_crit_url[0]
                        ? loop_config.webhook_crit_url : loop_config.webhook_url;
                    _webhook_post_async(esc_url, esc_payload, &loop_config);                        
                                                              
                                                                  
                                                                    
                    pcv_webpush_notify("escalation", TRUE, esc_msg);
                    PCV_LOG_WARN(ALERT_LOG_DOM, "%s", esc_msg);
                    g_mutex_lock(&G.mu);                          
                }
            }
            g_mutex_unlock(&G.mu);
        }

                                                   
                                                             
                                                              
        if (_mono_now() >= next_sla_check) {
            _sla_check_vms();
            next_sla_check = _mono_now() + SLA_CHECK_INTERVAL;
        }

                                                                 
        g_mutex_lock(&G.lifecycle_mu);
        if (G.running) {
            gint64 deadline = g_get_monotonic_time()
                + (gint64)ALERT_CHECK_SEC * G_TIME_SPAN_SECOND;
            g_cond_wait_until(&G.lifecycle_cond, &G.lifecycle_mu, deadline);
        }
        g_mutex_unlock(&G.lifecycle_mu);
    }

    PCV_LOG_INFO(ALERT_LOG_DOM, "Alert engine stopped");
    return NULL;
}

   
                                                              
  
                                                   
                                     
  
                                                   
                                                       
                                                             
                                            
  
                                                        
                                                              
                                                             
                                              
                                                                           
                                               
                                               
                                                                
   
static void
_wake_or_recover_alert_worker(void)
{
    g_mutex_lock(&G.lifecycle_mu);
    if (G.initialized) {
        if (!G.running || !G.thread) {
            G.running = TRUE;
            G.thread = g_thread_new("alert-engine", _alert_thread, NULL);
            G.worker_start_count++;
        }
        g_cond_broadcast(&G.lifecycle_cond);
    }
    g_mutex_unlock(&G.lifecycle_mu);
}

   
                                                                          
  
                                              
                                                  
                                               
  
           
                                                
                               
                                                      
                                                       
                                              
  
                                                      
                                           
  
                                                      
                          
                                        
                                                                        
                                          
                                        
                                                  
                                                         
   
static void
_daemon_source_add_strict_int(JsonObject *source,
                              GKeyFile *candidate_file,
                              const gchar *key,
                              gint default_value)
{
    GError *error = NULL;
    gchar *raw = candidate_file
        ? g_key_file_get_value(candidate_file, "alert", key, &error)
        : pcv_config_dup_raw_value("alert", key);
    if (error) {
        g_clear_error(&error);
        g_clear_pointer(&raw, g_free);
    }
    if (!raw) {
        json_object_set_int_member(source, key, default_value);
        return;
    }

    gchar *value = g_strdup(raw);
    g_strstrip(value);
    gchar *end = NULL;
    errno = 0;
    gint64 parsed = g_ascii_strtoll(value, &end, 10);
    if (value[0] != '\0' && end != value && *end == '\0'
        && errno != ERANGE && parsed >= G_MININT && parsed <= G_MAXINT) {
        json_object_set_int_member(source, key, parsed);
    } else {
          
                                                       
                                                        
           
        json_object_set_string_member(source, key, raw);
    }
    g_free(value);
    g_free(raw);
}

   
                                                          
  
                                                        
                                                  
                                                    
  
                              
                                                                     
                                          
                                           
                                                              
                                                         
                                                  
   
static gchar *
_daemon_source_dup_string(GKeyFile *candidate_file,
                          const gchar *key,
                          const gchar *default_value)
{
    if (!candidate_file) {
        return g_strdup(pcv_config_get_string(
            "alert", key, default_value));
    }

    GError *error = NULL;
    gchar *value = g_key_file_get_string(
        candidate_file, "alert", key, &error);
    if (error || !value || !*value) {
        g_clear_error(&error);
        g_free(value);
        return g_strdup(default_value);
    }
    return value;
}

   
                                                            
  
                                                  
                                                    
                                             
  
                                                            
                                                          
                                                     
                                                           
  
                                                           
                                                                 
                                                          
                                                  
                                                 
                                                                     
                                   
                                                       
                                                                          
   
static gchar *
_daemon_source_dup_secret(GKeyFile *candidate_file)
{
    if (!candidate_file)
        return pcv_config_get_secret("alert", "webhook_secret", "");

    const gchar *environment =
        g_getenv("PCV_SECRET_ALERT_WEBHOOK_SECRET");
    if (environment && *environment)
        return g_strdup(environment);

    GError *error = NULL;
    gchar *raw = g_key_file_get_string(
        candidate_file, "alert", "webhook_secret", &error);
    if (error || !raw || !*raw) {
        g_clear_error(&error);
        g_free(raw);
        return g_strdup("");
    }
    if (g_str_has_prefix(raw, "ENC2:")
        || g_str_has_prefix(raw, "ENC:")) {
        gchar *decrypted = pcv_config_decrypt_value(raw);
        pcv_secure_free_str(&raw);
        return decrypted ? decrypted : g_strdup("");
    }
    return raw;
}

   
                                                                            
  
                                                  
                                                     
                           
  
                                                    
                                                                 
  
                                                                  
                                                                 
  
                                                
                                                                    
                                                     
                            
                                                                    
                                                  
                                          
                                             
                                                                        
   
static JsonObject *
_build_daemon_source(GKeyFile *candidate_file, gchar **secret_out)
{
    JsonObject *source = json_object_new();
    GError *error = NULL;
    gchar *enabled = candidate_file
        ? g_key_file_get_value(
              candidate_file, "alert", "enabled", &error)
        : pcv_config_dup_raw_value("alert", "enabled");
    if (error) {
        g_clear_error(&error);
        g_clear_pointer(&enabled, g_free);
    }
    if (!enabled)
        enabled = g_strdup("false");
    if (g_ascii_strcasecmp(enabled, "true") == 0
        || g_strcmp0(enabled, "1") == 0) {
        json_object_set_boolean_member(source, "enabled", TRUE);
    } else if (g_ascii_strcasecmp(enabled, "false") == 0
               || g_strcmp0(enabled, "0") == 0) {
        json_object_set_boolean_member(source, "enabled", FALSE);
    } else {
        json_object_set_string_member(source, "enabled", enabled);
    }
    g_free(enabled);

    static const struct {
        const gchar *key;
        gint default_value;
    } integer_fields[] = {
        {"cpu_warn", 80},
        {"cpu_crit", 95},
        {"mem_warn", 85},
        {"mem_crit", 95},
        {"disk_warn", 80},
        {"disk_crit", 90},
        {"data_pool_warn", 80},
        {"data_pool_crit", 90},
        {"eval_period", 30},
        {"dedup_window", ALERT_DEDUP_WINDOW_SEC},
    };
    for (guint i = 0; i < G_N_ELEMENTS(integer_fields); i++) {
        _daemon_source_add_strict_int(
            source, candidate_file,
            integer_fields[i].key, integer_fields[i].default_value);
    }

    gchar *webhook_url = _daemon_source_dup_string(
        candidate_file, "webhook_url", "");
    json_object_set_string_member(source, "webhook_url", webhook_url);
    g_free(webhook_url);
    gchar *webhook_crit_url = _daemon_source_dup_string(
        candidate_file, "webhook_crit_url", "");
    json_object_set_string_member(
        source, "webhook_crit_url", webhook_crit_url);
    g_free(webhook_crit_url);

    gchar *webhook_format = candidate_file
        ? g_key_file_get_value(
              candidate_file, "alert", "webhook_format", &error)
        : pcv_config_dup_raw_value("alert", "webhook_format");
    if (error) {
        g_clear_error(&error);
        g_clear_pointer(&webhook_format, g_free);
    }
    json_object_set_string_member(source, "webhook_format",
                                  webhook_format ? webhook_format : "generic");
    g_free(webhook_format);

    gchar *telegram_chat_id = _daemon_source_dup_string(
        candidate_file, "telegram_chat_id", "");
    json_object_set_string_member(
        source, "telegram_chat_id", telegram_chat_id);
    g_free(telegram_chat_id);
    *secret_out = _daemon_source_dup_secret(candidate_file);
    return source;
}

   
                                                
  
                                                    
                                               
  
                                                          
                                                          
                                     
  
                                                          
                                                               
                                                            
                              
                                                            
                                                           
                                                  
                                             
                                                     
                         
                                                          
   
gboolean
pcv_alert_engine_validate_daemon_config_data(const gchar *data,
                                             gsize length)
{
    if (!data)
        return FALSE;

    GKeyFile *candidate_file = g_key_file_new();
    GError *error = NULL;
    if (!g_key_file_load_from_data(
            candidate_file, data, length,
            G_KEY_FILE_NONE, &error)) {
        g_clear_error(&error);
        g_key_file_free(candidate_file);
        return FALSE;
    }

    gchar *secret = NULL;
    JsonObject *source = _build_daemon_source(
        candidate_file, &secret);
    g_auto(AlertConfigSnapshot) candidate = {0};
    gboolean valid = _build_daemon_config_candidate(
        source, secret, TRUE, &candidate);
    pcv_secure_free_str(&secret);
    json_object_unref(source);
    g_key_file_free(candidate_file);
    return valid;
}

   
                                                
  
                                              
                                                 
  
                                                                 
                                                     
                                                      
                       
  
                                             
                                                            
                                                               
                                                    
                                                               
                                                         
                                                               
   
PcvAlertConfigSetResult
pcv_alert_engine_load_daemon_config(PcvAlertConfigSourceMode mode)
{
    gchar *secret = NULL;
    JsonObject *source = _build_daemon_source(NULL, &secret);
    PcvAlertConfigSetResult result =
        _apply_daemon_config(source, mode, secret, TRUE);
    pcv_secure_free_str(&secret);
    json_object_unref(source);
    return result;
}

   
                                                
  
                                                
                                                
                                     
  
            
                                                               
                                                                    
                                                             
  
                                                      
                                                      
                                                                   
                       
                                                 
                                                                     
                                                      
                            
                                      
   
PcvAlertConfigSetResult
pcv_alert_engine_reload_daemon_config(void)
{
    _ensure_alert_locks_initialized();
    g_mutex_lock(&G.reload_mu);
    if (!pcv_config_reload()) {
          
                                                          
                                                                  
                          
           
        g_mutex_lock(&G.state_mu);
        g_rw_lock_writer_lock(&G.config_lock);
        G.daemon_config_valid = FALSE;
        g_strlcpy(G.daemon_config_error, "invalid_alert_config",
                  sizeof(G.daemon_config_error));
        g_rw_lock_writer_unlock(&G.config_lock);
        g_mutex_unlock(&G.state_mu);
        g_mutex_unlock(&G.reload_mu);
        return PCV_ALERT_CONFIG_SET_INVALID;
    }

    PcvAlertConfigSetResult result = pcv_alert_engine_load_daemon_config(
        PCV_ALERT_CONFIG_SOURCE_RELOAD);
    g_mutex_unlock(&G.reload_mu);
    return result;
}

   
                                                
  
                                               
                                                  
                                       
  
                                                         
                                               
             
  
                                                          
                                                                           
                                      
                                                                   
                                                           
                                                            
                                         
                                                                   
   
void
pcv_alert_engine_restore_daemon_source_status(gboolean valid,
                                               const gchar *error)
{
    _ensure_alert_locks_initialized();
    g_mutex_lock(&G.state_mu);
    g_rw_lock_writer_lock(&G.config_lock);
    G.daemon_config_valid = valid;
    if (valid) {
        G.daemon_config_error[0] = '\0';
    } else {
        g_strlcpy(G.daemon_config_error,
                  (error && *error) ? error : "invalid_alert_config",
                  sizeof(G.daemon_config_error));
    }
    g_rw_lock_writer_unlock(&G.config_lock);
    g_mutex_unlock(&G.state_mu);
}

                                                                 

   
                                           
  
                                                   
                                                 
  
         
                                 
                                                   
                                                            
                                               
  
                                    
                                              
                                                
                                                
                                                
                                                
                                                
                                                
                                              
                                                  
                                         
                                                  
  
                                   
                            
   
void
pcv_alert_engine_init(void)
{
    _ensure_alert_locks_initialized();
    g_mutex_lock(&G.lifecycle_mu);
    if (G.initialized) {
        g_mutex_unlock(&G.lifecycle_mu);
        return;
    }
    g_mutex_unlock(&G.lifecycle_mu);

                                                            
                                                          
    pcv_alert_dlq_set_post_fn(_webhook_post);

    PcvAlertConfigSetResult source_result =
        pcv_alert_engine_load_daemon_config(PCV_ALERT_CONFIG_SOURCE_STARTUP);

    g_mutex_lock(&G.mu);
    memset(G.history, 0, sizeof(G.history));
    G.hist_count = 0;
    G.hist_idx = 0;
    g_mutex_unlock(&G.mu);

    g_mutex_lock(&G.lifecycle_mu);
    G.initialized = TRUE;
    G.running = TRUE;
    G.worker_start_count = 0;
    G.thread = g_thread_new("alert-engine", _alert_thread, NULL);
    G.worker_start_count++;
    g_mutex_unlock(&G.lifecycle_mu);

    g_auto(AlertConfigSnapshot) active = {0};
    _config_snapshot(&active);
    if (!active.enabled) {
        PCV_LOG_INFO(ALERT_LOG_DOM,
                     "Alert metric evaluation disabled; event processing remains active");
    }
    if (source_result != PCV_ALERT_CONFIG_SET_OK) {
        PCV_LOG_WARN(ALERT_LOG_DOM,
                     "Invalid [alert] source; safe defaults applied");
    }
}

   
                                     
  
                                                    
                              
  
         
                                                          
                                         
                                             
                                   
                                            
  
                                          
                            
   
void
pcv_alert_engine_shutdown(void)
{
    _ensure_alert_locks_initialized();
    g_mutex_lock(&G.lifecycle_mu);
    if (!G.initialized) {
        g_mutex_unlock(&G.lifecycle_mu);
        return;
    }
    G.initialized = FALSE;
    G.running = FALSE;
    GThread *thread = G.thread;
    G.thread = NULL;
    g_cond_broadcast(&G.lifecycle_cond);
    g_mutex_unlock(&G.lifecycle_mu);

    if (thread) {
        g_thread_join(thread);
    }

    g_mutex_lock(&G.state_mu);
    g_rw_lock_writer_lock(&G.config_lock);
    pcv_secure_wipe(G.webhook_secret, sizeof(G.webhook_secret));
    g_rw_lock_writer_unlock(&G.config_lock);
    g_mutex_unlock(&G.state_mu);
}

                                                       
                                                                       
gint
pcv_alert_engine_test_worker_start_count(void)
{
    gint count;
    _ensure_alert_locks_initialized();
    g_mutex_lock(&G.lifecycle_mu);
    count = G.worker_start_count;
    g_mutex_unlock(&G.lifecycle_mu);
    return count;
}

   
                                      
  
                                                          
  
                                                 
  
               
                                                    
                                                      
                                         
                                              
  
                        
                                                  
                                        
                                       
                                    
                                              
  
                                                                         
                                        
  
                                        
                                                                
   
                                                                 
                             
                                                                    

   
                         
                       
  
                                                 
                               
  
                                                 
  
                                                 
   
gboolean
pcv_alert_acknowledge(gint64 alert_id)
{
    g_mutex_lock(&G.mu);
    for (gint i = 0; i < G.hist_count && i < ALERT_HISTORY_MAX; i++) {
        if (G.history[i].alert_id == alert_id) {
            G.history[i].acknowledged = TRUE;
            g_mutex_unlock(&G.mu);
            PCV_LOG_INFO(ALERT_LOG_DOM,
                         "Alert %" G_GINT64_FORMAT " acknowledged", alert_id);
            return TRUE;
        }
    }
    g_mutex_unlock(&G.mu);
    return FALSE;
}

JsonArray *
pcv_alert_engine_get_history(void)
{
    AlertRecord *snapshot = g_new(AlertRecord, ALERT_HISTORY_MAX);
    gint snapshot_count = 0;

    g_mutex_lock(&G.mu);
    snapshot_count = G.hist_count;
    gint start = (G.hist_count < ALERT_HISTORY_MAX) ? 0 : G.hist_idx;
    for (gint i = 0; i < G.hist_count; i++) {
        gint idx = (start + i) % ALERT_HISTORY_MAX;
        snapshot[i] = G.history[idx];
    }
    g_mutex_unlock(&G.mu);

                                                                
                                               
    JsonArray *arr = json_array_new();
    for (gint i = 0; i < snapshot_count; i++) {
        const AlertRecord *r = &snapshot[i];
        JsonObject *obj = json_object_new();
        json_object_set_string_member(obj, "metric",  r->metric);
        json_object_set_string_member(obj, "severity", r->level == ALERT_CRIT ? "crit" : "warn");
        json_object_set_double_member(obj, "value",    r->value);
        json_object_set_int_member   (obj, "timestamp",r->fired_at);
        json_object_set_string_member(obj, "message",  r->message);
        json_object_set_int_member   (obj, "alert_id", r->alert_id);                       
        json_object_set_boolean_member(obj, "acknowledged", r->acknowledged);              
        json_object_set_boolean_member(obj, "escalated", r->escalated);                    
        json_array_add_object_element(arr, obj);                         
    }
    g_free(snapshot);
    return arr;
}

   
                                        
  
                                                            
  
                      
                                             
                                                  
                                                  
                                                  
                                                  
                                                  
                                                  
                                                
                                            
                                        
                                                                
                                                 
                                                     
  
                                                                           
  
                                        
                                
   
JsonObject *
pcv_alert_engine_get_config(void)
{
    g_auto(AlertConfigSnapshot) snapshot = {0};
    _config_snapshot(&snapshot);

    g_mutex_lock(&G.mu);
    gint alert_count = G.hist_count;
    g_mutex_unlock(&G.mu);

    JsonObject *obj = json_object_new();
    json_object_set_boolean_member(obj, "enabled", snapshot.enabled);
    json_object_set_int_member(obj, "cpu_warn", snapshot.cpu_warn);
    json_object_set_int_member(obj, "cpu_crit", snapshot.cpu_crit);
    json_object_set_int_member(obj, "mem_warn", snapshot.mem_warn);
    json_object_set_int_member(obj, "mem_crit", snapshot.mem_crit);
    json_object_set_int_member(obj, "disk_warn", snapshot.disk_warn);
    json_object_set_int_member(obj, "disk_crit", snapshot.disk_crit);
    json_object_set_int_member(obj, "data_pool_warn", snapshot.data_pool_warn);
    json_object_set_int_member(obj, "data_pool_crit", snapshot.data_pool_crit);
    json_object_set_int_member(obj, "eval_period", snapshot.eval_period_sec);
    json_object_set_int_member(obj, "dedup_window", snapshot.dedup_window_sec);
    json_object_set_string_member(obj, "webhook_url", snapshot.webhook_url);
    json_object_set_string_member(obj, "webhook_format",
                                  snapshot.webhook_format);
    json_object_set_string_member(obj, "telegram_chat_id",
                                  snapshot.telegram_chat_id);
                                                    
                                                           
    json_object_set_boolean_member(obj, "webhook_secret_configured",
                                   snapshot.webhook_secret[0] != '\0');
    json_object_set_string_member(obj, "webhook_crit_url",
                                  snapshot.webhook_crit_url);
    json_object_set_int_member(obj, "config_revision",
                               snapshot.config_revision);
    json_object_set_boolean_member(obj, "daemon_config_valid",
                                    snapshot.daemon_config_valid);
    json_object_set_string_member(obj, "daemon_config_error",
                                  snapshot.daemon_config_error);
    json_object_set_int_member(obj, "alert_count", alert_count);

                      
    JsonArray *cr_arr = json_array_new();
    for (gint i = 0; i < snapshot.n_composite_rules; i++) {
        const CompositeRule *r = &snapshot.composite_rules[i];
        JsonObject *cr = json_object_new();
        json_object_set_boolean_member(cr, "active",   r->active);
        json_object_set_string_member (cr, "metric_a", r->metric_a);
        json_object_set_double_member (cr, "thresh_a", r->thresh_a);
        json_object_set_string_member (cr, "op",
                                       r->op == COMPOSITE_OP_AND ? "AND" : "OR");
        json_object_set_string_member (cr, "metric_b", r->metric_b);
        json_object_set_double_member (cr, "thresh_b", r->thresh_b);
        json_object_set_string_member (cr, "level",
                                       r->level == ALERT_CRIT ? "CRIT" : "WARN");
        json_array_add_object_element(cr_arr, cr);
    }
    json_object_set_array_member(obj, "composite_rules", cr_arr);

    return obj;
}

   
                                                 
  
                                                   
                                                   
  
                                                  
                                                         
  
                                                            
                                                          
                     
  
                                                           
                                                  
                                                                     
                                                                             
                                                                                 
                                                                         
                                       
                                                                      
                                                    
  
                                        
                                                                  
                                   
   
PcvAlertConfigSetResult
pcv_alert_engine_set_config(JsonObject *cfg, gint64 expected_revision)
{
    if (!cfg || json_object_get_size(cfg) == 0 || expected_revision < 1) {
        return PCV_ALERT_CONFIG_SET_INVALID;
    }
    _ensure_alert_locks_initialized();

      
                                                        
                                                                    
       
    g_mutex_lock(&G.state_mu);
    g_rw_lock_writer_lock(&G.config_lock);
    if (expected_revision != G.config_revision) {
        g_rw_lock_writer_unlock(&G.config_lock);
        g_mutex_unlock(&G.state_mu);
        return PCV_ALERT_CONFIG_SET_CONFLICT;
    }

    g_auto(AlertConfigSnapshot) candidate = {0};
    _config_snapshot_locked(&candidate);
    if (!_overlay_runtime_config(&candidate, cfg)) {
        g_rw_lock_writer_unlock(&G.config_lock);
        g_mutex_unlock(&G.state_mu);
        return PCV_ALERT_CONFIG_SET_INVALID;
    }
    _config_commit_locked(&candidate);
    G.config_revision++;
    candidate.config_revision = G.config_revision;

      
                                    
                                                   
                                    
                                                        
       
    _reset_metric_watch_state_locked();
    if (pcv_test_alert_config_commit_hook) {
        pcv_test_alert_config_commit_hook();
    }
    g_rw_lock_writer_unlock(&G.config_lock);
    g_mutex_unlock(&G.state_mu);
    _wake_or_recover_alert_worker();

    PCV_LOG_INFO(ALERT_LOG_DOM, "Alert config updated: enabled=%d cpu=%d/%d mem=%d/%d disk=%d/%d eval=%ds dedup=%ds webhook=%s",
                 candidate.enabled,
                 candidate.cpu_warn, candidate.cpu_crit,
                 candidate.mem_warn, candidate.mem_crit,
                 candidate.disk_warn, candidate.disk_crit,
                 candidate.eval_period_sec, candidate.dedup_window_sec,
                 candidate.webhook_url[0] ? "(configured)" : "(none)");
    return PCV_ALERT_CONFIG_SET_OK;
}

  
                                                         
                                                                            
                             
                                                                     
   
PcvAlertConfigSetResult
pcv_alert_engine_apply_daemon_config(JsonObject *cfg,
                                     PcvAlertConfigSourceMode mode)
{
    return _apply_daemon_config(cfg, mode, NULL, FALSE);
}

   
                                                                     
  
                                                  
                                                    
                                   
  
                           
                                                     
                                            
                                                                          
                                                               
                                               
                                                     
  
                                                                   
                                                                 
                                               
                                                          
                                         
                                                                            
                                                                 
                                                                           
                                                 
                            
   
static gboolean
_build_daemon_config_candidate(JsonObject *cfg,
                               const gchar *source_secret,
                               gboolean secret_supplied_externally,
                               AlertConfigSnapshot *candidate)
{
    _safe_config_defaults(candidate);
    gboolean valid = _daemon_config_is_complete(
                         cfg, secret_supplied_externally)
        && _overlay_runtime_config(candidate, cfg);
    if (valid && secret_supplied_externally) {
        valid = source_secret
            && strlen(source_secret) < sizeof(candidate->webhook_secret);
        if (valid) {
            g_strlcpy(candidate->webhook_secret, source_secret,
                      sizeof(candidate->webhook_secret));
        }
    }
    return valid;
}

   
                                                                
  
                                                 
                                                    
                                             
  
              
                                                                   
                                                      
                                             
  
                         
                                                            
                                               
                                                          
                                                     
                                   
                                                                    
                                    
  
                                                               
                                                              
                                                                      
                                                            
                                                 
                                                       
                                                           
                                                                
                                                    
   
static PcvAlertConfigSetResult
_apply_daemon_config(JsonObject *cfg,
                     PcvAlertConfigSourceMode mode,
                     const gchar *source_secret,
                     gboolean secret_supplied_externally)
{
    _ensure_alert_locks_initialized();
    if (mode != PCV_ALERT_CONFIG_SOURCE_STARTUP
        && mode != PCV_ALERT_CONFIG_SOURCE_RELOAD) {
        return PCV_ALERT_CONFIG_SET_INVALID;
    }

    g_auto(AlertConfigSnapshot) candidate = {0};
    gboolean valid = _build_daemon_config_candidate(
        cfg, source_secret, secret_supplied_externally, &candidate);

    g_mutex_lock(&G.state_mu);
    g_rw_lock_writer_lock(&G.config_lock);
    if (!valid) {
        if (mode == PCV_ALERT_CONFIG_SOURCE_STARTUP) {
            g_auto(AlertConfigSnapshot) safe = {0};
            _safe_config_defaults(&safe);
            _config_commit_locked(&safe);
            G.config_revision = 1;
            _reset_metric_watch_state_locked();
        }
        G.daemon_config_valid = FALSE;
        g_strlcpy(G.daemon_config_error, "invalid_alert_config",
                  sizeof(G.daemon_config_error));
        g_rw_lock_writer_unlock(&G.config_lock);
        g_mutex_unlock(&G.state_mu);
        return PCV_ALERT_CONFIG_SET_INVALID;
    }

    _config_commit_locked(&candidate);
    G.daemon_config_valid = TRUE;
    G.daemon_config_error[0] = '\0';
    if (mode == PCV_ALERT_CONFIG_SOURCE_STARTUP) {
        G.config_revision = 1;
    } else {
        G.config_revision++;
    }
    _reset_metric_watch_state_locked();
    g_rw_lock_writer_unlock(&G.config_lock);
    g_mutex_unlock(&G.state_mu);

    if (mode == PCV_ALERT_CONFIG_SOURCE_RELOAD) {
        _wake_or_recover_alert_worker();
    }
    return PCV_ALERT_CONFIG_SET_OK;
}

                                                              
                                                              
                                                               

   
                                                               
  
                                                 
                                                     
  
                                                                    
   
JsonArray *
pcv_alert_engine_dlq_list(void)
{
    return pcv_alert_dlq_list();
}

   
                                                                     
  
                                                  
                                   
  
                                                 
                          
                                                                     
   
JsonObject *
pcv_alert_engine_dlq_retry(void)
{
    return pcv_alert_dlq_retry();
}

                                                                 
                                                                                    
                                                                          
                                                              
                                                           
                                                                    
