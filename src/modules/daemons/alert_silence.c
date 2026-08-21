                                      
  
                                                    
  
                           
                                                   
                                                    
                                        
  
                                                   
                                                        
                                                      
  
                                                                
                                                                 
                                                           
                                    
                                                       
                                                               
                                                    
                                              
                                                    
                                        
  
                                                      
                                               
                                                
                                                                    
                                                                     
                                                        
                                                 
   
#include "alert_silence.h"
#include "utils/pcv_log.h"

#include <glib.h>

#define ALERT_SILENCE_LOG_DOM "alert_engine"

                                           
typedef struct {
    gchar  *metric;                                     
    gint64  until;                                      
    gchar  *reason;                   
} AlertSilence;

static GPtrArray *g_silences = nullptr;
static GMutex     g_silence_mu;
                                                       
                                                    
                                                            
static gsize      g_silence_once = 0;

                                                                 
                                                        
                                                                    
                                                               
                                                       
                                                        
                                  
                                                           
                                                                
static void
_silence_free(gpointer p)
{
    AlertSilence *s = p;
    g_free(s->metric);
    g_free(s->reason);
    g_free(s);
}

                                      
                                           
                                                                    
static void
_ensure_silence_init(void)
{
    if (g_once_init_enter(&g_silence_once)) {                                      
        g_mutex_init(&g_silence_mu);
        g_silences = g_ptr_array_new_with_free_func(_silence_free);
        g_once_init_leave(&g_silence_once, 1);                          
    }
}

                                                
void
pcv_alert_add_silence(const gchar *metric, gint duration_min, const gchar *reason)
{
    _ensure_silence_init();
    AlertSilence *s = g_new0(AlertSilence, 1);
    s->metric = g_strdup(metric);                                 
                                                                  
                                          
    s->until  = g_get_monotonic_time() + (gint64)duration_min * 60 * G_USEC_PER_SEC;
    s->reason = g_strdup(reason ? reason : "");                            

    g_mutex_lock(&g_silence_mu);                              
    g_ptr_array_add(g_silences, s);
    g_mutex_unlock(&g_silence_mu);

    PCV_LOG_INFO(ALERT_SILENCE_LOG_DOM, "Alert silenced: metric=%s duration=%dmin reason=%s",
                 metric, duration_min, reason ? reason : "");
}

                                                       
gboolean
pcv_alert_is_silenced(const gchar *metric)
{
    _ensure_silence_init();
    if (!metric) return FALSE;                                        
    gint64 now = g_get_monotonic_time();                                             
    gboolean silenced = FALSE;

    g_mutex_lock(&g_silence_mu);
                                                 
                                                
    for (guint i = 0; i < g_silences->len; i++) {
        AlertSilence *s = g_ptr_array_index(g_silences, i);
                                                                   
                                                                  
        if (s->metric && g_ascii_strcasecmp(s->metric, metric) == 0 && now < s->until) {
            silenced = TRUE;
            break;
        }
    }
    g_mutex_unlock(&g_silence_mu);
    return silenced;
}

                                                              
JsonArray *
pcv_alert_get_silences(void)
{
    _ensure_silence_init();
    JsonArray *arr = json_array_new();
    gint64 now = g_get_monotonic_time();

    g_mutex_lock(&g_silence_mu);
    for (guint i = 0; i < g_silences->len; i++) {
        AlertSilence *s = g_ptr_array_index(g_silences, i);
        if (now < s->until) {                               
            JsonObject *obj = json_object_new();
            json_object_set_string_member(obj, "metric", s->metric);
            json_object_set_int_member(obj, "remaining_sec",
                (gint64)((s->until - now) / G_USEC_PER_SEC));
            json_object_set_string_member(obj, "reason", s->reason);
            json_array_add_object_element(arr, obj);
        }
    }
    g_mutex_unlock(&g_silence_mu);
    return arr;
}

                                                 
                                                                    
                                                       
                                                             
                                                   
                        
void
pcv_alert_silence_reset(void)
{
    _ensure_silence_init();
    g_mutex_lock(&g_silence_mu);
    g_ptr_array_set_size(g_silences, 0);                                       
    g_mutex_unlock(&g_silence_mu);
}
