                                                                             
                           
                                                                              
  
                           
                                                   
                                                    
                                        
                                                                      
                                                                                       
                                                     
  
                                                      
                                                        
  
                                                                
                                                                    
                                                                        
                                                                           
                                                                                    
                          
                                                             
                                                     
                                                                                

#ifndef PURECVISOR_ZFS_EXEC_H
#define PURECVISOR_ZFS_EXEC_H

#include <glib.h>

G_BEGIN_DECLS

                                                                    
#define PCV_ZFS_EXEC_LOCK_WAIT_MS (5 * 60 * 1000)
                                                                            
#define PCV_ZFS_EXEC_SHORT_LOCK_WAIT_MS (5 * 1000)

   
                         
                                                                                 
                                                                     
                                                                                 
                                                                                
  
                                          
                             
   
gboolean pcv_zfs_create_locked(const gchar * const *options, const gchar *dataset,
                               gchar **stderr_out, GError **error);

   
                          
                                                                        
                                                           
                                                                                 
                                                                                
  
                                                        
                             
   
gboolean pcv_zfs_destroy_locked(const gchar *flag, const gchar *target,
                                gchar **stderr_out, GError **error);

   
                               
                                                                                        
                                                                 
                             
   
gboolean pcv_zfs_create_locked_short(const gchar * const *options, const gchar *dataset,
                                     gchar **stderr_out, GError **error);

   
                                
                                                                                         
                                                 
                             
   
gboolean pcv_zfs_destroy_locked_short(const gchar *flag, const gchar *target,
                                      gchar **stderr_out, GError **error);

   
                            
                                           
                   
                                 
  
                                                                                                    
                                                                                  
                                             
                                                                          
                     
   
gboolean pcv_zfs_destroy_settling(const gchar *flag, const gchar *target, GError **error);

   
                                       
                                                   
                                        
   
void pcv_zfs_exec_set_lock_wait_for_test(gint wait_ms);

G_END_DECLS

#endif                            
