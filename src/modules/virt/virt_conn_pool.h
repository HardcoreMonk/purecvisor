                                    
  
                           
                                                   
                                                    
                                        
  
                            
  
                                                     
                                                 
                                                      
                                                   
                                                  
  
                                                          
                                                         
                                  
  
                                                             
                                             
  
         
                                                   
                              
                                  
  
                                                  
                                                           
                          
   

#ifndef PURECVISOR_VIRT_CONN_POOL_H
#define PURECVISOR_VIRT_CONN_POOL_H

#include <glib.h>
#include <libvirt/libvirt.h>

G_BEGIN_DECLS

   
                        
                                              
  
                                              
  
                                                               
   
void virt_conn_pool_init(guint max_size);

   
                            
                                     
                                  
  
                                                   
  
                                               
                                                               
                                                                                                                                            
                               
                                                                  
                                         
   
virConnectPtr virt_conn_pool_acquire(void);

   
                                                            
  
                                                          
                                                        
  
                                                            
                                                                 
                                                              
                                    
                                                               
                                                                        
                                         
                                
   
virConnectPtr virt_conn_pool_try_acquire(void);

   
                                                                                         
  
                                                                           
                                                 
  
                                                                           
                                                 
                                
   
const gchar *virt_conn_pool_uri(void);

   
                               
                                          
  
                                                  
  
                                                                     
   
void virt_conn_pool_release(virConnectPtr conn);

   
                                    
                                 
  
                                                    
  
                                                          
                                                                             
   
void virt_conn_pool_shutdown(void);

   
                                      
  
                                                       
  
                                                                
                                 
                             
   
void virt_conn_pool_stats(guint *out_idle, guint *out_total, guint *out_max);

   
                                
  
                                                   
  
                                       
   
gdouble virt_conn_pool_wait_avg_seconds(void);

   
                                                              
  
                                                                                                                                     
                                                                                
   
void virt_conn_pool_note_libvirtd_ok(void);

   
                                                                
  
                                                                                 
   
void virt_conn_pool_note_coverage_end(void);

   
                                                     
  
                                   
   
void virt_conn_pool_set_door_now_for_test(gint64 (*now)(void));

   
                                                
  
                                                                 
                                                                    
                                                                   
                                                                                          
                                  
   
typedef struct {
    virConnectPtr (*open)(const gchar *uri, guint timeout_sec, GError **error);
    int           (*close)(virConnectPtr conn);
    int           (*is_alive)(virConnectPtr conn);
    int           (*get_version)(virConnectPtr conn, unsigned long *ver);
} PcvConnPoolOps;

   
                                                                  
  
                                                            
                                                               
   
void virt_conn_pool_set_ops_for_test(const PcvConnPoolOps *ops);

   
                                                                    
  
                                                    
   
void virt_conn_pool_set_terminating_fn_for_test(gboolean (*fn)(void));

   
                                                   
  
                                                                 
                                
                                                         
                         
   
gboolean virt_conn_pool_lock_is_free_for_test(void);

   
                                                    
  
                                           
   
void virt_conn_pool_reap_for_test(void);

G_END_DECLS

#endif                                  
