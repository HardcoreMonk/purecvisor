   
                          
                                                  
  
                           
                                                   
                                                    
                                        
  
                                                      
                                               
  
                                                                   
                                                  
  
                 
                                        
                                                               
                                                                          
                                                   
  
                                                      
                                                   
   
#include "vm_batch_policy.h"

                                                     
gboolean pcv_vm_batch_action_is_whitelisted(const gchar *action)
{
                                                                                      
    static const gchar *WHITELIST[] = { "start", "stop", NULL };
    if (!action)                                       
        return FALSE;
                                                        
    for (int i = 0; WHITELIST[i]; i++)
        if (g_strcmp0(action, WHITELIST[i]) == 0)
            return TRUE;
    return FALSE;                          
}

                                                            
const gchar *pcv_vm_batch_name_reject_reason(const gchar *vm, GHashTable *seen)
{
    if (!vm || !*vm)
        return "empty vm name";
    if (g_hash_table_contains(seen, vm))
        return "duplicate";
    g_hash_table_add(seen, g_strdup(vm));
    return NULL;
}

                                                   
guint pcv_vm_batch_stop_concurrency(guint pool_max_conn)
{
    guint k = pool_max_conn > 1 ? pool_max_conn - 1 : 1;
    return MIN(4u, k);
}
