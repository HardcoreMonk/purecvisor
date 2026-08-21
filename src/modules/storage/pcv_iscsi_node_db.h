   
                            
                                                  
  
                           
                                                   
                                                    
                                        
  
                                                       
                               
  
                                                                        
                                                            
                                           
  
                                                             
                                                              
                                                        
   
#ifndef PURECVISOR_ISCSI_NODE_DB_H
#define PURECVISOR_ISCSI_NODE_DB_H

#include <glib.h>

G_BEGIN_DECLS

#define PCV_ISCSI_NODE_DB_PRIMARY_ROOT "/var/lib/iscsi/nodes"
#define PCV_ISCSI_NODE_DB_LEGACY_ROOT "/etc/iscsi/nodes"
#define PCV_ISCSI_NODE_DB_LOCK_ROOT "/run/lock/iscsi"
#define PCV_ISCSI_NODE_DB_LOCK_TIMEOUT_MS 30000U

   
                                
                                                                
                                           
   
typedef gboolean (*PcvIscsiNodeDbBeforeCommitFn)(guint index,
                                                  const gchar *record_path,
                                                  gpointer user_data,
                                                  GError **error);

typedef struct {
    PcvIscsiNodeDbBeforeCommitFn before_commit;
    gpointer user_data;
} PcvIscsiNodeDbHooks;

   
                                 
                                                                   
                                            
  
                                                   
                                                            
                                                          
                                                         
                                                  
                                       
                                                    
                                   
                                             
   
gboolean pcv_iscsi_node_db_set_chap_at(const gchar *node_root,
                                        const gchar *lock_root,
                                        guint lock_timeout_ms,
                                        const gchar *target_iqn,
                                        const gchar *portal,
                                        const gchar *username,
                                        const gchar *password,
                                        const PcvIscsiNodeDbHooks *hooks,
                                        GError **error);

   
                                            
                                                         
                                                                     
                                                         
                                                
  
                                                            
                                                       
   
gboolean pcv_iscsi_node_db_set_chap_candidates_at(
    const gchar *primary_node_root,
    const gchar *legacy_node_root,
    const gchar *lock_root,
    guint lock_timeout_ms,
    const gchar *target_iqn,
    const gchar *portal,
    const gchar *username,
    const gchar *password,
    GError **error);

                                                                         
gboolean pcv_iscsi_node_db_set_chap(const gchar *target_iqn,
                                     const gchar *portal,
                                     const gchar *username,
                                     const gchar *password,
                                     GError **error);

G_END_DECLS

#endif                                 
