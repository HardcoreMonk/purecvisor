                                              
  
                                                                 
  
                           
                                                   
                                                    
                                        
  
                                                            
                                                 
                                                
  
                                                          
                                                                 
                                                                 
                                                                   
                                                          
                                                           
                                                  
                                                          
                                                    
                 
  
                                                               
                                                           
                                                   
   
#include "modules/daemons/pcv_undefine_debounce.h"

                                                       
typedef struct {
    gchar *name;                                      
    guint  token;                           
} PendingEntry;

                                           
struct PcvUndefineDebounce {
    GHashTable *pending;                                    
};

                                         
                                            
static void _entry_free(gpointer p) {
    PendingEntry *e = p;
    if (!e) return;                                               
    g_free(e->name);
    g_free(e);
}

                                                           
PcvUndefineDebounce *pcv_undefine_debounce_new(void) {
    PcvUndefineDebounce *d = g_new0(PcvUndefineDebounce, 1);
    d->pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _entry_free);
    return d;
}

                                              
void pcv_undefine_debounce_free(PcvUndefineDebounce *d) {
    if (!d) return;
    g_hash_table_destroy(d->pending);                            
    g_free(d);
}

                                                    
                                                                   
guint pcv_undefine_debounce_note_undefined(PcvUndefineDebounce *d,
                                           const char *uuid, const char *name,
                                           guint timer_token) {
    if (!d || !uuid) return 0;                                         
    guint old_token = 0;
                                                          
                                               
    PendingEntry *existing = g_hash_table_lookup(d->pending, uuid);
    if (existing) {
        old_token = existing->token;                                        
    }
    PendingEntry *e = g_new0(PendingEntry, 1);
    e->name  = g_strdup(name ? name : "");                                 
    e->token = timer_token;
                                                           
                                                      
    g_hash_table_replace(d->pending, g_strdup(uuid), e);             
    return old_token;
}

                                                              
                                                                     
guint pcv_undefine_debounce_note_defined(PcvUndefineDebounce *d, const char *uuid) {
    if (!d || !uuid) return 0;
    PendingEntry *e = g_hash_table_lookup(d->pending, uuid);
    if (!e) return 0;                                                
    guint token = e->token;                                                        
    g_hash_table_remove(d->pending, uuid);                           
    return token;
}

                                                         
                                                                     
gchar *pcv_undefine_debounce_take_expired(PcvUndefineDebounce *d, const char *uuid) {
    if (!d || !uuid) return NULL;
    PendingEntry *e = g_hash_table_lookup(d->pending, uuid);
    if (!e) return NULL;                                                     
    gchar *name = g_strdup(e->name);                                          
    g_hash_table_remove(d->pending, uuid);              
    return name;
}
