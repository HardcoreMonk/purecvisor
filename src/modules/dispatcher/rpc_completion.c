   
                         
                                                              
  
                                                                        
                                                                 
                                                      
  
                           
                                                                            
                                                                            
                                                                   
                                                                         
                                                       
                                                            
  
                       
                                                   
                                                   
                                                
   
#include "rpc_completion.h"
#include "rpc_utils.h"

typedef struct {
    gchar *method;
    PcvRpcCompletionResult result;
} PcvRpcCompletionScope;

static void
_completion_scope_free(gpointer data)
{
    PcvRpcCompletionScope *scope = data;
    if (!scope) return;
    g_free(scope->method);
    g_free(scope);
}

static GPrivate completion_scope_key = G_PRIVATE_INIT(_completion_scope_free);

   
                              
  
                           
                                                         
                                                                      
                                                                
  
                                                    
                 
   
void
pcv_rpc_completion_begin(const gchar *method)
{
    PcvRpcCompletionScope *scope = g_new0(PcvRpcCompletionScope, 1);
    scope->method = g_strdup(method);
    scope->result.error_code = PURE_RPC_ERR_INTERNAL_ERROR;
    g_private_replace(&completion_scope_key, scope);
}

   
                                                                   
  
                           
                                                       
                                                                 
  
                                            
   
void
pcv_rpc_completion_observe_response(gboolean success, gint error_code)
{
    PcvRpcCompletionScope *scope = g_private_get(&completion_scope_key);
    if (!scope) return;                                                   

    scope->result.response_observed = TRUE;
    scope->result.success = success;
    scope->result.error_code = success ? 0 : error_code;
}

   
                                         
  
                           
                                                          
                                                             
  
                                                       
   
void
pcv_rpc_completion_note_audit(const gchar *method)
{
    PcvRpcCompletionScope *scope = g_private_get(&completion_scope_key);
    if (!scope || !method) return;
    if (g_strcmp0(scope->method, method) == 0)
        scope->result.direct_audit = TRUE;
}

   
                                             
  
                           
                                                             
                                                                            
                                 
  
                                                     
         
   
PcvRpcCompletionResult
pcv_rpc_completion_finish(void)
{
    PcvRpcCompletionResult result = {
        .response_observed = FALSE,
        .success = FALSE,
        .error_code = PURE_RPC_ERR_INTERNAL_ERROR,
        .direct_audit = FALSE,
    };
    PcvRpcCompletionScope *scope = g_private_get(&completion_scope_key);
    if (scope) result = scope->result;
    g_private_replace(&completion_scope_key, NULL);
    return result;
}

   
                                                
  
                           
                                                    
                                                                         
                                 
  
                                                     
                        
   
gboolean
pcv_rpc_completion_dispatch_succeeded(PcvRpcCompletionResult result,
                                       gboolean response_may_arrive_later)
{
    if (result.response_observed)
        return result.success;

    return response_may_arrive_later;
}
