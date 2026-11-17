   
                              
                                                                                        
  
                           
                                                                                      
                                                                                           
                                                                                
                                                                                                 
                                                                                         
                                                                                
  
                                                                                   
                                                                           
   
#ifndef PURECVISOR_PCV_PRINCIPAL_NAMES_H
#define PURECVISOR_PCV_PRINCIPAL_NAMES_H

#include <glib.h>
#include <gio/gio.h>                                       

                                                                                                    
#define PCV_PRINCIPAL_AI_AGENT      "ai-agent"
#define PCV_PRINCIPAL_AI_OPS        "ai-ops"
                                                             
#define PCV_PRINCIPAL_UNKNOWN_ACTOR "-"
                                                                         
#define PCV_PRINCIPAL_TRIM_SQL      "char(32,9,10,12,13)"

                                                                                  
#define PCV_PRINCIPAL_ERROR (pcv_principal_error_quark())

                                             
typedef enum {
    PCV_PRINCIPAL_ERROR_RESERVED,                  
    PCV_PRINCIPAL_ERROR_KEY_NAME,                                        
    PCV_PRINCIPAL_ERROR_USER_NAME,                                
    PCV_PRINCIPAL_ERROR_DELETED,                           
    PCV_PRINCIPAL_ERROR_EXISTING_USER,                            
} PcvPrincipalError;

                                                                                   
GQuark pcv_principal_error_quark(void);

   
                                                                                                     
                                                                      
                              
   
gboolean pcv_principal_name_is_reserved(const gchar *name);

   
                                                                                                                  
                                                        
   
gchar *pcv_principal_name_normalize(const gchar *name);

   
                                                                                                                 
                                                                                          
                                                                                           
                                                  
   
gboolean pcv_principal_signup_name_conflict(const GError *err);

   
                                                                                                 
                                                                                                            
                               
                                                        
   
gboolean pcv_principal_tenant_lookup_applies(gint key_role);

   
                                                                                                                       
                                                                                      
                                               
   
gboolean pcv_rpc_method_name_is_valid(const gchar *name);

#endif                                       
