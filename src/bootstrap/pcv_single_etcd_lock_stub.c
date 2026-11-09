#include "bootstrap/pcv_single_edge_runtime.h"

   
                                                                                   
  
                           
                                                   
                                                    
                                        
  
                                                
                                                
                                        
  
                                                             
                                                       
                                               
                                                    
                                                                           
                                             
  
            
                                                 
                                               
                                                         
                                                          
  
           
                                             
                                                                
                                   
   
  
                                                                      
                                                   
                                                         
   
gboolean
pcv_etcd_acquire_inflight_lock(PcvEtcdClient *c,
                               const gchar *pool,
                               const gchar *node_name,
                               const gchar *op,
                               gint ttl_sec,
                               GError **error)
{
    (void)c;
    (void)pool;
    (void)node_name;
    (void)op;
    (void)ttl_sec;
    (void)error;
    return TRUE;                                      
}
