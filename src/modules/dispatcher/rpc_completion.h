   
                         
                                                  
  
                           
                                                   
                                                    
                                        
  
                                                        
                                                      
                                            
   
#ifndef PCV_RPC_COMPLETION_H
#define PCV_RPC_COMPLETION_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    gboolean response_observed;                                           
    gboolean success;                                               
    gint error_code;                                                            
    gboolean direct_audit;                                            
} PcvRpcCompletionResult;

                                                  
void pcv_rpc_completion_begin(const gchar *method);

                                                        
void pcv_rpc_completion_observe_response(gboolean success, gint error_code);

                                                          
void pcv_rpc_completion_note_audit(const gchar *method);

                                        
PcvRpcCompletionResult pcv_rpc_completion_finish(void);

   
                                                         
  
                                                                 
                                                               
                                                                   
                
  
                                                     
                       
   
gboolean pcv_rpc_completion_dispatch_succeeded(PcvRpcCompletionResult result,
                                                gboolean response_may_arrive_later);

G_END_DECLS

#endif                           
