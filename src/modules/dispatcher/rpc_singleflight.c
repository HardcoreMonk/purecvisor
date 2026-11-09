                                                                             
                                            
                                                                          
  
                           
                                                              
                                                                              
  
                                                        
                                                                                
#include "rpc_singleflight.h"

struct PcvSingleflight {
    GHashTable   *waiters;                                                
    GDestroyNotify waiter_free;
};

                                                       
static void
_waiters_unref(gpointer arr)
{
    g_ptr_array_unref(arr);
}

                                       
                               
PcvSingleflight *
pcv_singleflight_new(GDestroyNotify waiter_free)
{
    PcvSingleflight *sf = g_new0(PcvSingleflight, 1);
    sf->waiters     = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _waiters_unref);
    sf->waiter_free = waiter_free;
    return sf;
}

                                                  
                                                                    
void
pcv_singleflight_free(PcvSingleflight *sf)
{
    if (!sf)
        return;
    g_hash_table_destroy(sf->waiters);
    g_free(sf);
}

                                                               
                                               
gboolean
pcv_singleflight_join(PcvSingleflight *sf, const gchar *key, gpointer waiter)
{
    g_return_val_if_fail(sf && key, FALSE);
    GPtrArray *arr = g_hash_table_lookup(sf->waiters, key);
    gboolean first = (arr == NULL);
    if (first) {
        arr = g_ptr_array_new_with_free_func(sf->waiter_free);
        g_hash_table_insert(sf->waiters, g_strdup(key), arr);
    }
    g_ptr_array_add(arr, waiter);
    return first;
}

                                                       
                                                 
GPtrArray *
pcv_singleflight_complete(PcvSingleflight *sf, const gchar *key)
{
    g_return_val_if_fail(sf && key, NULL);
    gpointer orig_key = NULL, val = NULL;
    if (!g_hash_table_lookup_extended(sf->waiters, key, &orig_key, &val))
        return NULL;
    GPtrArray *arr = g_ptr_array_ref(val);                                       
    g_hash_table_remove(sf->waiters, key);
    return arr;
}

                                                                                                      
                           
guint
pcv_singleflight_waiter_count(PcvSingleflight *sf, const gchar *key)
{
    g_return_val_if_fail(sf && key, 0);
    GPtrArray *arr = g_hash_table_lookup(sf->waiters, key);
    return arr ? arr->len : 0;
}
