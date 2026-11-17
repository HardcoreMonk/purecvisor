   
                            
                                                                                 
  
                           
                                                                             
                                                                                                     
                                                                              
                                                                                  
                                                                                 
                         
  
                                                                                 
                                    
   
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

                                                                                  
                                                                                      
                                                                                     
                                                                                                 
                                                                      

                                                                                                               
void pcv_rbac_names_migrate_locked(sqlite3 *db);

   
                                                                                  
                                                                      
                                                                                                     
                                                                                 
                                                                                                        
                                               
   
gboolean pcv_rbac_names_insert_user_locked(sqlite3 *db, const gchar *username, const gchar *hash, const gchar *salt,
                                           PcvRole role, const gchar *tenant, const gchar *nonce,
                                           gboolean reuse_deleted_name, gboolean *out_reused, GError **error);

   
                                                                                                     
                                                                                                                  
                                                     
   
gboolean pcv_rbac_names_insert_apikey_locked(sqlite3 *db, const gchar *key_hash, const gchar *client_name, PcvRole role,
                                             const gchar *description, gint64 expires_at, GError **error);

   
                                                                                                          
                                                             
   
gboolean pcv_rbac_names_tombstone_locked(sqlite3 *db, const gchar *username);

                                                                                                     
void pcv_rbac_names_log_bootstrap_refusal(const gchar *admin_user, const GError *err);

                                                                    
                                                                          
                                               

                                                                                      
gboolean pcv_rbac_attempts_login_locked(const gchar *username, gint *out_remain);
                                                                
void pcv_rbac_attempts_login_record(const gchar *username, gboolean success);
                                                                
gboolean pcv_rbac_attempts_totp_locked(const gchar *username, gint *out_remain);
                                                                   
void pcv_rbac_attempts_totp_record(const gchar *username, gboolean success);

#endif                                     
