   
                            
                                                                                 
  
                           
                                                                             
                                                                                                     
                                                                              
                                                                                  
                                                                                 
                         
  
                                                                                 
                                    
   
#ifndef PURECVISOR_PCV_RBAC_INTERNAL_H
#define PURECVISOR_PCV_RBAC_INTERNAL_H

#include <sqlite3.h>
#include <glib.h>
#include "pcv_rbac.h"

                                                                                                    
sqlite3 *pcv_rbac_db_lock(void);
                                               
void pcv_rbac_db_unlock(void);

                                                                                   
                                                                                        
                                                                           
                                                                    
#define PCV_RBAC_ACCESS_TOKEN_EXPIRY   900                         
#define PCV_RBAC_REFRESH_TOKEN_EXPIRY  604800                                  

                                                                                                       
gchar *pcv_rbac_identity_nonce_new(void);
                                                                           
gchar *pcv_rbac_refresh_token_new(void);
                                                                      
gchar *pcv_rbac_refresh_token_hash(const gchar *token);
                                                                                                                   
gboolean pcv_rbac_totp_delete_rows_locked(const gchar *username);

   
                                                                               
                                                                                                      
                                              
                                                                                         
   
PcvSubjectCheck pcv_rbac_identity_check_locked(sqlite3 *db, const gchar *username,
                                               const gchar *identity_nonce, PcvRole *out_role);

                                                                                                              
PcvUserExistence pcv_rbac_lookup_subject_locked(sqlite3 *db, const gchar *username, PcvRole *out_role, gchar **out_nonce);

   
                                                                                                 
                                                                                     
                                                                                       
                                                                               
                                                               
   
void pcv_rbac_identity_migrate(void);

                                                                    
                                                                          
                                               

                                                                                      
gboolean pcv_rbac_attempts_login_locked(const gchar *username, gint *out_remain);
                                                                
void pcv_rbac_attempts_login_record(const gchar *username, gboolean success);
                                                                
gboolean pcv_rbac_attempts_totp_locked(const gchar *username, gint *out_remain);
                                                                   
void pcv_rbac_attempts_totp_record(const gchar *username, gboolean success);

#endif                                     
