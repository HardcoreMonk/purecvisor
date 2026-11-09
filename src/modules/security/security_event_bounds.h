   
                                                                                   
  
                           
                                                                          
                                                                   
                                 
  
                                                           
                                     
  
                                                                                   
                                                               
                                                          
                                                           
                                                                    
                                   
                                                                        
                                        
                                                                         
   
#ifndef PURECVISOR_SECURITY_EVENT_BOUNDS_H
#define PURECVISOR_SECURITY_EVENT_BOUNDS_H

#include <glib.h>
#include <sqlite3.h>

#include "modules/security/security_event.h"

G_BEGIN_DECLS

#define PCV_SEC_REPEAT_SUMMARY_SEC     3600                       
#define PCV_SEC_AUDIT_BUDGET_WINDOW_SEC 3600                                    
#define PCV_SEC_AUDIT_BUDGET_PER_HOUR  600                             
#define PCV_SEC_AUDIT_BUDGET_WARN_MAX  500                               
#define PCV_SEC_OPEN_FOLDABLE_CAP      5000                            
#define PCV_SEC_OPEN_EXPIRE_SEC        1209600                      
#define PCV_SEC_BOUNDS_BATCH           500                            
#define PCV_SEC_EXPIRE_CHECK_SEC       3600                       
#define PCV_SEC_MAX_RETAINED_EVENTS    20000                           

                                                                                                  
                                                     
gboolean pcv_sec_bounds_migrate(sqlite3 *db, GError **error);

                                                                                                
                                                            
gboolean pcv_sec_bounds_enforce_retention(sqlite3 *db, GError **error);

                                                         
                                                                         
#define PCV_SEC_SOURCE_SLOTS PCV_SECURITY_SOURCE_COUNT

                                 
typedef enum {
    PCV_SEC_INSERT_NEW = 1,
    PCV_SEC_INSERT_COALESCED = 2
} PcvSecInsertKind;

                                                      
                                                        
typedef struct {
    PcvSecInsertKind kind;
    gchar row_event_id[64];                                                        
    gboolean audit_new;                                                             
    gboolean repeat_due;                                                                   
    gint64 repeat_count;                                         
    guint flood_count[PCV_SEC_SOURCE_SLOTS];                                        
    gint64 flood_since[PCV_SEC_SOURCE_SLOTS];                    
    gint folded;                                                 
    gint64 folded_oldest_last_seen;                                        
    gint expired;                                             
    gint64 expired_before;                                              
} PcvSecInsertResult;

                                                         
                                                               
gboolean pcv_sec_bounds_apply(sqlite3 *db, const PcvSecurityEvent *ev, PcvSecInsertKind kind,
                              gint64 now, PcvSecInsertResult *out, GError **error);

                                           
                                                                                          
void pcv_sec_bounds_emit(const PcvSecurityEvent *ev, const PcvSecInsertResult *r);

                                                              
                                                            
gboolean pcv_sec_bounds_protected_over_cap(sqlite3 *db, gint64 now, gint *count_out);

#define PCV_SEC_CLOSE_NOTE_MAX 200                                           

                                                                                 
typedef enum {
    PCV_SEC_CLOSE_OK = 0,
    PCV_SEC_CLOSE_INVALID_REASON,                                                 
    PCV_SEC_CLOSE_NOT_FOUND,                               
    PCV_SEC_CLOSE_NOT_OPEN,                                          
    PCV_SEC_CLOSE_PENDING,                                                               
    PCV_SEC_CLOSE_ERROR                                                
} PcvSecCloseResult;

                                                                            
                                                                                      
                                                                     
PcvSecCloseResult pcv_sec_bounds_close_event(sqlite3 *db, const gchar *event_id, const gchar *actor,
                                             const gchar *reason, gint64 now, GError **error);

G_END_DECLS

#endif                                         
