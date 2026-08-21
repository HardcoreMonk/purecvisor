   
                          
                                                                        
  
                           
                                                              
                                                                  
                                                                 
                                                                 
                                                              
                                                    
  
                       
                                                    
                                              
                                                       
                                                     
   

#ifndef PCV_AUDIT_CHAIN_H
#define PCV_AUDIT_CHAIN_H

#include <glib.h>
#include <sqlite3.h>

G_BEGIN_DECLS

#define PCV_AUDIT_CHAIN_GENESIS \
    "0000000000000000000000000000000000000000000000000000000000000000"

typedef enum {
    PCV_AUDIT_CHAIN_ERROR_SQLITE,
    PCV_AUDIT_CHAIN_ERROR_CRYPTO,
    PCV_AUDIT_CHAIN_ERROR_INTEGRITY,
    PCV_AUDIT_CHAIN_ERROR_STATE,
} PcvAuditChainError;

#define PCV_AUDIT_CHAIN_ERROR (pcv_audit_chain_error_quark())
   
                               
  
                           
                                                    
                                               
                             
                                                 
  
                       
                                                      
   
GQuark pcv_audit_chain_error_quark(void);

   
                       
                                                      
                                                             
                                                          
       
  
                       
                                                   
                                     
   
typedef struct {
    const gchar *ts;
    const gchar *node;
    const gchar *username;
    const gchar *method;
    const gchar *target;
    const gchar *result;
    gint         error_code;
    gint64       duration_ms;
    const gchar *src_ip;
    const gchar *event_ts;
} PcvAuditChainRecord;

   
                       
                                                          
                                                              
             
  
                       
                                               
                                            
   
typedef struct {
    gboolean current_ok;
    gboolean historical_break;
    gint64   active_epoch;
    gint64   first_break_rowid;
    gint64   last_verified_at;
    gchar    reason[32];
} PcvAuditChainHealth;

   
                           
                                               
  
                           
                                                                          
                       
                                                                  
                                 
                                                                          
                                                                   
                                                                    
                                                
  
                       
                                                 
                                    
   
gboolean pcv_audit_chain_prepare(sqlite3 *db,
                                 PcvAuditChainHealth *health,
                                 GError **error);

   
                          
                                                           
  
                           
                                                             
                                                                
                                   
                                                                            
                                        
                                                                     
                                                                     
  
                       
                                                  
                                       
   
gboolean pcv_audit_chain_verify(sqlite3 *db,
                                PcvAuditChainHealth *health,
                                GError **error);

   
                          
                                                                  
  
                           
                                                                   
                                  
                                                                   
                                                           
                                                                               
                                             
                                                                           
                             
  
                       
                                                 
                                                         
   
gboolean pcv_audit_chain_append(sqlite3 *db,
                                const PcvAuditChainRecord *record,
                                gint64 *inserted_rowid,
                                GError **error);

   
                             
                                                          
  
                           
                                                                        
                                              
                                                                       
                                 
                                                                            
                                    
                                                                        
                                     
  
                       
                                                   
                                 
   
gboolean pcv_audit_chain_retention(sqlite3 *db,
                                   const gchar *cutoff_ts,
                                   gint *deleted_rows,
                                   GError **error);

   
                               
                                
  
                           
                                                                                 
                               
                                                              
                        
                                                      
                                                                            
                    
  
                       
                                                                
                                                   
               
   
gchar *pcv_audit_chain_record_hash(const gchar *prev_hash,
                                   const gchar *ts,
                                   const gchar *username,
                                   const gchar *method,
                                   const gchar *target,
                                   const gchar *result,
                                   gint error_code,
                                   GError **error);

G_END_DECLS

#endif                        
