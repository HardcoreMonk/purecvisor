   
                                                         
  
                           
                                                   
                                                    
                                        
  
                                                    
                                                 
                                                     
                                         
  
                                                                
                                                 
                                                   
                                                                
                                                                                
                                                
                                                           
                                                             
                                                                                 
                                                                         
                                                                          
                                                        
                                        
                                                                                        
                                                                                       
   
#include "rest_auth.h"

#include "utils/pcv_crypto.h"

  
                                
  
                                                 
                                             
                                                   
                                
  
                                                   
                                            
  
                                                                   
                                                                      
                                                               
                                                
                                              
   
gboolean
pcv_rest_auth_should_fallback_bootstrap(const gchar *username,
                                        const gchar *password,
                                        const gchar *cfg_user,
                                        const gchar *cfg_pass,
                                        gboolean user_in_db)
{
                                                          
    if (!username || !password || !cfg_user || !cfg_pass) return FALSE;
                                                   
                                                           
                                            
    if (g_strcmp0(username, cfg_user) != 0 ||
        !pcv_secret_str_eq(password, cfg_pass)) return FALSE;
                                                                
    return !user_in_db;                              
}

                      
const gchar *
pcv_rest_bearer_api_key(const gchar *authorization)
{
    if (!authorization)
        return NULL;
    while (g_ascii_isspace(*authorization))
        authorization++;
    if (g_ascii_strncasecmp(authorization, "Bearer", 6) != 0 || !g_ascii_isspace(authorization[6]))
        return NULL;
    const gchar *token = authorization + 6;
    while (g_ascii_isspace(*token))
        token++;
    return g_str_has_prefix(token, "pcv_") ? token : NULL;
}
