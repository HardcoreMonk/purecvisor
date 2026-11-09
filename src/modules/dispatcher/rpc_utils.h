   
                    
                                          
  
                           
                                                   
                                                    
                                        
  
                                                      
                                                       
                                             
  
                                                                 
              
                                                      
                                          
                                                  
  
          
                                                        
                                                        
                                                               
  
                                 
                                       
                                        
  
                       
           
                                                               
           
                                                                                        
  
                  
                                                        
                                                                      
                                                             
                                      
  
                  
                                                      
                                                            
                                                                 
                                                             
                                                  
  
         
                                                         
                                                             
                                          
                                                            
                                                                 
   
#ifndef PURECVISOR_RPC_UTILS_H
#define PURECVISOR_RPC_UTILS_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

   
                                           
  
                                                              
                                        
  
                
                                                  
                                                       
                                         
   
typedef enum {
                                    
    PURE_RPC_ERR_PARSE_ERROR     = -32700,
                                                                 
    PURE_RPC_ERR_INVALID_REQUEST = -32600,
                                    
    PURE_RPC_ERR_METHOD_NOT_FOUND= -32601,
                                                 
    PURE_RPC_ERR_INVALID_PARAMS  = -32602,
                                           
    PURE_RPC_ERR_INTERNAL_ERROR  = -32603,

                                                

                                                            
                                                                            
    PURE_RPC_ERR_ZFS_OPERATION   = -32000,
                                                           
                                                                              
    PURE_RPC_ERR_VM_NOT_FOUND    = -32001,
                                           
                                                                                 
    PURE_RPC_ERR_CONFLICT        = -32002,
                                              
    PURE_RPC_ERR_TIMEOUT         = -32003,
                                                                    
    PURE_RPC_ERR_BUSY            = -32004,
                                                       
    PURE_RPC_ERR_NOT_FOUND       = -32005,
                                              
    PURE_RPC_ERR_FORBIDDEN       = -32006,
                                                      
                                                             
                                     
    PURE_RPC_ERR_TOTP_REQUIRED        = -32007,
                                                                 
    PURE_RPC_ERR_TOTP_ENROLL_REQUIRED = -32008,
                            
    PURE_RPC_ERR_TOTP_INVALID_CODE    = -32009,
                                                         
    PURE_RPC_ERR_TOTP_PENDING_EXPIRED = -32010,
                                                   
    PURE_RPC_ERR_TOTP_LOCKED          = -32011,
                                                                     
                                                                
    PURE_RPC_ERR_HOSTKEY_UNTRUSTED    = -32012,
                                                                               
                                                                 
    PURE_RPC_ERR_HOSTKEY_MISMATCH     = -32013
} PureRpcErrorCode;

   
                                       
  
                                                
  
                
                                                                                       
  
                                                              
                                                          
                                                        
                                                            
   
gchar* pure_rpc_build_error_response(const gchar *rpc_id,
                                     PureRpcErrorCode code,
                                     const gchar *message);

   
                                       
  
                                             
  
                
                                                                         
  
                                                    
                                               
                                                       
                                                       
                                                    
                                                            
   
gchar* pure_rpc_build_success_response(const gchar *rpc_id,
                                       JsonNode *result_node);

   
                                         
  
                                                     
                                
  
                                                 
                                            
        
  
                                                        
                                               
                                                       
                                                      
                                                      
   
gboolean pcv_rpc_params_get_int_alias(JsonObject *params,
                                      const gchar *primary_key,
                                      const gchar *alias_key,
                                      gint *out_value);

   
                                
  
                                                  
                                                     
  
                                                             
                                                         
                                                     
  
                                                 
                                   
                                                              
   
const gchar *pcv_rpc_params_get_string(JsonObject *params, const gchar *key);

   
                                                          
  
                                                                             
  
                                                                                                         
                                          
                                                                               
   
gboolean pcv_rpc_params_is_object_array(JsonObject *params, const gchar *key);

   
                                      
  
                                                          
                                                            
  
                                                                                           
                                              
                                                                                              
   
gboolean pcv_rpc_params_get_bool(JsonObject *params, const gchar *key, gboolean default_value, gboolean *out);

   
                                       
  
                                                       
                                                           
  
                                                                      
                                                                      
  
                                                                
                                           
                                   
                                   
                                    
                                                              
                                                
   
gboolean pcv_rpc_params_get_uint_in_range(JsonObject *params, const gchar *key,
                                          guint min_value, guint max_value,
                                          guint default_value, guint *out);

                                        
                                               
                                                   
                                                 
                         
#define PCV_RPC_JSON_MAX_DEPTH 128

   
                                                             
                                                
                                                       
                                                      
   
gboolean pcv_rpc_json_depth_ok(const gchar *json, gint max_depth);

                                     
#define PCV_RPC_JSON_MAX_BYTES (1u * 1024u * 1024u)

   
                                                           
                                                                      
                                                       
                                            
                                                                       
   
gboolean pcv_rpc_parse_guarded(const gchar *data, gssize len,
                               JsonParser **parser, GError **err);

   
                                                              
                                                             
                                                                         
                                                     
                                                                             
                                         
   
gboolean pcv_source_remove_if_attached(guint source_id);

G_END_DECLS

#endif                             
