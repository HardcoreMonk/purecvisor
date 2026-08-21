   
                       
                                                                     
  
                           
                                                   
                                                    
                                        
  
                                                            
                                 
  
                                                        
                                                      
                                                       
                                               
                                                    
                                
  
                                                        
                                                    
                
                                                               
                                                                
                                                          
                                                
                                                  
                                                      
                                                            
                                                                               
                                                             
  
                                                             
                                                                 
                                             
                                                   
                                                  
                                                        
  
                       
                                                              
                                                 
                                                       
                                                            
   
#include "modules/security/pcv_suricata.h"
#include "modules/security/pcv_suricata_rules.h"                                    
#include "modules/security/hips_actions.h"                                     
#include "modules/security/pcv_suricata_ips.h"                                    

#include "utils/pcv_spawn.h"
#include "utils/pcv_log.h"
#include "modules/audit/pcv_audit.h"
#include "modules/security/security_store.h"
#include "modules/daemons/prometheus_exporter.h"                                

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <glib/gstdio.h>

#define SURICATA_LOG_DOM            "suricata"
#define SURICATA_PROBE_TIMEOUT_SEC  5
#define SURICATA_RELOAD_TIMEOUT_SEC 5
#define SURICATA_RESTART_TIMEOUT_SEC 30
#define SURICATA_HEALTH_INTERVAL_SEC 30
#define SURICATA_EVE_POLL_INTERVAL_SEC 1

                                                            

static struct {
    GThread  *thread;
    gboolean  running;
    gboolean  initialized;
    gboolean  absent_logged;                                        
} G = {0};

                                           
                                                              
                                                         
static struct {
    GMutex           mu;
    gboolean         valid;                           
    PcvSuricataState state;
} G_probe_cache;

  
                                                        
                                                               
                                                      
                                               
                                         
   
static void
_probe_cache_store(PcvSuricataState s)
{
    g_mutex_lock(&G_probe_cache.mu);
    G_probe_cache.state = s;
    G_probe_cache.valid = TRUE;
    g_mutex_unlock(&G_probe_cache.mu);
}

                                                         

   
                                                                         
                                           
                                                   
                   
                                                                 
                                                                       
   
PcvSuricataState
pcv_suricata_state_from_output(const char *out)
{
    if (out && g_str_has_prefix(out, "active"))
        return PCV_SURICATA_ACTIVE;
    if (out && g_str_has_prefix(out, "failed"))
        return PCV_SURICATA_FAILED;
                                                           
                                                      
                                        
    return PCV_SURICATA_INACTIVE;
}

   
                                                            
                                                  
                  
                                                          
                                         
                                                         
                                                                         
   
PcvSuricataState
pcv_suricata_probe(void)
{
    gchar *bin = g_find_program_in_path("suricata");
    if (!bin)
        return PCV_SURICATA_ABSENT;
    g_free(bin);

    const gchar *argv[] = {"systemctl", "is-active", "suricata", NULL};
    gchar *out = NULL;
    pcv_spawn_sync_timeout(argv, &out, NULL, SURICATA_PROBE_TIMEOUT_SEC, NULL);

    PcvSuricataState s = pcv_suricata_state_from_output(out);
    g_free(out);
    return s;
}

   
                                                                
                                                  
                                                               
                                                                    
   
gboolean
pcv_suricata_reload(GError **error)
{
    const gchar *argv[] = {"systemctl", "reload", "suricata", NULL};
    return pcv_spawn_sync_timeout(argv, NULL, NULL, SURICATA_RELOAD_TIMEOUT_SEC, error);
}

   
                                                           
                                                           
                                                  
   
const char *
pcv_suricata_state_str(PcvSuricataState s)
{
    switch (s) {
    case PCV_SURICATA_ACTIVE:   return "active";
    case PCV_SURICATA_INACTIVE: return "inactive";
    case PCV_SURICATA_FAILED:   return "failed";
    case PCV_SURICATA_ABSENT:   return "absent";
    default:                    return "unknown";
    }
}

                                                              

   
                  
  
                                                       
                                                      
                                               
                                                         
  
                                                                   
                                            
                                                              
                                                    
                                                    
                                                                                  
                                                  
  
                                                                         
                                                                     
                                                  
                                                                 
                                                     
                                    
   
static gpointer
_health_thread(gpointer data)
{
    (void)data;

    while (G.running) {
        PcvSuricataState s = pcv_suricata_probe();
        _probe_cache_store(s);                                   

        if (s == PCV_SURICATA_ABSENT) {
            if (!G.absent_logged) {
                G.absent_logged = TRUE;
                PCV_LOG_INFO(SURICATA_LOG_DOM,
                    "suricata binary not found — health watch degraded "
                    "(no restart attempts, no re-alert)");
            }
        } else if (s == PCV_SURICATA_FAILED) {
            gint64 start_us = g_get_monotonic_time();

            const gchar *argv[] = {"systemctl", "restart", "suricata", NULL};
            GError *error = NULL;
            gboolean ok = pcv_spawn_sync_timeout(argv, NULL, NULL,
                                                  SURICATA_RESTART_TIMEOUT_SEC, &error);

            gint64 dur_ms = (g_get_monotonic_time() - start_us) / 1000;

            if (ok) {
                PCV_LOG_WARN(SURICATA_LOG_DOM,
                    "suricata FAILED — restarted successfully (%" G_GINT64_FORMAT "ms)",
                    dur_ms);
                pcv_audit_log("system", "suricata.restart", "suricata",
                              "ok", 0, dur_ms, "local");
            } else {
                PCV_LOG_ERROR(SURICATA_LOG_DOM,
                    "suricata FAILED — restart failed: %s",
                    error ? error->message : "unknown error");
                pcv_audit_log("system", "suricata.restart", "suricata",
                              "fail", error ? error->code : -1, dur_ms, "local");
            }
            g_clear_error(&error);
        }
                                                          

                                                          
                                                                 
                                     
        pcv_suricata_metrics_tick();

                                                                 
                                                         
        pcv_suricata_ips_health_tick();

        g_usleep(SURICATA_HEALTH_INTERVAL_SEC * G_USEC_PER_SEC);
    }

    PCV_LOG_INFO(SURICATA_LOG_DOM, "suricata health watch thread stopped");
    return NULL;
}

   
                                                                      
                                                
                                                 
                                          
   
void
pcv_suricata_health_start(void)
{
    if (G.initialized) {
        PCV_LOG_WARN(SURICATA_LOG_DOM,
            "pcv_suricata_health_start() called twice — ignoring (idempotent)");
        return;
    }
    G.running = TRUE;
    G.initialized = TRUE;
    G.absent_logged = FALSE;
    G.thread = g_thread_new("suricata-health", _health_thread, NULL);
    PCV_LOG_INFO(SURICATA_LOG_DOM,
        "suricata health watch started (%ds interval)", SURICATA_HEALTH_INTERVAL_SEC);
}

   
                                                              
                                              
                                                          
                                                            
                                                             
                                                         
   
void
pcv_suricata_health_stop(void)
{
    if (!G.initialized)
        return;
    G.running = FALSE;
    if (G.thread) {
        g_thread_join(G.thread);
        G.thread = NULL;
    }
    G.initialized = FALSE;
}

                                                         

   
                                                           
                                                   
                                                       
                                     
                                                            
                                                         
                     
                                                          
                                                             
                                                          
   
gboolean
pcv_suricata_rate_allow(PcvSuricataRateState *st, gint64 now_usec)
{
    if (!st)
        return FALSE;

    if (!st->seeded) {
                                                    
                                                            
                                                       
                                           
        st->tokens = (gdouble)PCV_SURICATA_RATE_BURST;
        st->last_update_usec = now_usec;
        st->seeded = TRUE;
    } else if (now_usec > st->last_update_usec) {
        gdouble elapsed_sec = (now_usec - st->last_update_usec) / (gdouble)G_USEC_PER_SEC;
        st->tokens = MIN(st->tokens + elapsed_sec * PCV_SURICATA_RATE_PER_SEC,
                          (gdouble)PCV_SURICATA_RATE_BURST);
        st->last_update_usec = now_usec;
    }

    if (st->tokens >= 1.0) {
        st->tokens -= 1.0;
        return TRUE;
    }
    return FALSE;
}

                                                             

static GQuark
suricata_eve_error_quark(void)
{
    return g_quark_from_static_string("pcv-suricata-eve");
}

  
                                                                     
                                              
                                                        
                                                    
                                                        
                                                                
                                                            
                                          
                                                        
                                          
   
  
                                                  
                                                
                      
                                         
                     
                                     
                                                          
   
static gint64
_get_int_member_safe(JsonObject *o, const char *name, gint64 def)
{
    if (!o) {
        return def;
    }
    JsonNode *node = json_object_get_member(o, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) {
        return def;                                         
    }
    GType vtype = json_node_get_value_type(node);
    if (vtype != G_TYPE_INT64 && vtype != G_TYPE_INT) {
        return def;                                            
    }
    return json_node_get_int(node);
}

  
                                                                   
                                          
                                                  
                                                  
   
static const char *
_get_str_member_safe(JsonObject *o, const char *name, const char *def)
{
    if (!o) {
        return def;
    }
    JsonNode *node = json_object_get_member(o, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) {
        return def;
    }
    if (json_node_get_value_type(node) != G_TYPE_STRING) {
        return def;                                     
    }
    return json_node_get_string(node);
}

  
                                                                    
                                                      
                                    
                                                 
                                                   
   
static PcvSecuritySeverity
_eve_severity(JsonObject *alert)
{
                                                                   
                                                          
                          
    gint sev = (gint)_get_int_member_safe(alert, "severity", 3);
    if (sev <= 1) return PCV_SECURITY_SEVERITY_CRIT;
    if (sev == 2) return PCV_SECURITY_SEVERITY_WARN;
    return PCV_SECURITY_SEVERITY_INFO;
}

  
                                                         
                                                         
                                                                  
                                                        
   
  
                                                   
                                                    
         
                                                     
                                                  
                                                                    
   
static void
_eve_evidence_json(JsonObject *eve, JsonObject *alert, gchar *out, gsize out_sz)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    if (alert) {
        if (json_object_has_member(alert, "signature_id")) {
            json_builder_set_member_name(b, "sid");
            json_builder_add_int_value(b, _get_int_member_safe(alert, "signature_id", 0));
        }
        if (json_object_has_member(alert, "rev")) {
            json_builder_set_member_name(b, "rev");
            json_builder_add_int_value(b, _get_int_member_safe(alert, "rev", 0));
        }
        if (json_object_has_member(alert, "category")) {
            json_builder_set_member_name(b, "category");
            json_builder_add_string_value(b, _get_str_member_safe(alert, "category", ""));
        }
        if (json_object_has_member(alert, "severity")) {
            json_builder_set_member_name(b, "severity");
            json_builder_add_int_value(b, _get_int_member_safe(alert, "severity", 0));
        }
    }
    if (json_object_has_member(eve, "proto")) {
        json_builder_set_member_name(b, "proto");
        json_builder_add_string_value(b, _get_str_member_safe(eve, "proto", ""));
    }
    if (json_object_has_member(eve, "src_port")) {
        json_builder_set_member_name(b, "src_port");
        json_builder_add_int_value(b, _get_int_member_safe(eve, "src_port", 0));
    }
    if (json_object_has_member(eve, "dest_port")) {
        json_builder_set_member_name(b, "dest_port");
        json_builder_add_int_value(b, _get_int_member_safe(eve, "dest_port", 0));
    }
    if (json_object_has_member(eve, "app_proto")) {
        json_builder_set_member_name(b, "app_proto");
        json_builder_add_string_value(b, _get_str_member_safe(eve, "app_proto", ""));
    }
    if (json_object_has_member(eve, "in_iface")) {
        json_builder_set_member_name(b, "in_iface");
        json_builder_add_string_value(b, _get_str_member_safe(eve, "in_iface", ""));
    }

    json_builder_end_object(b);

    JsonNode *root = json_builder_get_root(b);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *data = json_generator_to_data(gen, NULL);
    json_node_free(root);
    g_object_unref(gen);
    g_object_unref(b);

    g_strlcpy(out, data ? data : "{}", out_sz);
    g_free(data);
}

   
                                                                  
                                                    
                                                      
                                                           
                                                         
                           
                                                     
                                                      
                                                                  
                                                             
   
gboolean
pcv_suricata_eve_to_event(JsonObject *eve, PcvSecurityEvent *out, GError **error)
{
    if (!eve || !out) {
        g_set_error(error, suricata_eve_error_quark(), 1, "eve/out is NULL");
        return FALSE;
    }

    const gchar *etype = _get_str_member_safe(eve, "event_type", "");
    if (g_strcmp0(etype, "alert") != 0) {
                                                            
                            
        g_set_error(error, suricata_eve_error_quark(), 2,
                    "not an alert event (event_type=\"%s\")", etype);
        return FALSE;
    }

    memset(out, 0, sizeof *out);
    out->source = PCV_SECURITY_SOURCE_SURICATA;
    out->type = PCV_SECURITY_EVENT_NETWORK_THREAT;
    out->status = PCV_SECURITY_STATUS_OPEN;
                                                   
                                                       
    out->timestamp = g_get_real_time() / G_USEC_PER_SEC;

                                                 
                                                              
                                             
    JsonNode *alert_node = json_object_get_member(eve, "alert");
    JsonObject *alert = (alert_node && JSON_NODE_HOLDS_OBJECT(alert_node))
        ? json_node_get_object(alert_node)
        : NULL;

    out->severity = _eve_severity(alert);
    out->target_kind = PCV_SECURITY_TARGET_IP;

    const gchar *src_ip = _get_str_member_safe(eve, "src_ip", "?");
    const gchar *dest_ip = _get_str_member_safe(eve, "dest_ip", "?");
    g_snprintf(out->target, sizeof out->target, "%s->%s", src_ip, dest_ip);

                                                                 
                                                 
                                                  
                                          
    gint64 sid = _get_int_member_safe(alert, "signature_id", -1);
    if (sid >= 0) {
        g_snprintf(out->recommended_action, sizeof out->recommended_action,
                   "review sid:%" G_GINT64_FORMAT, sid);
    } else {
        g_strlcpy(out->recommended_action, "review sid:?", sizeof out->recommended_action);
    }

    const gchar *signature = _get_str_member_safe(alert, "signature", "?");
    g_snprintf(out->summary, sizeof out->summary, "Suricata: %s", signature);

    _eve_evidence_json(eve, alert, out->evidence_json, sizeof out->evidence_json);

    pcv_security_event_make_id(out, "suricata");
    return TRUE;
}

                                                                      

static struct {
    GMutex  mu;
    guint64 alerts_crit;
    guint64 alerts_warn;
    guint64 alerts_info;
    guint64 eve_dropped;
} G_stats;

  
                                                               
                                                     
   
static void
_stats_record_ingested(PcvSecuritySeverity sev)
{
    g_mutex_lock(&G_stats.mu);
    switch (sev) {
    case PCV_SECURITY_SEVERITY_CRIT: G_stats.alerts_crit++; break;
    case PCV_SECURITY_SEVERITY_WARN: G_stats.alerts_warn++; break;
    default:                         G_stats.alerts_info++; break;
    }
    g_mutex_unlock(&G_stats.mu);
}

  
                                                                   
                                              
   
static void
_stats_record_dropped(void)
{
    g_mutex_lock(&G_stats.mu);
    G_stats.eve_dropped++;
    g_mutex_unlock(&G_stats.mu);
}

   
                                                                
                                                   
                                                                        
                                                           
   
void
pcv_suricata_get_stats(guint64 *alerts_crit, guint64 *alerts_warn,
                        guint64 *alerts_info, guint64 *eve_dropped)
{
    g_mutex_lock(&G_stats.mu);
    if (alerts_crit) *alerts_crit = G_stats.alerts_crit;
    if (alerts_warn) *alerts_warn = G_stats.alerts_warn;
    if (alerts_info) *alerts_info = G_stats.alerts_info;
    if (eve_dropped) *eve_dropped = G_stats.eve_dropped;
    g_mutex_unlock(&G_stats.mu);
}

                                                                  

static struct {
    GThread  *thread;
    gboolean  running;
    gboolean  initialized;
    gboolean  absent_logged;                                       
} G_eve = {0};

                                               
static void _maybe_isolate(const PcvSecurityEvent *ev);

  
                                                                
                                                   
                                                  
                    
                                                     
                                                   
                 
   
static void
_eve_process_line(const gchar *line, PcvSuricataRateState *rate_st)
{
    if (!line || !*line)
        return;

    GError *err = NULL;
    JsonNode *node = json_from_string(line, &err);
    if (!node) {
        g_clear_error(&err);
        return;                              
    }
    if (!JSON_NODE_HOLDS_OBJECT(node)) {
        json_node_free(node);
        return;
    }

    JsonObject *eve = json_node_get_object(node);
    PcvSecurityEvent ev;
    if (!pcv_suricata_eve_to_event(eve, &ev, &err)) {
        g_clear_error(&err);                               
        json_node_free(node);
        return;
    }
    json_node_free(node);                                            

    gint64 now_usec = g_get_monotonic_time();
    if (!pcv_suricata_rate_allow(rate_st, now_usec)) {
        _stats_record_dropped();
        return;
    }

    GError *submit_err = NULL;
    if (!pcv_security_submit_event(&ev, &submit_err)) {
        PCV_LOG_WARN(SURICATA_LOG_DOM, "eve alert submit failed: %s",
                     submit_err ? submit_err->message : "unknown error");
        g_clear_error(&submit_err);
        return;
    }
    _stats_record_ingested(ev.severity);

                                                           
                                             
                                                                
    if (ev.severity == PCV_SECURITY_SEVERITY_CRIT) {
        _maybe_isolate(&ev);
    }
}

  
                                                             
                                                     
               
   
   
                                                      
                                                   
                                                                 
                                                            
                                                          
                                   
                                                                     
                                    
   
PcvSuricataLineFeedResult
pcv_suricata_eve_line_feed(GString *linebuf, gboolean *discarding,
                           guint64 *oversized_count, gchar c)
{
    if (c == '\n') {
        if (*discarding) {
            *discarding = FALSE;
            return PCV_SURICATA_LINE_FEED_DISCARDED;
        }
        return PCV_SURICATA_LINE_FEED_READY;                                           
    }

    if (*discarding) {
        return PCV_SURICATA_LINE_FEED_PENDING;                           
    }

    if ((gsize)linebuf->len >= PCV_SURICATA_EVE_MAX_LINE) {
        *discarding = TRUE;
        if (oversized_count) {
            (*oversized_count)++;
        }
        g_string_set_size(linebuf, 0);                     
        return PCV_SURICATA_LINE_FEED_CAP_EXCEEDED;
    }

    g_string_append_c(linebuf, c);
    return PCV_SURICATA_LINE_FEED_PENDING;
}

  
                                                         
                                                 
                                                        
                                             
                                                             
  
                                                        
                                                       
                              
   
static gpointer
_eve_thread(gpointer data)
{
    (void)data;

    FILE *fp = NULL;
    PcvSuricataRateState rate_st = {0};
    GString *linebuf = g_string_new(NULL);
    gboolean discarding = FALSE;
                                                               
                                               
                                                         
    guint64 oversized_count = 0;

    while (G_eve.running) {
        GStatBuf st;
        gboolean exists = (g_stat(PCV_SURICATA_EVE_PATH, &st) == 0);

        if (!exists) {
            if (fp) { fclose(fp); fp = NULL; }
            if (!G_eve.absent_logged) {
                G_eve.absent_logged = TRUE;
                PCV_LOG_INFO(SURICATA_LOG_DOM,
                    "eve.json not found (%s) — ingest degraded, retrying",
                    PCV_SURICATA_EVE_PATH);
            }
            g_usleep(SURICATA_EVE_POLL_INTERVAL_SEC * G_USEC_PER_SEC);
            continue;
        }

        if (!fp) {
            fp = fopen(PCV_SURICATA_EVE_PATH, "r");
            if (!fp) {
                g_usleep(SURICATA_EVE_POLL_INTERVAL_SEC * G_USEC_PER_SEC);
                continue;
            }
                                                          
                                                    
                                                   
            fseek(fp, 0, SEEK_END);
            g_string_set_size(linebuf, 0);
            discarding = FALSE;
        } else if (ftell(fp) > (glong)st.st_size) {
                                                           
                                                  
                                                       
            fclose(fp);
            fp = fopen(PCV_SURICATA_EVE_PATH, "r");
            if (!fp) {
                g_usleep(SURICATA_EVE_POLL_INTERVAL_SEC * G_USEC_PER_SEC);
                continue;
            }
            g_string_set_size(linebuf, 0);
            discarding = FALSE;
        }

        gint c;
        clearerr(fp);
        while ((c = fgetc(fp)) != EOF) {
            PcvSuricataLineFeedResult r =
                pcv_suricata_eve_line_feed(linebuf, &discarding, &oversized_count, (gchar)c);
            switch (r) {
            case PCV_SURICATA_LINE_FEED_READY:
                _eve_process_line(linebuf->str, &rate_st);
                g_string_set_size(linebuf, 0);
                break;
            case PCV_SURICATA_LINE_FEED_CAP_EXCEEDED:
                                                          
                                            
                if (oversized_count == 1 || oversized_count == 10 ||
                    oversized_count == 100 || oversized_count == 1000 ||
                    oversized_count == 10000 || oversized_count == 100000) {
                    PCV_LOG_WARN(SURICATA_LOG_DOM,
                        "eve.json line exceeds %d bytes — discarding until next "
                        "newline (count=%" G_GUINT64_FORMAT ", possible corruption/attack)",
                        PCV_SURICATA_EVE_MAX_LINE, oversized_count);
                }
                break;
            case PCV_SURICATA_LINE_FEED_DISCARDED:
            case PCV_SURICATA_LINE_FEED_PENDING:
            default:
                break;
            }
        }

        g_usleep(SURICATA_EVE_POLL_INTERVAL_SEC * G_USEC_PER_SEC);
    }

    if (fp) fclose(fp);
    g_string_free(linebuf, TRUE);
    PCV_LOG_INFO(SURICATA_LOG_DOM, "suricata eve tail thread stopped");
    return NULL;
}

   
                                                                        
                                               
                                                           
                                 
   
void
pcv_suricata_eve_tail_start(void)
{
    if (G_eve.initialized) {
        PCV_LOG_WARN(SURICATA_LOG_DOM,
            "pcv_suricata_eve_tail_start() called twice — ignoring (idempotent)");
        return;
    }
    (void)pcv_security_store_ensure_open();                                  
    G_eve.running = TRUE;
    G_eve.initialized = TRUE;
    G_eve.absent_logged = FALSE;
    G_eve.thread = g_thread_new("suricata-eve", _eve_thread, NULL);
    PCV_LOG_INFO(SURICATA_LOG_DOM,
        "suricata eve tail started (%ds poll interval)", SURICATA_EVE_POLL_INTERVAL_SEC);
}

   
                                                                  
                                              
   
void
pcv_suricata_eve_tail_stop(void)
{
    if (!G_eve.initialized)
        return;
    G_eve.running = FALSE;
    if (G_eve.thread) {
        g_thread_join(G_eve.thread);
        G_eve.thread = NULL;
    }
    G_eve.initialized = FALSE;
}

   
                                                              
                                        
   
gboolean
pcv_suricata_eve_tail_running(void)
{
    return G_eve.running;
}

                                                                     
                                                    
                                                                        

   
                                                                      
                                                    
                                              
                                                                 
                                
                                         
                                                                      
   
void
pcv_suricata_engine_status_cached(PcvSuricataState *state, gboolean *binary_present)
{
    PcvSuricataState s;

    g_mutex_lock(&G_probe_cache.mu);
    gboolean have = G_probe_cache.valid;
    s = G_probe_cache.state;
    g_mutex_unlock(&G_probe_cache.mu);

    if (!have) {
                                                              
                                                    
        s = pcv_suricata_probe();
        _probe_cache_store(s);
    }

    if (state) *state = s;
                                                    
    if (binary_present) *binary_present = (s != PCV_SURICATA_ABSENT);
}

                                                                  

                                                    
                                                
                                                 
  
                                                     
                                                  
                        
                                   
   
static guint64
_rules_count_file(const gchar *path)
{
    FILE *fp = g_fopen(path, "r");
    if (!fp)
        return 0;

    guint64 count = 0;
    gboolean line_counted = FALSE;                        
    gint c;
    while ((c = fgetc(fp)) != EOF) {
        if (c == '\n') {
            line_counted = FALSE;
            continue;
        }
        if (line_counted)
            continue;                              
        if (c == ' ' || c == '\t' || c == '\r') {
                                                      
            continue;
        }
                          
        line_counted = TRUE;
        if (c != '#')
            count++;                     
    }

    fclose(fp);
    return count;
}

   
                                                                     
                                                 
                                         
                             
                                                       
                                                 
                            
   
guint64
pcv_suricata_rules_count_cached(const gchar *path,
                                PcvSuricataRulesCountCache *cache,
                                gboolean *recomputed)
{
    if (recomputed) *recomputed = FALSE;
    if (!cache)
        return _rules_count_file(path);

    GStatBuf st;
    gboolean exists = (g_stat(path, &st) == 0);

    gint64 mtime = exists ? (gint64)st.st_mtime : 0;
    gint64 size  = exists ? (gint64)st.st_size  : 0;

    if (cache->valid && cache->mtime == mtime && cache->size == size) {
        return cache->count;                      
    }

                            
    guint64 count = exists ? _rules_count_file(path) : 0;
    cache->mtime = mtime;
    cache->size  = size;
    cache->count = count;
    cache->valid = TRUE;
    if (recomputed) *recomputed = TRUE;
    return count;
}

   
                                                              
                                                     
                                               
   
guint64
pcv_suricata_rules_count(void)
{
    static GMutex mu;
    static PcvSuricataRulesCountCache cache;                        
    g_mutex_lock(&mu);
    guint64 n = pcv_suricata_rules_count_cached(PCV_SURICATA_RULES_PATH, &cache, NULL);
    g_mutex_unlock(&mu);
    return n;
}

                                                                      

   
                                                                   
                                                  
                                   
                                                     
   
void
pcv_suricata_metrics_tick(void)
{
                                                               
                                                        
                                                                

                                                         
    PcvSuricataState state;
    pcv_suricata_engine_status_cached(&state, NULL);
    pcv_prom_gauge_set_labels("purecvisor_suricata_engine_up", "",
                              state == PCV_SURICATA_ACTIVE ? 1.0 : 0.0);

                                                          
                                                    
                   
    guint64 crit = 0, warn = 0, info = 0, dropped = 0;
    pcv_suricata_get_stats(&crit, &warn, &info, &dropped);
    pcv_prom_gauge_set_labels("purecvisor_suricata_alerts_total",
                              "severity=\"crit\"", (gdouble)crit);
    pcv_prom_gauge_set_labels("purecvisor_suricata_alerts_total",
                              "severity=\"warn\"", (gdouble)warn);
    pcv_prom_gauge_set_labels("purecvisor_suricata_alerts_total",
                              "severity=\"info\"", (gdouble)info);
    pcv_prom_gauge_set_labels("purecvisor_suricata_eve_dropped_total", "",
                              (gdouble)dropped);

                                          
    pcv_prom_gauge_set_labels("purecvisor_suricata_rules_count", "",
                              (gdouble)pcv_suricata_rules_count());
}

                                                                     
                                         
                                                                        

static GQuark
suricata_policy_error_quark(void)
{
    return g_quark_from_static_string("pcv-suricata-policy");
}

  
                                                                        
                                                
   
static void
_tenant_policy_free(gpointer data)
{
    PcvSuricataTenantPolicy *tp = data;
    if (!tp) return;
    g_free(tp->profile);
    g_free(tp);
}

  
                                                                 
                                  
                                               
   
static GHashTable *
_drop_sids_new(void)
{
    return g_hash_table_new(g_direct_hash, g_direct_equal);
}

  
                                                         
                                            
                                                      
                                                       
   
static GHashTable *
_drop_sids_copy(GHashTable *src)
{
    GHashTable *dst = _drop_sids_new();
    if (!src)
        return dst;
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, src);
    while (g_hash_table_iter_next(&it, &key, NULL))
        g_hash_table_add(dst, key);
    return dst;
}

  
                                                                      
                                 
                                                    
   
static gint
_sid_ptr_cmp(gconstpointer a, gconstpointer b)
{
    guint x = GPOINTER_TO_UINT(a);
    guint y = GPOINTER_TO_UINT(b);
    return (x > y) - (x < y);
}

   
                                                                   
                                                 
                                                
   
PcvSuricataPolicy *
pcv_suricata_policy_new(void)
{
    PcvSuricataPolicy *p = g_new0(PcvSuricataPolicy, 1);
    p->enforce = FALSE;                   
    p->tenants = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _tenant_policy_free);
    p->drop_sids = _drop_sids_new();                              
    return p;
}

   
                                                          
                               
   
void
pcv_suricata_policy_free(PcvSuricataPolicy *p)
{
    if (!p) return;
    if (p->tenants) g_hash_table_destroy(p->tenants);
    if (p->drop_sids) g_hash_table_destroy(p->drop_sids);
    g_free(p);
}

   
                                                              
                                                  
                                        
                                                    
                                                                       
                                       
                                                          
   
PcvSuricataPolicy *
pcv_suricata_policy_load_file(const gchar *path)
{
    PcvSuricataPolicy *p = pcv_suricata_policy_new();
    if (!path)
        return p;

    gchar *data = NULL;
    if (!g_file_get_contents(path, &data, NULL, NULL)) {
                               
        return p;
    }

    JsonNode *root = json_from_string(data, NULL);
    g_free(data);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
                                   
        if (root) json_node_free(root);
        return p;
    }

    JsonObject *o = json_node_get_object(root);

                                                                        
    const gchar *ai = _get_str_member_safe(o, "auto_isolate", "dry_run");
    p->enforce = (g_strcmp0(ai, "enforce") == 0);

    JsonNode *tnode = json_object_get_member(o, "tenants");
    if (tnode && JSON_NODE_HOLDS_OBJECT(tnode)) {
        JsonObject *tenants = json_node_get_object(tnode);
        GList *names = json_object_get_members(tenants);
        for (GList *l = names; l; l = l->next) {
            const gchar *name = l->data;
            JsonNode *vn = json_object_get_member(tenants, name);
            if (!vn || !JSON_NODE_HOLDS_OBJECT(vn))
                continue;                  
            JsonObject *tv = json_node_get_object(vn);

            PcvSuricataTenantPolicy *tp = g_new0(PcvSuricataTenantPolicy, 1);
                                                      
            JsonNode *in = json_object_get_member(tv, "inspect");
            tp->inspect = (in && JSON_NODE_HOLDS_VALUE(in) &&
                           json_node_get_value_type(in) == G_TYPE_BOOLEAN)
                          ? json_node_get_boolean(in) : TRUE;
            tp->profile = g_strdup(_get_str_member_safe(tv, "profile", "default"));
            g_hash_table_replace(p->tenants, g_strdup(name), tp);
        }
        g_list_free(names);
    }

                                                    
                                               
                                                
                                                    
                           
    JsonNode *dnode = json_object_get_member(o, "drop_sids");
    if (dnode && JSON_NODE_HOLDS_ARRAY(dnode)) {
        JsonArray *arr = json_node_get_array(dnode);
        guint len = json_array_get_length(arr);
        for (guint i = 0; i < len; i++) {
            JsonNode *en = json_array_get_element(arr, i);
            if (!en || !JSON_NODE_HOLDS_VALUE(en) ||
                json_node_get_value_type(en) != G_TYPE_INT64)
                continue;
            gint64 v = json_node_get_int(en);
            if (v <= 0 || v > (gint64)G_MAXUINT)
                continue;
            g_hash_table_add(p->drop_sids, GUINT_TO_POINTER((guint)v));
        }
    }

    json_node_free(root);
    return p;
}

   
                                                              
                                                 
                                          
                                                    
                                                     
                                       
   
gboolean
pcv_suricata_policy_save_file(const PcvSuricataPolicy *p, const gchar *path, GError **error)
{
    if (!p || !path) {
        g_set_error(error, suricata_policy_error_quark(), 1, "policy/path is NULL");
        return FALSE;
    }

    JsonObject *root = pcv_suricata_policy_to_json(p, NULL);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, root);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, node);
    json_generator_set_pretty(gen, TRUE);
    gchar *out = json_generator_to_data(gen, NULL);
    json_node_free(node);
    g_object_unref(gen);

                                               
    gchar *tmp = g_strconcat(path, ".tmp", NULL);
    gboolean ok = g_file_set_contents(tmp, out ? out : "{}", -1, error);
    if (ok) {
        if (g_rename(tmp, path) != 0) {
            g_set_error(error, suricata_policy_error_quark(), 2,
                        "rename('%s' -> '%s') failed: %s", tmp, path, g_strerror(errno));
            g_unlink(tmp);                           
            ok = FALSE;
        }
    } else {
        g_unlink(tmp);                           
    }

    g_free(out);
    g_free(tmp);
    return ok;
}

   
                                                                 
                                                    
                                      
                                                                     
                                                                        
                                                                  
                                              
   
gboolean
pcv_suricata_policy_apply(PcvSuricataPolicy *p, const gchar *tenant,
                          const gboolean *inspect, const gchar *profile,
                          const gchar *auto_isolate, GError **error)
{
    if (!p) {
        g_set_error(error, suricata_policy_error_quark(), 1, "policy is NULL");
        return FALSE;
    }

                                                 
    if (!auto_isolate && !tenant) {
        g_set_error(error, suricata_policy_error_quark(), 2,
                    "policy.set requires at least one of {auto_isolate, tenant}");
        return FALSE;
    }

                                            
    if ((inspect || profile) && !tenant) {
        g_set_error(error, suricata_policy_error_quark(), 3,
                    "inspect/profile require a tenant target");
        return FALSE;
    }

    if (auto_isolate) {
        if (g_strcmp0(auto_isolate, "enforce") == 0) {
            p->enforce = TRUE;
        } else if (g_strcmp0(auto_isolate, "dry_run") == 0) {
            p->enforce = FALSE;
        } else {
            g_set_error(error, suricata_policy_error_quark(), 4,
                        "auto_isolate must be \"dry_run\" or \"enforce\" (got \"%s\")",
                        auto_isolate);
            return FALSE;
        }
    }

    if (tenant) {
        PcvSuricataTenantPolicy *tp = g_hash_table_lookup(p->tenants, tenant);
        if (!tp) {
                                                              
            tp = g_new0(PcvSuricataTenantPolicy, 1);
            tp->inspect = TRUE;
            tp->profile = g_strdup("default");
            g_hash_table_replace(p->tenants, g_strdup(tenant), tp);
        }
        if (inspect) tp->inspect = *inspect;
        if (profile) {
            g_free(tp->profile);
            tp->profile = g_strdup(profile);
        }
    }

    return TRUE;
}

   
                                                                  
                                                         
                                          
                                               
                                                             
                                                                  
   
JsonObject *
pcv_suricata_policy_to_json(const PcvSuricataPolicy *p, const gchar *tenant)
{
    JsonObject *out = json_object_new();
    if (!p)
        return out;

    if (tenant) {
                                                                 
        PcvSuricataTenantPolicy *tp = g_hash_table_lookup(p->tenants, tenant);
        json_object_set_string_member(out, "tenant", tenant);
        json_object_set_boolean_member(out, "inspect", tp ? tp->inspect : TRUE);
        json_object_set_string_member(out, "profile",
                                      tp && tp->profile ? tp->profile : "default");
        return out;
    }

                                             
    json_object_set_string_member(out, "auto_isolate", p->enforce ? "enforce" : "dry_run");
    JsonObject *tenants = json_object_new();
    if (p->tenants) {
        GList *names = g_hash_table_get_keys(p->tenants);
        names = g_list_sort(names, (GCompareFunc)g_strcmp0);               
        for (GList *l = names; l; l = l->next) {
            const gchar *name = l->data;
            PcvSuricataTenantPolicy *tp = g_hash_table_lookup(p->tenants, name);
            JsonObject *tv = json_object_new();
            json_object_set_boolean_member(tv, "inspect", tp ? tp->inspect : TRUE);
            json_object_set_string_member(tv, "profile",
                                          tp && tp->profile ? tp->profile : "default");
            json_object_set_object_member(tenants, name, tv);
        }
        g_list_free(names);
    }
    json_object_set_object_member(out, "tenants", tenants);

                                                         
                                                   
    JsonArray *sids = json_array_new();
    if (p->drop_sids) {
        GList *keys = g_hash_table_get_keys(p->drop_sids);
        keys = g_list_sort(keys, _sid_ptr_cmp);
        for (GList *l = keys; l; l = l->next)
            json_array_add_int_element(sids, (gint64)GPOINTER_TO_UINT(l->data));
        g_list_free(keys);
    }
    json_object_set_array_member(out, "drop_sids", sids);
    return out;
}

   
                                                                   
                                                   
                                                  
                                                   
                                      
                                                                 
                                                         
   
gboolean
pcv_suricata_policy_set_drop_sids(PcvSuricataPolicy *p, JsonArray *sids,
                                  gboolean add, GError **error)
{
    if (!p) {
        g_set_error(error, suricata_policy_error_quark(), 1, "policy is NULL");
        return FALSE;
    }
    if (!sids) {
        g_set_error(error, suricata_policy_error_quark(), 5, "sids array is required");
        return FALSE;
    }

    guint len = json_array_get_length(sids);
    if (len == 0) {
                                             
                                                           
        g_set_error(error, suricata_policy_error_quark(), 6,
                    "sids array must not be empty");
        return FALSE;
    }

                                             
    for (guint i = 0; i < len; i++) {
        JsonNode *en = json_array_get_element(sids, i);
        if (!en || !JSON_NODE_HOLDS_VALUE(en) ||
            json_node_get_value_type(en) != G_TYPE_INT64) {
            g_set_error(error, suricata_policy_error_quark(), 7,
                        "sids[%u] must be an integer", i);
            return FALSE;
        }
        gint64 v = json_node_get_int(en);
        if (v <= 0 || v > (gint64)G_MAXUINT) {
            g_set_error(error, suricata_policy_error_quark(), 8,
                        "sids[%u] out of range (1..%u): %" G_GINT64_FORMAT,
                        i, G_MAXUINT, v);
            return FALSE;
        }
    }

    if (!p->drop_sids)
        p->drop_sids = _drop_sids_new();                             

                  
    for (guint i = 0; i < len; i++) {
        guint sid = (guint)json_node_get_int(json_array_get_element(sids, i));
        if (add)
            g_hash_table_add(p->drop_sids, GUINT_TO_POINTER(sid));
        else
            g_hash_table_remove(p->drop_sids, GUINT_TO_POINTER(sid));
    }
    return TRUE;
}

   
                                                                  
                                                
                                                   
   
GHashTable *
pcv_suricata_policy_drop_sids(const PcvSuricataPolicy *p)
{
    return p ? p->drop_sids : NULL;
}

                                          

static struct {
    GMutex             mu;
    PcvSuricataPolicy *loaded;                       
} G_policy;

  
                                                                     
                                                
                                                          
   
static PcvSuricataPolicy *
_policy_ensure_loaded_locked(void)
{
    if (!G_policy.loaded)
        G_policy.loaded = pcv_suricata_policy_load_file(PCV_SURICATA_POLICY_PATH);
    return G_policy.loaded;
}

   
                                                             
                                                      
                                                               
                                                             
   
gboolean
pcv_suricata_policy_set(const gchar *tenant, const gboolean *inspect,
                        const gchar *profile, const gchar *auto_isolate,
                        GError **error)
{
    g_mutex_lock(&G_policy.mu);
    PcvSuricataPolicy *p = _policy_ensure_loaded_locked();
    gboolean ok = pcv_suricata_policy_apply(p, tenant, inspect, profile, auto_isolate, error);
    if (ok)
        ok = pcv_suricata_policy_save_file(p, PCV_SURICATA_POLICY_PATH, error);
    g_mutex_unlock(&G_policy.mu);
    return ok;
}

   
                                                                    
                                                   
                               
   
JsonObject *
pcv_suricata_policy_get_json(const gchar *tenant)
{
    g_mutex_lock(&G_policy.mu);
    PcvSuricataPolicy *p = _policy_ensure_loaded_locked();
    JsonObject *j = pcv_suricata_policy_to_json(p, tenant);
    g_mutex_unlock(&G_policy.mu);
    return j;
}

   
                                                                   
                                                    
                                                        
                                              
   
void
pcv_suricata_policy_summary(gboolean *enforce, guint *tenant_count)
{
    g_mutex_lock(&G_policy.mu);
    PcvSuricataPolicy *p = _policy_ensure_loaded_locked();
    if (enforce)      *enforce = p->enforce;
    if (tenant_count) *tenant_count = p->tenants ? g_hash_table_size(p->tenants) : 0;
    g_mutex_unlock(&G_policy.mu);
}

                                                               
                                                   
                                                  
                                                          
                                                                        

   
                                                                          
                                                   
                                                             
                                                                      
   
gboolean
pcv_suricata_policy_set_drop_sids_global(JsonArray *sids, gboolean add, GError **error)
{
    g_mutex_lock(&G_policy.mu);
    PcvSuricataPolicy *p = _policy_ensure_loaded_locked();
    gboolean ok = pcv_suricata_policy_set_drop_sids(p, sids, add, error);
    g_mutex_unlock(&G_policy.mu);
    return ok;
}

   
                                                                       
                                                  
                                       
                                                       
   
GHashTable *
pcv_suricata_policy_drop_sids_snapshot(void)
{
    g_mutex_lock(&G_policy.mu);
    PcvSuricataPolicy *p = _policy_ensure_loaded_locked();
    GHashTable *copy = _drop_sids_copy(pcv_suricata_policy_drop_sids(p));
    g_mutex_unlock(&G_policy.mu);
    return copy;
}

   
                                                                    
                                                  
                                
   
gboolean
pcv_suricata_policy_drop_sids_commit(GError **error)
{
    g_mutex_lock(&G_policy.mu);
    PcvSuricataPolicy *p = _policy_ensure_loaded_locked();
    gboolean ok = pcv_suricata_policy_save_file(p, PCV_SURICATA_POLICY_PATH, error);
    g_mutex_unlock(&G_policy.mu);
    return ok;
}

   
                                                                          
                                           
                                                        
                                     
   
void
pcv_suricata_policy_drop_sids_rollback(GHashTable *prev)
{
    g_mutex_lock(&G_policy.mu);
    PcvSuricataPolicy *p = _policy_ensure_loaded_locked();
    if (p->drop_sids)
        g_hash_table_destroy(p->drop_sids);
    p->drop_sids = _drop_sids_copy(prev);
    g_mutex_unlock(&G_policy.mu);
}

                                                                     
                                       
                                                                        

   
                                                                      
                                                   
                                                             
                                                            
   
gboolean
pcv_suricata_target_src_ip(const gchar *target, gchar *out, gsize out_sz)
{
    if (!target || !out || out_sz == 0)
        return FALSE;

    const gchar *arrow = strstr(target, "->");
    if (!arrow)
        return FALSE;

    gsize len = (gsize)(arrow - target);
    if (len >= out_sz)
        len = out_sz - 1;                        
    memcpy(out, target, len);
    out[len] = '\0';
    return TRUE;
}

   
                                                              
                                                         
                                                    
            
                                                                       
                                                             
                                          
                                                 
                                                            
                                                                          
   
PcvSuricataIsolateDecision
pcv_suricata_isolate_decide(gboolean enforce, const gchar *src_ip, GHashTable *actioned)
{
                                                      
                                                                   
                                                      
    const gchar *argv[16] = {0};
    if (!src_ip || !pcv_hips_action_build_block_ip_argv(src_ip, argv, G_N_ELEMENTS(argv)))
        return PCV_SURICATA_ISOLATE_SKIP_INVALID;

                                                        
    if (actioned && g_hash_table_contains(actioned, src_ip))
        return PCV_SURICATA_ISOLATE_SKIP_ACTIONED;

                                                    
    if (actioned && g_hash_table_size(actioned) >= PCV_SURICATA_ISOLATE_CAP)
        return PCV_SURICATA_ISOLATE_SKIP_CAP;

                           
    return enforce ? PCV_SURICATA_ISOLATE_ENFORCE : PCV_SURICATA_ISOLATE_DRY_RUN;
}

                                                    
                                               
static struct {
    GMutex      mu;
    GHashTable *actioned;
    guint64     cap_skipped;                               
} G_isolate;

   
                                                           
                                                   
                                                  
                                           
                                                         
                                                
                                                                     
                                           
                                                    
   
static void
_maybe_isolate(const PcvSecurityEvent *ev)
{
    if (!ev) return;

    gchar src[64];
    if (!pcv_suricata_target_src_ip(ev->target, src, sizeof src)) {
                                                               
        PCV_LOG_DEBUG(SURICATA_LOG_DOM,
                      "isolate skip: cannot parse src from target '%s'", ev->target);
        return;
    }

    gboolean enforce = FALSE;
    pcv_suricata_policy_summary(&enforce, NULL);                        

    g_mutex_lock(&G_isolate.mu);
    if (!G_isolate.actioned)
        G_isolate.actioned = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    PcvSuricataIsolateDecision d =
        pcv_suricata_isolate_decide(enforce, src, G_isolate.actioned);

                                                   
                                                          
    gboolean record = (d == PCV_SURICATA_ISOLATE_DRY_RUN ||
                       d == PCV_SURICATA_ISOLATE_ENFORCE);
    if (record)
        g_hash_table_add(G_isolate.actioned, g_strdup(src));
    if (d == PCV_SURICATA_ISOLATE_SKIP_CAP)
        G_isolate.cap_skipped++;
    guint64 cap_skipped = G_isolate.cap_skipped;
    g_mutex_unlock(&G_isolate.mu);

    switch (d) {
    case PCV_SURICATA_ISOLATE_DRY_RUN:
                                                            
        PCV_LOG_INFO(SURICATA_LOG_DOM,
                     "would isolate %s (enforce 미승격 — dry_run)", src);
        pcv_audit_log("system", "suricata.isolate.dry_run", src, "ok", 0, 0, "local");
        break;

    case PCV_SURICATA_ISOLATE_ENFORCE: {
                                                                 
                                                           
                                                          
        GError *e = NULL;
        gboolean ok = pcv_hips_action_execute("block_ip", src, &e);
        if (ok) {
            PCV_LOG_WARN(SURICATA_LOG_DOM, "isolated %s via HIPS block_ip (enforce)", src);
        } else {
            PCV_LOG_ERROR(SURICATA_LOG_DOM, "isolate %s failed: %s",
                          src, e ? e->message : "unknown error");
        }
        pcv_audit_log("system", "suricata.isolate", src, ok ? "ok" : "fail",
                      ok ? 0 : (e ? e->code : -1), 0, "local");
        g_clear_error(&e);
        break;
    }

    case PCV_SURICATA_ISOLATE_SKIP_INVALID:
        PCV_LOG_DEBUG(SURICATA_LOG_DOM,
                      "isolate skip: src '%s' not an IPv4 literal (block_ip IPv4-only)", src);
        break;

    case PCV_SURICATA_ISOLATE_SKIP_ACTIONED:
                                     
        break;

    case PCV_SURICATA_ISOLATE_SKIP_CAP:
        if (cap_skipped == 1 || cap_skipped == 100 || cap_skipped == 10000) {
            PCV_LOG_WARN(SURICATA_LOG_DOM,
                         "isolate cap (%d) reached — skipping new src %s "
                         "(cap_skipped=%" G_GUINT64_FORMAT ")",
                         PCV_SURICATA_ISOLATE_CAP, src, cap_skipped);
        }
        break;

    default:
        break;
    }
}
