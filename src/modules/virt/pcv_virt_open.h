                                   
  
                           
                                                                         
                                                                     
                                                                                
                                                                          
                                                                   
                                                                                 
              
                                                                      
                                                                  
  
                       
                                                                 
                                                         
   
#ifndef PURECVISOR_VIRT_OPEN_H
#define PURECVISOR_VIRT_OPEN_H

#include <glib.h>
#include <libvirt/libvirt.h>

G_BEGIN_DECLS

                                     
#define PCV_VIRT_OPEN_TIMEOUT_SEC 10
                                       
#define PCV_VIRT_OPEN_ABANDONED_MAX 4
                                                         
#define PCV_VIRT_CLEANUP_TIMEOUT_SEC 3

   
                                                 
  
                                                                       
                                
  
                                                                                         
                                                                                           
                                                                       
                                                   
   
virConnectPtr pcv_virt_open_bounded(const gchar *uri, gboolean read_only, guint timeout_sec, GError **error);

   
                                                                    
  
                                                                  
                                                                  
  
                                                                                            
                                                                                                   
                                                                    
                              
                                                                
   
gboolean pcv_virt_cleanup_bounded(const gchar *what, GThreadFunc fn, gpointer data, guint timeout_sec);

                                                                
#define PCV_VIRT_CLOSE_DETACHED_MAX 4

   
                                                         
  
                                                                     
  
                                                                          
                                                                                                                                               
                                                          
                 
                                                                                    
                                                                                                 
   
void pcv_virt_close_detached(virConnectPtr conn);

                                                                           
                                   
guint pcv_virt_close_detached_pending(void);

                                                                                      
                                                 
                                                                  
void pcv_virt_open_set_fn_for_test(virConnectPtr (*open_fn)(const gchar *uri, gboolean read_only),
                                   int (*keepalive_fn)(virConnectPtr conn, int interval, unsigned int count),
                                   void (*close_fn)(virConnectPtr conn));

                                                                
                                  
guint pcv_virt_open_abandoned_for_test(void);

G_END_DECLS

#endif                             
