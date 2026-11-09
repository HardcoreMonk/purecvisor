                                                                             
                                     
                                                                                                            
  
                           
                                                                                          
                                                                            
                                                                                                  
                                                                                            
                                                                                                 
                                                                                         
                                                                                                         
                                                                                                   
                                                                                 
                                                                                                                  
                                                                                                                
                                                        
                                                                                       
                                                                                
  
                                                                     
                                                                   
                                                                           
                                                                                
#ifndef PURECVISOR_LXC_VOLUME_GUARD_H
#define PURECVISOR_LXC_VOLUME_GUARD_H

#include <glib.h>
#include "lxc_userns.h"                                                           

G_BEGIN_DECLS

   
                              
                                                                                                 
                                                                                                         
                                                                               
                            
                                                                                                          
                                                                                                            
                                                                                                                
                                
                                                                                          
                  
                                    
                                                         
   
int pcv_lxc_volume_source_open(const gchar *host_path, gchar **canon_out, GError **error);

                                                                        
                                                                                             
                                                                                    
                                                                                        
                                                                                            

   
                    
                                                                                             
                                                                                                
              
                                                                             
                                                                                                
                          
                                                 
   
typedef struct {
    gchar   *source;
    gchar   *dest;
    gchar   *options;
    gboolean is_bind;
} PcvLxcMountEntry;

   
                                 
                                                                                                          
                                                      
                                         
                                                      
   
gboolean pcv_lxc_mount_entry_is_volume(const PcvLxcMountEntry *e);

   
                             
                                                             
                                                                                                              
                                                                                                     
                                                                                                           
            
                                      
   
gchar *pcv_lxc_volume_dest_canon(const gchar *dest);

   
                         
                                                                                                                              
                                                                                      
                                
                                              
   
gchar *pcv_lxc_volstage_slot(const gchar *volstage_root, const gchar *name, const gchar *dest, guint ordinal);

   
                               
                                                                                                        
                                                                                    
                                                                                              
                                                                                         
                                                                                     
                                                                                       
                                                                            
   
gchar *pcv_lxc_runtime_config_text(const gchar *config_text, const gchar *volstage_root, const gchar *name, GError **error);

   
                      
                                                                                                     
                                                    
                                                                                     
                                                                                                              
                                                           
                                                                                
                                                
                                                          
   
typedef struct {
    gchar   *source;
    gchar   *dest;
    gchar   *container_path;
    gchar   *slot;
    gboolean readonly;
    gboolean recursive;
} PcvLxcVolstageLine;

   
                          
                                                                                                             
                                                                                   
                                                            
                                                                                                             
                                                               
                                                                       
   
GPtrArray *pcv_lxc_volstage_lines(const gchar *config_text, const gchar *volstage_root, const gchar *name, GError **error);

   
                           
                                                                                                                         
                                                                       
                      
                                                      
   
gboolean pcv_lxc_config_root_gid(const gchar *config_text, gint64 *gid_out);

   
                                
                                                                                                      
                                                                                                      
                                  
                                                                                      
                                                                        
                                               
   
GPtrArray *pcv_lxc_config_mount_entries(const gchar *config_text);

   
                              
                                                                                                   
                                                                                            
                                                                                 
                                                                    
                                                                                                      
                                            
                          
                                                                            
   
gboolean pcv_lxc_volume_guard_check(const gchar *lxc_path, const gchar *name, gchar **bad_container_path_out);

   
                                
                                                                                                      
                                                               
   
gchar *pcv_lxc_volume_guard_message(const gchar *container_path);

                                                                                     
                                                                            
                                                                                         
                                                                                                            
                                                                          
                                                                                
                                 

                                                                                                
#define PCV_LXC_VOLUME_GUARD_HOOK "/usr/local/bin/pcvctl"
                                                                                                        
#define PCV_LXC_VOLUME_GUARD_HOOK_KEY   "lxc.hook.pre-start"
#define PCV_LXC_VOLUME_GUARD_HOOK_VALUE PCV_LXC_VOLUME_GUARD_HOOK " container volume-guard-hook"

   
                                  
                                                                                          
                                                                                                    
                                                                                     
                                                                                        
                                                                                       
                 
                                                                          
                                                                              
   
gchar *pcv_lxc_config_with_guard_hook(const gchar *config_text);

   
                                       
                                                                                                    
                                                                         
                                                        
   
gboolean pcv_lxc_volume_guard_hook_installed(const gchar *path);

                                                                                                  
#define PCV_LXC_VOLSTAGE_ROOT "/run/purecvisor-volstage"

   
                                  
                                                                                                      
                                     
                                                                                                             
                                                                                                                  
                                                                         
                                                                                              
                                                                                                                  
                                     
                                                                                                                  
                                                                                                              
                                                                                      
                                                                                               
                                                                                                         
                                                                                           
                  
                                                         
                                                                                     
                                
   
int pcv_lxc_volume_guard_hook_main(const gchar *config_path, const gchar *lxc_name);

   
                                           
                                                                                              
                                                                                     
                                     
                                                 
   
int pcv_lxc_volume_guard_hook_run_with_root(const gchar *volstage_root, const gchar *config_path, const gchar *lxc_name);

                                                                                     
#define PCV_LXC_VOLUME_GUARD_VOLGEN "purecvisor.volgen"

   
                                    
                                                                                             
                                                                                             
                                                                                                                              
                                                                                              
                                                      
                          
                                                                
   
gboolean pcv_lxc_volume_guard_check_start(const gchar *lxc_path, const gchar *name, gchar **bad_container_path_out);

   
                               
                                                                                               
                                                                                          
                                                                                 
                                                                               
   
gboolean pcv_lxc_volume_guard_forget(const gchar *lxc_path, const gchar *name);

G_END_DECLS

#endif                                    
