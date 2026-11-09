   
                    
                                                                  
  
                           
                                                                                   
                                                                                      
                                                                            
                                                            
                                                                   
  
                                                                  
                                                                   
                
   
#pragma once
#include <glib.h>

#include "vpc_store.h"

   
                                                                            
                     
                        
  
                                         
  
                                
   
gboolean pcv_vpc_fence_nft_quarantine(PcvVpcStore *store, GError **error);

   
                                                                             
                     
                                                                        
                   
  
                                                                                           
                                                                                   
                                                                                            
                                                    
  
                                                      
  
                          
   
gboolean pcv_vpc_fence_quarantine(PcvVpcStore *store, const gchar *skip_vpc_id, GError **error);

   
                                                                                 
                                    
  
                              
  
                       
   
gboolean pcv_vpc_fence_full_policy(PcvVpcStore *store, GError **error);

   
                                                                              
                     
                                                                
                   
  
                                                                                        
                                                           
                                                                                                                    
                                                                    
  
                                                                                     
  
                                          
   
gboolean pcv_vpc_fence_clear_ovn(PcvVpcStore *store, const gchar *target_vpc_id, GError **error);

   
                                                         
                     
                        
                             
  
                                                                                                            
  
                                                                 
   
void pcv_vpc_fence_policy_sync_failed(PcvVpcStore *store, const gchar *vpc_id, const GError *cause);

   
                                                                  
                     
                                                                                                   
                                                                                         
  
                                                                              
                                                                         
                                                                                         
                                                                   
  
                                       
   
void pcv_vpc_fence_release(PcvVpcStore *store, const gchar *keep_vpc_id);

   
                                                                               
                     
  
                                                   
  
                                          
   
void pcv_vpc_fence_all_down(PcvVpcStore *store);

   
                                                   
                     
                                                   
                    
                          
                       
  
                                                                
  
                                                              
   
gboolean pcv_vpc_fence_ovn_failed(PcvVpcStore *store, GHashTable *failed, const gchar *vpc_id,
                                  const GError *cause, GError **error);

   
                                                                                  
                     
                           
                                    
                           
                    
  
                                          
  
                                                         
  
                              
   
gboolean pcv_vpc_fence_reconcile_ovn_begin(PcvVpcStore *store, JsonArray *vpcs,
                                           const gchar *transit_pool, GHashTable *failed,
                                           GError **error);

   
                                                                                             
                                                                 
  
                                  
  
                              
   
gboolean pcv_vpc_fence_reconcile_ovn_clear(PcvVpcStore *store, GHashTable *failed, GError **error);

   
                                                                       
                 
  
                                                                  
  
                                 
   
void pcv_vpc_fence_ovn_failed_edges_down(GHashTable *failed);
