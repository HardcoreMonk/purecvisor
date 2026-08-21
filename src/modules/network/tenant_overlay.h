   
                         
                                                                 
  
                           
                                                   
                                                    
                                        
  
          
                                                  
                                                 
                                                     
                                                         
                                                               
  
                     
                                                       
                                                                    
                                                          
                                                       
  
                                           
  
             
                                                      
                                                       
                                                                          
                                        
  
                       
                                                        
                                                        
                                                   
  
         
                                                        
                                                         
                                                  
  
        
                                                                      
                                                                        
                                                                     
   

#ifndef PURECVISOR_TENANT_OVERLAY_H
#define PURECVISOR_TENANT_OVERLAY_H

#include <glib.h>

G_BEGIN_DECLS

   
                                 
                                                             
                                                             
                                    
  
                                                    
                                                             
                                                        
   
gboolean pcv_tenant_overlay_wg_genkeys(gchar **privkey_out,
                                       gchar **pubkey_out,
                                       GError **error);

   
                                     
                                                                         
                                              
                                           
                                                     
                                           
                                                                   
                                          
  
                                                        
                                                          
                                                                 
                                                        
                                                
                      
   
gboolean pcv_tenant_overlay_wg_endpoint_up(const char *ep_name,
                                           const char *privkey,
                                           const char *overlay_ip_cidr,
                                           guint listen_port,
                                           const char *transport_ip_cidr,
                                           GError **error);

   
                                  
                                        
                                        
                                                            
                                                                 
                                        
  
                                                             
                                                                         
                          
   
gboolean pcv_tenant_overlay_wg_peer_add(const char *ep_name,
                                        const char *peer_pubkey,
                                        const char *peer_overlay_ip,
                                        const char *peer_endpoint,
                                        GError **error);

   
                                                                   
                                        
                                            
                                                                  
                                        
  
                                                           
                                                                        
                                                                      
                                                   
                                                        
                                                      
   
gboolean pcv_tenant_overlay_wg_peer_remove(const char *ep_name,
                                           const char *peer_pubkey,
                                           const char *peer_overlay_ip,
                                           GError **error);

   
                                       
                             
                                
  
                                                     
                                              
                                                     
                                                                 
                                                       
                             
                                                          
   
gboolean pcv_tenant_overlay_wg_endpoint_down(const char *ep_name,
                                             GError **error);

   
                                                                        
                                                                       
                                                                       
                                                                      
                                                                   
                                                                     
                                         
                                                                            
                                                                                     
                                                                       
                                            
  
                                                        
                                                    
                                                  
                                                                   
                                                            
                                                                  
              
                              
                                                                          
                                                                      
                                                 
                                                                          
                                                        
                                                                              
                                                                       
                                                                             
                                                               
                                                            
                                                                     
                                            
                      
   
gboolean pcv_tenant_overlay_wg_attach_tap(const char *ep_name,
                                          const char *tap_name,
                                          const char *overlay_ip,
                                          const char *overlay_subnet_cidr,
                                          GError **error);

   
                                               
                                                          
                                        
                                                    
                                                     
                                        
  
                                                     
                                                              
                                                                  
                                                       
                                                     
                             
                                                      
   
gchar *pcv_tenant_overlay_wg_capture_test(const char *bridge,
                                          const char *src_ep,
                                          const char *dst_overlay_ip,
                                          const char *marker,
                                          GError **error);

                                                                               
                                                                   
                                                                               
                                                     
                                           
  
                                                                  
                                                   
                                                     
                         
                                                                               
   

   
                             
                               
                              
  
                                                  
                                              
                                                  
                                                           
   
gboolean pcv_tenant_overlay_create(const gchar *name, GError **error);

   
                             
                  
                              
  
                                                 
                                              
                                             
                                                    
                                                         
   
gboolean pcv_tenant_overlay_delete(const gchar *name, GError **error);

   
                               
                  
                              
  
                                             
                                               
                                                        
                           
   
gchar *pcv_tenant_overlay_alloc_ip(const gchar *name, GError **error);

   
                              
                 
                                                     
  
                                             
                                                         
                                         
                                        
   
void pcv_tenant_overlay_free_ip(const gchar *name, const gchar *ip);

   
                           
  
                                          
                    
                                                                        
   
GPtrArray *pcv_tenant_overlay_list(void);

   
                                 
                     
                                                                        
  
                                            
                                                                         
   
gboolean pcv_tenant_overlay_get_subnet(const gchar *name, gchar **cidr_out);

                                                                               
                                                                           
                                                                               
                                                               
                                                                   
                                                            
                                 
                                                                               
   

   
                                        
                        
                                    
                                                    
                                                             
                                    
  
                                                     
                                                  
                                                     
                                                                          
                                           
                                        
                                                                              
   
gboolean pcv_tenant_overlay_gen_and_store_key(const gchar *tenant,
                                              const gchar *vm,
                                              const gchar *overlay_ip,
                                              gchar **pubkey_out,
                                              GError **error);

   
                                   
                        
                       
                                                       
                                    
  
                                                      
                                                                               
           
                                                                     
                              
   
gboolean pcv_tenant_overlay_load_privkey(const gchar *tenant,
                                         const gchar *vm,
                                         gchar **privkey_out,
                                         GError **error);

                                                                               
                                                              
                                                                               
                                                                
                                                       
                                                               
                                     
  
                                                                         
                                                                     
                                                       
                                                          
                                           
  
                                                         
                                                   
                                                          
        
  
                                                               
                                                               
                                                         
                                                                               
   

   
                                
                                                                
                                             
                               
  
                                                      
                                                         
                                              
                                                         
                                                             
                                         
                                                    
                             
                                                          
   
gchar *pcv_tenant_overlay_attach_vm(const gchar *tenant, const gchar *vm,
                                    GError **error);

   
                                
                   
                              
                               
  
                                                      
                                                      
                                                   
                                                       
                                                     
                                                                    
                                                  
  
         
                                        
                                                                 
                                                           
                                                                   
                                                                 
                                                            
                                                             
   
gboolean pcv_tenant_overlay_detach_vm(const gchar *tenant, const gchar *vm,
                                      GError **error);

   
                                                   
                        
                       
                                                              
  
                                                     
                                                                 
                                            
                                                                             
   
gboolean pcv_tenant_overlay_get_member_ep(const gchar *tenant, const gchar *vm,
                                          gchar **ep_name_out);

                                                                               
                                                      
                                                                               
                                                            
                                                          
                                                
                                                                
          
                                                                                  

   
                                                                 
              
  
                                                                
                                                      
                                         
                                         
                               
   
gboolean pcv_tenant_overlay_vm_in_any_tenant(const gchar *vm);

   
                                                      
                                      
  
                                                       
                                                      
                                                        
                                        
  
                                                   
                                                                   
   
void pcv_tenant_overlay_on_vm_gone(const gchar *vm);

   
                                                                
  
                                                                     
                                                        
                                                               
                                           
                                                                      
   
GPtrArray *pcv_tenant_overlay_list_member_vms(void);

   
                                                                    
                            
  
                                                             
                                                        
                                        
   
gboolean pcv_tenant_overlay_ep_name_is_overlay(const gchar *name);

   
                                                    
                              
  
                                                   
                                         
                                                            
   
GPtrArray *pcv_tenant_overlay_wg_list_netns(GError **error);

   
                                                             
                                         
  
                                                 
                                                                  
                                                 
                     
   
guint pcv_tenant_overlay_sweep_orphan_endpoints(guint *fail_out);

                                                                               
                                                                   
                                                                               
                                                                            
                                                                    
                                                           
                            
                                                                               
   

   
                                
                              
  
                                                  
                                                               
                                                                        
                                                         
                                                                       
                                                        
                                                          
                                                         
                  
                                                          
   
gboolean pcv_tenant_overlay_rehydrate(GError **error);

G_END_DECLS

#endif                                  
