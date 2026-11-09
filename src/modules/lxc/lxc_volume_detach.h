                                                                             
                                      
                                                                                                                                        
  
                           
                                                                                      
                                                                                                          
                                                                                                         
                                                                                                            
                                                                                                
                                                                                                
                                                                                                    
                                                                                               
                                                                                
                                                                    
  
                                                                               
                                                                         
                                                                                
#ifndef PURECVISOR_LXC_VOLUME_DETACH_H
#define PURECVISOR_LXC_VOLUME_DETACH_H

#include <glib.h>
#include <sys/types.h>                     
#include "lxc_volume_mount.h"                                             
#include "lxc_driver.h"                                               

G_BEGIN_DECLS

                                                                                       
                                                                                                              
                                                                                              
                                                                      
                                             
typedef struct {
    gchar     *(*read_text)(const gchar *path, gint64 deadline_us, gint *errno_out);                                         
    GPtrArray *(*list_dirs)(const gchar *path, gint64 deadline_us, gint *errno_out);                                               
    gint64     (*now_us)(void);                                                                        
} PcvLxcVerifyOps;

   
                                   
                                                         
                                                
   
void pcv_lxc_verify_set_ops_for_test(const PcvLxcVerifyOps *ops);

   
                                    
                                                            
                                            
   
const PcvLxcVerifyOps *pcv_lxc_verify_real_ops_for_test(void);

#define PCV_LXC_VERIFY_MAX_TASKS   4096
#define PCV_LXC_VERIFY_MAX_CGROUPS 1024
                                                                                             
                                                    
#define PCV_LXC_VERIFY_BUDGET_US   5000000

   
                              
                                                                                                      
                                                                                                
                                                                                
  
                                                                      
                                                                                                        
                                                                                                      
                                                                                                                
                                                                                                                    
                                                                                            
                                                                                                                                                                          
                                                                                            
                                                                                                                
                                                                                                      
                                                                                                                       
                                                                                                                                                  
                                                                                                                         
                                                                                                                  
                                                                                           
                                 
                                                                                 
                                               
   
gboolean pcv_lxc_volume_verify_gone(const gchar *cgroup_root, const gchar *name, pid_t init_pid,
                                    const PcvLxcVolumeSet *set, const gchar *target_dest, GError **error);

                                                                                                     
                                                                                         
                                                                                 
                                                                                                             
                                                                                                       
                                                                                         
                                                               
                                                                                  

#define PCV_LXC_NS_DETACH_TIMEOUT_SEC 5                                    
#define PCV_LXC_NS_DETACH_OUTPUT_MAX  ((gsize)64 * 1024)                             

   
                                 
                                                                                                      
                                      
                                                                                                             
                                                                                                                      
                                                                  
                                                                                                         
                                                                                 
                                          
                                                    
   
GStrv    pcv_lxc_volume_ns_detach_argv(const gchar *pcvctl, const gchar *name, pid_t init_pid, const PcvLxcVolumeSet *set,
                                       const gchar *target_dest);

   
                            
                                                                                                                                      
                                                                                                                  
                                                                                                                            
                                                                          
                                                                                          
                                                                                           
                                                          
                                                                                       
                                                                                                                       
                              
                                                     
   
gboolean pcv_lxc_volume_ns_detach(const gchar *name, pid_t init_pid, const PcvLxcVolumeSet *set, const gchar *target_dest,
                                  GError **error);

   
                                         
                                                                                                    
                                                                      
                                            
   
void     pcv_lxc_volume_set_ns_detach_for_test(gboolean (*fn)(const gchar *const *argv, gchar **out, GError **error));

                                                                                                                    
                                                                                                               
                                                                                    
                                                                                     
                                                                                           
                                                                                    
                                                                  
                                                                                
                                         

   
                                         
                                                                                                                       
                                                   
                                                        
   
void     pcv_lxc_volume_set_state_ops_for_test(PcvLxcState (*state)(const gchar *name), pid_t (*init_pid)(const gchar *name));

   
                             
                                                                                             
                                                                                                   
                                                                                             
                                                                                                   
                                      
                                                             
                                                                                                               
                                                           
                                                                                                    
                                                                                                               
                                                                                                                             
                                                                                                                    
                                                                                                                                 
                                                                                                           
                                                                                                      
                                                                                                                                    
                                                                                                                      
                                                                                                                    
                                                                                                        
                                                                                                                       
                                                                                                                
                                                                                  
                                                   
   
gboolean pcv_lxc_volume_detach_run(const gchar *lxc_path, const gchar *name, const gchar *container_path,
                                   gboolean *drop_line_out, gboolean *was_running_out, GError **error);

   
                              
                                                      
                                                                                                            
                                                                                                                  
                                                                                                       
                                                         
                                                                                                                                     
                                                                                                                                 
                                 
                                                                           
   
gboolean pcv_lxc_volume_config_drop(const gchar *lxc_path, const gchar *name, const gchar *container_path,
                                    gboolean running, GError **error);

G_END_DECLS

#endif                                     
