   
                                                                           
  
                           
                                                   
                                                    
                                        
  
                                                       
                                                         
                                                      
                            
  
                                                                      
                                                
                                                                  
                                                                    
                                                                    
                                                     
                                                            
                                 
                                                                                
                                                                         
   
#ifndef PURECVISOR_SECURITY_STORE_H
#define PURECVISOR_SECURITY_STORE_H

#include "modules/security/security_event.h"

G_BEGIN_DECLS

                                                                    
                                                                    
#define PCV_SECURITY_DB_DEFAULT "/var/lib/purecvisor/pcv_security.db"

  
                                                                               
                                                                          
                                                                               
             
   
                                                       
gboolean pcv_security_store_open(const gchar *path);
                                                                    
                                                      
gboolean pcv_security_store_ensure_open(void);
                               
void pcv_security_store_close(void);

                                                   
                                                          
                                                                      
gboolean pcv_security_submit_event(PcvSecurityEvent *ev, GError **error);
                                                                 
                                                        
gboolean pcv_security_store_insert_event(const PcvSecurityEvent *ev, GError **error);
                                                      
                                              
                                                                                       
JsonArray *pcv_security_store_list_events(gint offset, gint limit,
                                           const gchar *severity,
                                           const gchar *source,
                                           const gchar *status);
                                                                    
JsonObject *pcv_security_store_get_event(const gchar *event_id);
                                                                         
gboolean pcv_security_store_update_event_status(const gchar *event_id,
                                                PcvSecurityStatus status,
                                                GError **error);
                                                          
gint pcv_security_store_count_by_coalesce_key(const gchar *coalesce_key);
                                                           
gboolean pcv_security_store_get_bool_config(const gchar *key, gboolean def);
                                                         
                               
gboolean pcv_security_store_set_bool_config(const gchar *key,
                                            gboolean value,
                                            const gchar *admin_user,
                                            GError **error);
                                                              
JsonObject *pcv_security_store_health(void);
                                                                  
                                                       
gboolean pcv_security_store_upsert_pending_action(const PcvSecurityEvent *ev,
                                                  const gchar *action,
                                                  gint ttl_sec,
                                                  GError **error);
                                                  
JsonArray *pcv_security_store_list_pending_actions(void);
                                                                  
                                              
JsonObject *pcv_security_store_get_action(const gchar *event_id);
                                                                                    
                                                                  
gboolean pcv_security_store_action_is_expired(const gchar *event_id);
                                                                                 
gboolean pcv_security_store_update_action_status(const gchar *event_id,
                                                 const gchar *status,
                                                 const gchar *admin_user,
                                                 const gchar *reason,
                                                 GError **error);

  
                                                                    
                                                                         
                                                    
                                  
   
                                                              
                                                          
                               
gboolean pcv_security_store_put_wg_key(const gchar *tenant, const gchar *vm,
                                       const gchar *pubkey,
                                       const gchar *privkey_enc,
                                       const gchar *overlay_ip,
                                       GError **error);
                                                                      
                                                    
                                                         
gboolean pcv_security_store_get_wg_key(const gchar *tenant, const gchar *vm,
                                       gchar **pubkey_out,
                                       gchar **privkey_enc_out,
                                       gchar **overlay_ip_out,
                                       GError **error);
                                                                 
gboolean pcv_security_store_del_wg_key(const gchar *tenant, const gchar *vm,
                                       GError **error);

  
                                                                      
                                                              
                                                        
                                                               
                                                             
               
   

                                            
typedef struct {
    gchar *name;
    gint   subnet_index;                                     
} PcvOverlayTenantRow;

                                                                    
typedef struct {
    gchar *tenant;
    gchar *vm;
    gchar *pubkey;
    gchar *overlay_ip;
    gint   slot;                              
} PcvOverlayWgKeyRow;

void pcv_overlay_tenant_row_free(gpointer row);                       
void pcv_overlay_wg_key_row_free(gpointer row);                       

                                                                      
                               
gboolean pcv_security_store_put_tenant(const gchar *name, gint subnet_index,
                                       GError **error);
                                                
gboolean pcv_security_store_del_tenant(const gchar *name, GError **error);
                                                                                    
                                                       
gboolean pcv_security_store_list_tenants(GPtrArray **out_rows, GError **error);

                                                                       
                                                           
                                                                   
gboolean pcv_security_store_set_wg_key_slot(const gchar *tenant, const gchar *vm,
                                            gint slot, GError **error);
                                                                                   
                                                              
gboolean pcv_security_store_list_wg_keys(GPtrArray **out_rows, GError **error);

G_END_DECLS

#endif
