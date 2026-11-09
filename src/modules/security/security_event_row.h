   
                                                                                      
  
                           
                                                                            
                                                                            
                                                                              
  
                                                              
                                             
  
                                                         
                                                    
                                                                 
   
#ifndef PURECVISOR_SECURITY_EVENT_ROW_H
#define PURECVISOR_SECURITY_EVENT_ROW_H

#include <glib.h>
#include <json-glib/json-glib.h>
#include <sqlite3.h>

G_BEGIN_DECLS

                                                                                                 
#define PCV_SEC_EVENT_SELECT_COLUMNS \
    "event_id,timestamp,source,type,severity,confidence,target_kind,target,summary," \
    "recommended_action,status,evidence_json,coalesce_key,occurrence_count,last_seen,created_at," \
    "closed_reason,closed_by,closed_at,close_note"

                                          
                                                               
JsonObject *pcv_security_event_row_to_json(sqlite3_stmt *stmt);

G_END_DECLS

#endif                                      
