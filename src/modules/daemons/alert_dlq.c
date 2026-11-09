                                  
  
                                               
  
                           
                                                   
                                                    
                                        
  
                                                   
                                                  
                                                      
                                          
  
                                                                
                                                                       
                                                          
                                                     
                                                                    
                                                       
                                                
                                                    
  
                                                  
                                                                
                                                                   
                                                                   
                                                       
   
#include "alert_dlq.h"
#include "utils/pcv_log.h"
#include "api/drain.h"

#include <glib.h>
#include <string.h>

#define ALERT_DLQ_LOG_DOM "alert_engine"

                                                      
constexpr int WEBHOOK_DLQ_MAX = 1000;
static_assert(WEBHOOK_DLQ_MAX >= 100, "DLQ buffer too small");

static GPtrArray  *g_webhook_dlq = nullptr;
static GMutex      g_dlq_mu;                                             
static GMutex      g_dlq_retry_mu;                                                    
static PcvDlqPostFn g_post_fn = nullptr;                                              

                                                                                         
                                                                                   
#define DLQ_RETRY_MAX_ITEMS  100
#define DLQ_RETRY_MAX_USEC   (60 * G_USEC_PER_SEC)
static gint64   (*g_now_fn)(void) = nullptr;
static gboolean (*g_terminating_fn)(void) = nullptr;

                                                   
static gint64
_dlq_now(void)
{
    return g_now_fn ? g_now_fn() : g_get_monotonic_time();
}

                                                      
static gboolean
_dlq_terminating(void)
{
    return g_terminating_fn ? g_terminating_fn() : pcv_drain_is_terminating();
}

                                                        
void
pcv_alert_dlq_set_post_fn(PcvDlqPostFn fn)
{
    g_post_fn = fn;
}

                                                           
void
pcv_alert_dlq_store(const gchar *url, const gchar *payload)
{
    g_mutex_lock(&g_dlq_mu);
    if (!g_webhook_dlq)                                             
        g_webhook_dlq = g_ptr_array_new_with_free_func(g_free);
    if (g_webhook_dlq->len < WEBHOOK_DLQ_MAX) {
                                                                     
        gchar *entry = g_strdup_printf("%s|%s", url, payload);
        g_ptr_array_add(g_webhook_dlq, entry);
        PCV_LOG_WARN(ALERT_DLQ_LOG_DOM, "Webhook DLQ stored (%u entries)", g_webhook_dlq->len);
    } else {
                                                       
        PCV_LOG_WARN(ALERT_DLQ_LOG_DOM, "Webhook DLQ full (%d), dropping entry", WEBHOOK_DLQ_MAX);
    }
    g_mutex_unlock(&g_dlq_mu);
}

                                                        
JsonArray *
pcv_alert_dlq_list(void)
{
    JsonArray *arr = json_array_new();
    g_mutex_lock(&g_dlq_mu);
    if (g_webhook_dlq) {
        for (guint i = 0; i < g_webhook_dlq->len; i++) {
            const gchar *entry = g_ptr_array_index(g_webhook_dlq, i);
                                                                            
            const gchar *sep = strchr(entry, '|');
            JsonObject *obj = json_object_new();
            if (sep) {                                        
                gchar *url = g_strndup(entry, (gsize)(sep - entry));
                json_object_set_string_member(obj, "url", url);
                json_object_set_string_member(obj, "payload", sep + 1);
                g_free(url);
            } else {
                                                                    
                json_object_set_string_member(obj, "url", "");
                json_object_set_string_member(obj, "payload", entry);
            }
            json_object_set_int_member(obj, "index", (gint64)i);                        
            json_array_add_object_element(arr, obj);
        }
    }
    g_mutex_unlock(&g_dlq_mu);
    return arr;
}

                                                             
GPtrArray *
pcv_alert_dlq_snapshot(void)
{
    GPtrArray *snap = g_ptr_array_new_with_free_func(g_free);
    g_mutex_lock(&g_dlq_mu);
    if (g_webhook_dlq) {
                                                      
                                                 
        for (guint i = 0; i < g_webhook_dlq->len; i++)
            g_ptr_array_add(snap, g_strdup(g_ptr_array_index(g_webhook_dlq, i)));
    }
    g_mutex_unlock(&g_dlq_mu);
    return snap;
}

                                                           
void
pcv_alert_dlq_remove_matching(GPtrArray *values)
{
    if (!values || values->len == 0) return;                       
    g_mutex_lock(&g_dlq_mu);
    if (g_webhook_dlq) {
        for (guint v = 0; v < values->len; v++) {
            const gchar *want = g_ptr_array_index(values, v);
                                                      
                                            
            for (guint i = 0; i < g_webhook_dlq->len; i++) {
                if (g_strcmp0(g_ptr_array_index(g_webhook_dlq, i), want) == 0) {
                    g_ptr_array_remove_index(g_webhook_dlq, i);
                    break;
                }
            }
        }
    }
    g_mutex_unlock(&g_dlq_mu);
}

                                           
                                                            
                                                          
                                                    
                                                                  
JsonObject *
pcv_alert_dlq_retry(void)
{
    JsonObject *result = json_object_new();
    gint retried = 0, succeeded = 0, failed = 0;                  
    gint64 started_us = _dlq_now();                                     
    gboolean capped = FALSE;

                                                               
                                                                
    g_mutex_lock(&g_dlq_retry_mu);

                                                             
    GPtrArray *snapshot   = pcv_alert_dlq_snapshot();
    GPtrArray *ok_entries = g_ptr_array_new_with_free_func(g_free);

                                                                  
    guint i = 0;
    for (; i < snapshot->len; i++) {
                                                               
        if (retried >= DLQ_RETRY_MAX_ITEMS || _dlq_now() - started_us >= DLQ_RETRY_MAX_USEC
            || _dlq_terminating()) {
            capped = TRUE;
            break;
        }
        const gchar *entry = g_ptr_array_index(snapshot, i);
        const gchar *sep = strchr(entry, '|');
        retried++;
        if (sep) {
            gchar *url = g_strndup(entry, (gsize)(sep - entry));
            gboolean ok = (g_post_fn != nullptr) && g_post_fn(url, sep + 1);
            g_free(url);
            if (ok) {
                succeeded++;
                g_ptr_array_add(ok_entries, g_strdup(entry));                     
            } else {
                failed++;
            }
        } else {
            failed++;
        }
    }

    gint remaining = (gint)(snapshot->len - i);                          

                                  
    pcv_alert_dlq_remove_matching(ok_entries);

    g_ptr_array_unref(snapshot);
    g_ptr_array_unref(ok_entries);

    g_mutex_unlock(&g_dlq_retry_mu);

    json_object_set_int_member(result, "retried", retried);
    json_object_set_int_member(result, "succeeded", succeeded);
    json_object_set_int_member(result, "failed", failed);
    json_object_set_int_member(result, "remaining", remaining);
    json_object_set_boolean_member(result, "capped", capped);
    PCV_LOG_INFO(ALERT_DLQ_LOG_DOM, "DLQ retry: %d retried, %d succeeded, %d failed, %d remaining%s",
                 retried, succeeded, failed, remaining, capped ? " (capped)" : "");
    return result;
}

                                                                
                                                                  
                                                          
                                               
                        
void
pcv_alert_dlq_reset(void)
{
    g_mutex_lock(&g_dlq_mu);
    if (g_webhook_dlq)
        g_ptr_array_set_size(g_webhook_dlq, 0);
    g_mutex_unlock(&g_dlq_mu);
}

                                                                        
                        
void
pcv_alert_dlq_set_now_fn_for_test(gint64 (*fn)(void))
{
    g_now_fn = fn;
}

                                                                                      
                        
void
pcv_alert_dlq_set_terminating_fn_for_test(gboolean (*fn)(void))
{
    g_terminating_fn = fn;
}
