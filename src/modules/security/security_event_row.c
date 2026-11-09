   
                                                                                     
  
                           
                                                                                    
                                                                       
                                                                       
                                                           
  
                                                         
  
                                                                          
                                                                  
                                                        
   
#include "modules/security/security_event_row.h"

                                                                        
static const gchar *
row_text(sqlite3_stmt *stmt, gint idx)
{
    const unsigned char *s = sqlite3_column_text(stmt, idx);
    return s ? (const gchar *)s : "";
}

  
                                                                            
                                                        
                                                                           
   
JsonObject *
pcv_security_event_row_to_json(sqlite3_stmt *stmt)
{
      
                                                       
      
                                                                              
                                                                           
                                             
                                                                 
                                                                   
       
    JsonObject *obj = json_object_new();
    json_object_set_string_member(obj, "event_id", row_text(stmt, 0));
    json_object_set_int_member(obj, "timestamp", sqlite3_column_int64(stmt, 1));
    json_object_set_string_member(obj, "source", row_text(stmt, 2));
    json_object_set_string_member(obj, "type", row_text(stmt, 3));
    json_object_set_string_member(obj, "severity", row_text(stmt, 4));
    json_object_set_int_member(obj, "confidence", sqlite3_column_int(stmt, 5));
    json_object_set_string_member(obj, "target_kind", row_text(stmt, 6));
    json_object_set_string_member(obj, "target", row_text(stmt, 7));
    json_object_set_string_member(obj, "summary", row_text(stmt, 8));
    json_object_set_string_member(obj, "recommended_action", row_text(stmt, 9));
    json_object_set_string_member(obj, "status", row_text(stmt, 10));
    json_object_set_string_member(obj, "evidence_json", row_text(stmt, 11));
    json_object_set_string_member(obj, "coalesce_key", row_text(stmt, 12));
    json_object_set_int_member(obj, "occurrence_count", sqlite3_column_int(stmt, 13));
    json_object_set_int_member(obj, "last_seen", sqlite3_column_int64(stmt, 14));
    json_object_set_int_member(obj, "created_at", sqlite3_column_int64(stmt, 15));
                                                                                           
    json_object_set_string_member(obj, "closed_reason", row_text(stmt, 16));
                                                                                       
    json_object_set_string_member(obj, "closed_by", row_text(stmt, 17));
    json_object_set_int_member(obj, "closed_at", sqlite3_column_int64(stmt, 18));
    json_object_set_string_member(obj, "close_note", row_text(stmt, 19));
    return obj;
}
