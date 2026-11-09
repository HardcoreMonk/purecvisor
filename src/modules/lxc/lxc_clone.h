                                                                             
                              
                                                                                                                                                    
  
                           
                                                                                            
                                                                                           
                                                                                                             
                                                                     
                                                                                                                
                                                                                                    
                                                                                                                        
                                                                                     
                                                                                                                
                                                                              
                                                                                                
  
                                                                                     
                                                                            
                                                                                
#ifndef PURECVISOR_LXC_CLONE_H
#define PURECVISOR_LXC_CLONE_H

#include <glib.h>

G_BEGIN_DECLS

   
                             
                      
                                       
                                                     
                           
                                                                         
                                                                                                               
                                                                                                                    
                                                                                                             
                                                              
                                         
                                                            
   
gchar *pcv_lxc_clone_config_text(const gchar *text, const gchar *lxc_path, const gchar *lxc_path_canon,
                                 const gchar *src, const gchar *dst,
                                 void (*rand_bytes)(guint8 *buf, gsize n), GError **error);

   
                                
                                                                              
                                                
   
gboolean pcv_lxc_clone_has_clone_hook(const gchar *text);

   
                                
                                                                                                 
                                                          
                                                                       
   
gboolean pcv_lxc_clone_write_hostname(int rootfs_fd, const gchar *dst);

                                                                                                        
                                                                                   

   
                  
                                                                                                          
                                  
                                                                                                                               
                                                                                           
                                                                                                     
                                                                                
                                                                        
                                                                             
                                                                                                                        
                                                                                                                                      
                                                                                                      
                                                                                                                             
                                               
                                                                             
                                                                               
                                                                                             
                                                                                         
                                                                                                    
  
                                                                                                
                           
                                                       
   
typedef struct {
    gboolean (*run)(const gchar *const *argv, gchar **stdout_out, GError **error);
    gboolean (*pipe)(const gchar *const *producer, const gchar *const *consumer, GError **error);
    gboolean (*is_running)(const gchar *name);
    gboolean (*dir_exists)(const gchar *path);
    gboolean (*residue_clear)(const gchar *lxc_path, const gchar *name, GError **error);
    gboolean (*rewrite_config)(const gchar *lxc_path, const gchar *dst, const gchar *src, GError **error);
    gboolean (*write_hostname)(const gchar *lxc_path, const gchar *dst);
    gboolean (*settle)(const gchar *lxc_path, const gchar *dst, GError **error);
    gboolean (*product_files)(const gchar *lxc_path, const gchar *src, const gchar *dst, GError **error);
    gboolean (*stamp_owner)(const gchar *dst, const gchar *subject);
    gboolean (*destroy_snapshot)(const gchar *snapshot, GError **error);
    gboolean (*destroy_dataset)(const gchar *dataset, GError **error);
    gboolean (*remove_dir)(const gchar *path, GError **error);
} PcvLxcCloneOps;

   
                                  
                                                                                    
                                                                             
                                                                 
   
void pcv_lxc_clone_set_ops_for_test(const PcvLxcCloneOps *ops);

   
                     
                                                                                
                                                                                           
                                                                              
                                                                         
  
                                                                                       
                                                                                                                         
                                                                                                                    
                                                                                                                                                                             
                                                                                                                                     
                                                                                                                    
                                                                                                                               
                                                                                                                            
                                                                                                 
                                                                             
                                                                                              
                                                                                         
                                                                                                                                                  
                                                                                  
                                                                                          
                                                                               
   
gboolean pcv_lxc_clone_run(const gchar *lxc_path, const gchar *zfs_base, const gchar *src, const gchar *dst,
                           const gchar *subject, gboolean src_unpriv, GError **error);

G_END_DECLS

#endif                             
