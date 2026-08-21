   
                               
                                                           
  
                           
                                                   
                                                    
                                        
  
                                             
                                               
  
                                                       
                                                  
                                                    
   
#include "daemon_config_policy.h"

#include <errno.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "utils/pcv_config.h"
#include "utils/pcv_secure.h"

static GMutex g_daemon_config_set_mu;

                                                        
                                      
extern void pcv_test_daemon_config_publish_hook(void)
    __attribute__((weak));

                                               
                                                    
                                                          
                                                           
                                
gboolean
pcv_daemon_config_set_alert_reload_is_fatal(
    const gchar *section, PcvAlertConfigSetResult reload_result)
{
    return g_strcmp0(section, "alert") == 0
        && reload_result != PCV_ALERT_CONFIG_SET_OK;
}

   
                                                                 
  
                                                             
                                               
                           
  
                                                       
                                                        
                                                             
                      
                                                   
                                                              
                                                               
                                                             
   
static void
_propagate_or_clear(GError **error, GError *local_error)
{
    if (!local_error)
        return;
    if (error)
        g_propagate_error(error, local_error);
    else
        g_error_free(local_error);
}

   
                                                                  
  
                                                  
                                                
                                               
                                            
  
                                                                     
                           
                                                         
                                                       
                    
                                                                        
                                                             
                                             
                                                          
                                                                
                                                  
  
                                                                   
                                                        
                                                     
                                                                   
                                                         
                                                       
                                                           
                                                           
                                                           
                                                          
             
   
PcvAlertConfigSetResult
pcv_daemon_config_set_value(const gchar *conf_path,
                            const gchar *section,
                            const gchar *key,
                            const gchar *value,
                            GError **error)
{
    if (!conf_path || !section || !key || !value) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                            "daemon config path, section, key and value are required");
        return PCV_ALERT_CONFIG_SET_INVALID;
    }

    g_mutex_lock(&g_daemon_config_set_mu);

    PcvAlertConfigSetResult result = PCV_ALERT_CONFIG_SET_INVALID;
    gchar *original = NULL;
    gsize original_len = 0;
    gboolean had_original = FALSE;
    gchar *candidate_data = NULL;
    gsize candidate_len = 0;
    GKeyFile *key_file = g_key_file_new();
    GError *local_error = NULL;
    gboolean previous_source_valid = TRUE;
    gchar *previous_source_error = NULL;

    had_original = g_file_get_contents(
        conf_path, &original, &original_len, &local_error);
    if (!had_original) {
        if (!g_error_matches(local_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            goto out;
        g_clear_error(&local_error);
    } else if (!g_key_file_load_from_data(
                   key_file, original, original_len,
                   G_KEY_FILE_KEEP_COMMENTS, &local_error)) {
        goto out;
    }

    if (g_strcmp0(section, "alert") == 0) {
        JsonObject *before = pcv_alert_engine_get_config();
        previous_source_valid = json_object_get_boolean_member(
            before, "daemon_config_valid");
        previous_source_error = g_strdup(json_object_get_string_member(
            before, "daemon_config_error"));
        json_object_unref(before);
    }

    g_key_file_set_string(key_file, section, key, value);
    candidate_data = g_key_file_to_data(
        key_file, &candidate_len, &local_error);
    if (!candidate_data)
        goto out;
    if (g_strcmp0(section, "alert") == 0
        && !pcv_alert_engine_validate_daemon_config_data(
            candidate_data, candidate_len)) {
          
                                                             
                                                           
                                      
           
        result = PCV_ALERT_CONFIG_SET_INVALID;
        goto out;
    }
    if (pcv_test_daemon_config_publish_hook)
        pcv_test_daemon_config_publish_hook();
    if (!g_file_set_contents(
            conf_path, candidate_data, (gssize)candidate_len, &local_error)) {
        goto out;
    }

    PcvAlertConfigSetResult reload_result =
        pcv_alert_engine_reload_daemon_config();
    if (!pcv_daemon_config_set_alert_reload_is_fatal(
            section, reload_result)) {
        result = PCV_ALERT_CONFIG_SET_OK;
        goto out;
    }

      
                                                                   
                                                      
                          
       
    if (had_original) {
        if (!g_file_set_contents(
                conf_path, original, (gssize)original_len, &local_error)) {
            goto out;
        }
    } else if (g_remove(conf_path) != 0 && errno != ENOENT) {
        g_set_error(&local_error, G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "failed to remove rolled-back daemon config '%s': %s",
                    conf_path, g_strerror(errno));
        goto out;
    }

    if (!pcv_config_reload()) {
        g_set_error_literal(
            &local_error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
            "failed to reload the restored daemon config");
        goto out;
    }
    pcv_alert_engine_restore_daemon_source_status(
        previous_source_valid, previous_source_error);
    result = PCV_ALERT_CONFIG_SET_INVALID;

out:
    if (candidate_data) {
        pcv_secure_wipe(candidate_data, candidate_len);
        g_free(candidate_data);
    }
    if (original) {
        pcv_secure_wipe(original, original_len);
        g_free(original);
    }
    g_free(previous_source_error);
    g_key_file_free(key_file);
    _propagate_or_clear(error, local_error);
    g_mutex_unlock(&g_daemon_config_set_mu);
    return result;
}
