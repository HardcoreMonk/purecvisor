   
                              
                                                                                          
  
                           
                                                                                   
                                                                                              
                                                                                                             
  
                                                                 
                                                                  
   
#ifndef PURECVISOR_REPLICATION_HOSTKEY_H
#define PURECVISOR_REPLICATION_HOSTKEY_H

#include <glib.h>

G_BEGIN_DECLS

                                                                                 
#define PCV_REPL_KNOWN_HOSTS_PATH     "/var/lib/purecvisor/replication_known_hosts"
#define PCV_REPL_SSH_KEY_PATH         "/etc/purecvisor/cluster_id_ed25519"
#define PCV_REPL_CONNECT_TIMEOUT_SEC  10
#define PCV_REPL_PROBE_TIMEOUT_SEC    30
#define PCV_REPL_SCAN_TIMEOUT_SEC     30

                                                               
#define PCV_REPL_HOSTKEY_ERROR (pcv_repl_hostkey_error_quark())
typedef enum {
    PCV_REPL_HOSTKEY_ERROR_INVALID,                                 
    PCV_REPL_HOSTKEY_ERROR_UNTRUSTED,                             
    PCV_REPL_HOSTKEY_ERROR_MISMATCH,                                    
    PCV_REPL_HOSTKEY_ERROR_CONFLICT,                             
    PCV_REPL_HOSTKEY_ERROR_NOT_FOUND,                            
    PCV_REPL_HOSTKEY_ERROR_FAILED,                                     
} PcvReplHostkeyErrorCode;

                                                                                
GQuark pcv_repl_hostkey_error_quark(void);

                                                                        
typedef struct {
    gchar *target;                             
    gchar *type;                                                        
    gchar *blob_b64;                               
    gchar *fingerprint;                                                                   
} PcvReplHostKey;

                                                               
void pcv_repl_host_key_free(PcvReplHostKey *key);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PcvReplHostKey, pcv_repl_host_key_free)

                                                                                         
                                           
void pcv_repl_known_hosts_set_path_for_test(const gchar *path);

                                                                                             
                                                                                   
gchar *pcv_repl_canonical_target(const gchar *target_node);

                                                                                         
                                                                
                                                                               
gchar **pcv_repl_ssh_argv(const gchar *ssh_user, const gchar *target_node,
                          const gchar * const *remote_argv);

                                                                                     
gchar **pcv_repl_probe_argv(const gchar *ssh_user, const gchar *target_node);

                                                                                                
gchar *pcv_repl_fingerprint(const gchar *blob_b64);

                                                                                      
                                                    
                                  
PcvReplHostKey *pcv_repl_hostkey_lookup(const gchar *target_node, GError **error);

                                                                                                     
                                                                      
gboolean pcv_repl_hostkey_require(const gchar *target_node, GError **error);

                                                                                                       
                                                        
PcvReplHostkeyErrorCode pcv_repl_classify_ssh_stderr(const gchar *ssh_stderr);

                                                                                             
                                                                                
void pcv_repl_set_ssh_error(GError **error, const gchar *target_node, const gchar *ssh_stderr);

                                                                                  
                                                                                     
gboolean pcv_repl_hostkey_probe(const gchar *ssh_user, const gchar *target_node, GError **error);

                                                                                                     
                                                                                   
GPtrArray *pcv_repl_parse_keyscan(const gchar *output);

                                                                                                  
                                                                                     
gboolean pcv_repl_keyscan(const gchar *target_node, GPtrArray **out_keys, GError **error);

                                                                                         
gboolean pcv_repl_fingerprint_valid(const gchar *fingerprint);

                                                                                      
                                                            
gboolean pcv_repl_hostkey_add(const gchar *target_node, const PcvReplHostKey *key,
                              gboolean *out_added, GError **error);

                                                                           
gboolean pcv_repl_hostkey_remove(const gchar *target_node, GError **error);

                                                                                          
GPtrArray *pcv_repl_hostkey_list(GError **error);

                                                                                                    
                                                                 
gint pcv_repl_hostkey_rpc_code(const GError *error);

G_END_DECLS

#endif                                       
