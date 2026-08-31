   
                      
                                               
  
                           
                                                   
                                                    
                                        
  
          
                                             
                                                  
                                                 
                                                    
                                                   
                                              
                                   
  
                
                                                
                                                            
                                                    
  
                                                                       
            
                                             
                                                   
  
                                                       
                                                      
                                                    
                  
  
                         
                                             
                                                      
                                             
  
          
                                                       
                                     
                                        
  
         
                                                        
                                                         
                                                   
                                                             
                                                                       
   
#include "nfv_manager.h"
#include "utils/pcv_log.h"
#include <string.h>
#include <glib/gstdio.h>

#define NFV_LOG_DOM "nfv_manager"

   
                                         
void pcv_nfv_init(void) { PCV_LOG_INFO(NFV_LOG_DOM, "NFV manager initialized"); }

                                                
                                                        
void pcv_nfv_shutdown(void) {}

                                                                 
                          
                                        
                                             
                                                                    

   
                                    
                             
                                        
                        
                                                     
                    
  
                                              
                                 
  
                                                            
                       
                                                       
                                                  
                                                                         
   
gboolean pcv_nfv_fw_policy_create(const gchar *name, const gchar *sw, GError **error)
{
    if (!name || !sw) { g_set_error(error, g_quark_from_static_string("nfv"), 1, "name and switch required"); return FALSE; }
                                              
    gchar *path = g_strdup_printf("/var/run/purecvisor/nfv-policy-%s.json", name);
                                                    
    gchar *content = g_strdup_printf("{\"name\":\"%s\",\"switch\":\"%s\",\"rules\":[]}", name, sw);
    gboolean ok = g_file_set_contents(path, content, -1, error);
    g_free(content); g_free(path);
    if (ok) PCV_LOG_INFO(NFV_LOG_DOM, "FW policy '%s' created for switch '%s'", name, sw);
    return ok;
}

   
                                               
                   
                    
  
                                                   
  
                                                
  
                                                                  
   
gboolean pcv_nfv_fw_policy_delete(const gchar *name, GError **error)
{
    if (!name) { g_set_error(error, g_quark_from_static_string("nfv"), 1, "name required"); return FALSE; }
    gchar *path = g_strdup_printf("/var/run/purecvisor/nfv-policy-%s.json", name);
    g_unlink(path);                              
    g_free(path);
    return TRUE;
}

   
                                            
                               
  
                                              
                                               
  
                                                      
                               
  
                                                              
                                         
   
JsonArray *pcv_nfv_fw_policy_list(const gchar *sw __attribute__((unused)))
{
    JsonArray *arr = json_array_new();
    GDir *dir = g_dir_open("/var/run/purecvisor", 0, NULL);
    if (!dir) return arr;                                                  
    const gchar *name;
                           
    while ((name = g_dir_read_name(dir)) != NULL) {
                                                                     
        if (g_str_has_prefix(name, "nfv-policy-") && g_str_has_suffix(name, ".json")) {
            gchar *path = g_build_filename("/var/run/purecvisor", name, NULL);
            gchar *content = NULL;
            if (g_file_get_contents(path, &content, NULL, NULL) && content) {
                JsonParser *p = json_parser_new();
                                                              
                if (json_parser_load_from_data(p, content, -1, NULL))
                    json_array_add_element(arr, json_node_copy(json_parser_get_root(p)));
                g_object_unref(p);
            }
            g_free(content); g_free(path);
        }
    }
    g_dir_close(dir);
    return arr;
}

                                                                 
                    
                                                    
                                     
  
                                     
                                                                        
                                                                    

   
                                   
                                
                                                            
                    
  
                                           
                                           
  
                                                      
  
                                                                
                                                         
                                         
   
gboolean pcv_nfv_chain_create(const gchar *name, const gchar *steps_json, GError **error)
{
    if (!name) { g_set_error(error, g_quark_from_static_string("nfv"), 1, "name required"); return FALSE; }
    gchar *path = g_strdup_printf("/var/run/purecvisor/nfv-chain-%s.json", name);
                                                    
    gchar *content = g_strdup_printf("{\"name\":\"%s\",\"steps\":%s}", name, steps_json ? steps_json : "[]");
    gboolean ok = g_file_set_contents(path, content, -1, error);
    g_free(content); g_free(path);
    if (ok) PCV_LOG_INFO(NFV_LOG_DOM, "Service chain '%s' created", name);
    return ok;
}

   
                                        
                   
                    
  
                                                
  
                                         
   
gboolean pcv_nfv_chain_delete(const gchar *name, GError **error)
{
    if (!name) { g_set_error(error, g_quark_from_static_string("nfv"), 1, "name required"); return FALSE; }
    gchar *path = g_strdup_printf("/var/run/purecvisor/nfv-chain-%s.json", name);
    g_unlink(path);                             
    g_free(path);
    return TRUE;
}

   
                                        
  
                                                
                                                                 
  
                                                     
                               
  
                                                    
                                         
   
JsonArray *pcv_nfv_chain_list(void)
{
    JsonArray *arr = json_array_new();
    GDir *dir = g_dir_open("/var/run/purecvisor", 0, NULL);
    if (!dir) return arr;                                       
    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
                                                         
        if (g_str_has_prefix(name, "nfv-chain-") && g_str_has_suffix(name, ".json")) {
            gchar *path = g_build_filename("/var/run/purecvisor", name, NULL);
            gchar *content = NULL;
            if (g_file_get_contents(path, &content, NULL, NULL) && content) {
                JsonParser *p = json_parser_new();
                if (json_parser_load_from_data(p, content, -1, NULL))
                    json_array_add_element(arr, json_node_copy(json_parser_get_root(p)));
                g_object_unref(p);
            }
            g_free(content); g_free(path);
        }
    }
    g_dir_close(dir);
    return arr;
}
