                                                                             
                             
                                                                                   
                                                         
  
                           
                                                                                  
                                                                                    
                                                                                              
                                                                                                       
                                                                                                           
                                                                             
                                                                             
                                                                   
                                                                     
                                                                                                 
                                                                                   
                                                                                                                                     
                                                                              
                                                                  
                                                                                           
                                                               
                                                                                               
  
                                                                        
                                                                         
                                                          
                                                                                
#ifndef PURECVISOR_LXC_UNIT_H
#define PURECVISOR_LXC_UNIT_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

                                                                          
#define PCV_LXC_UNIT_TEMPLATE          "purecvisor-lxc@"
                                                                                        
#define PCV_LXC_UNIT_ENV_PATH          "/run/purecvisor/lxc-unit.env"
                                                                                                     
                                                                                                       
#define PCV_LXC_RUNTIME_CONFIG_DIR     "/run/purecvisor/lxc-runtime"
                                                   
#define PCV_LXC_UNIT_PREP_BUDGET_SEC   4
                                                           
#define PCV_LXC_UNIT_START_TIMEOUT_SEC 26
                                                                  
#define PCV_LXC_UNIT_START_BUDGET_SEC  30
                                  
#define PCV_LXC_UNIT_POLL_MS           100
                                                           
#define PCV_LXC_LOG_ROTATE_BYTES       ((goffset)1024 * 1024)
                                 
#define PCV_LXC_UNIT_CAUSE_MAX         512
                                                                             
#define PCV_LXC_UNIT_NOT_INSTALLED     "container runtime unit purecvisor-lxc@.service is not installed"

   
                 
                                                                                   
                                                                                                 
                                                                                                    
                                                                                     
                                                                            
                                  
                                                             
                                                               
   
typedef struct {
    gboolean (*run)(const gchar * const *argv, guint timeout_sec, gchar **stdout_out, gchar **stderr_out,
                    gint *exit_status, GError **error);
    gboolean (*is_running)(const gchar *lxc_path, const gchar *name);
    void     (*sleep_ms)(guint ms);
    gint64   (*now_us)(void);
} PcvLxcUnitOps;

   
                           
                                              
  
                                                                                      
                                                    
   
gboolean pcv_lxc_unit_path_valid(const gchar *lxc_path);

   
                         
                      
  
                                                                             
                              
   
gchar   *pcv_lxc_unit_env_text(const gchar *lxc_path);

   
                          
                                                  
                      
                         
  
                                                                                                 
                                                                                 
                                                                                     
                                                                        
   
gboolean pcv_lxc_unit_env_write(const gchar *env_path, const gchar *lxc_path, GError **error);

   
                           
                                                    
                                                  
                         
  
                                                                              
                                                
                                                  
   
gboolean pcv_lxc_unit_log_rotate(const gchar *log_path, goffset limit, GError **error);

   
                          
                                          
  
                                                                           
                                                                    
                                                                
                                                    
   
gchar   *pcv_lxc_unit_log_cause(const gchar *log_tail);

   
                         
                                                  
                                                  
                                                                                                        
                                                          
                                                                  
                             
  
                                                                                                   
                                                                                       
                                                                                                    
                                                                           
                          
                                                                                                      
                                                                             
   
GPtrArray *pcv_lxc_unit_log_tail(const gchar *lxc_path, const gchar *name, const gchar *fallback_dir,
                                 gint n_lines, gint *total, GError **error);

   
                      
                                                                           
                                                            
                         
  
                                            
                                                         
                                                                                                                      
                                                                     
                                                                                                              
                                                                                                                     
                                                                
                                                       
   
gboolean pcv_lxc_unit_start(const gchar *lxc_path, const gchar *name, GError **error);

   
                                         
                             
  
                                                                                       
                                                                                             
                                                                                          
                                         
                                                    
   
void     pcv_lxc_unit_reset_failed_best_effort(const gchar *name);

   
                                 
                                                             
                                                 
   
void     pcv_lxc_unit_set_ops_for_test(const PcvLxcUnitOps *ops);

   
                                      
                                                                            
                                              
   
void     pcv_lxc_unit_set_env_path_for_test(const gchar *path);

G_END_DECLS

#endif                            
