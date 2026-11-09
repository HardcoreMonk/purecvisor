   
                         
                                                               
                                  
  
                           
                                                               
                                                          
                                                               
                                                                      
                                                                   
                                                                   
                                  
  
                       
                                                            
                                                       
                 
  
                                                                     
   
#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

                                               
#define PCV_VPC_MGMT_PENDING_PATH "/var/lib/purecvisor/vpc_confirm_pending.json"
#define PCV_VPC_MGMT_SKIP_FLAG "/etc/purecvisor/vpc-reconcile.skip"
#define PCV_VPC_MGMT_DEFAULT_TIMEOUT_SEC 120

                                                            
                
typedef enum {
    PCV_VPC_MGMT_FLAG_ABSENT,
    PCV_VPC_MGMT_FLAG_VALID,
    PCV_VPC_MGMT_FLAG_IGNORED
} PcvVpcMgmtFlag;

                                                                                   
guint pcv_vpc_mgmt_confirm_timeout_sec(void);

                                                                                
                                     
GArray *pcv_vpc_mgmt_ports(void);

                                                                        
                                                                         
                                      
gboolean pcv_vpc_mgmt_parse_proc_net_tcp(const gchar *text, gboolean ipv6,
                                         const guint16 *ports, guint n_ports,
                                         GPtrArray *out_cidrs);

                                                                         
                                                                              
GPtrArray *pcv_vpc_mgmt_reserved_set(guint *config_errors_out, GError **error);

                                                                     
                                                                
gboolean pcv_vpc_mgmt_cidr_overlaps_any(const gchar *cidr, GPtrArray *set, const gchar **hit_out);

                                                             
gboolean pcv_vpc_mgmt_cidr_within(const gchar *inner, const gchar *outer);

                                                                                                   
                                                                                                
                                                                   
                                                                                                           
                                                                                           
                                                                                        
                                                                                 
gboolean pcv_vpc_mgmt_pool_covers(GPtrArray *pool, const gchar *cidr);
gchar *pcv_vpc_mgmt_pool_outside(GPtrArray *pool, JsonArray *subnet_cidrs);

                                                                
PcvVpcMgmtFlag pcv_vpc_mgmt_skip_flag_state(void);

                                                             
                                          
void pcv_vpc_mgmt_set_paths_for_test(const gchar *pending_path, const gchar *flag_path,
                                     const gchar *proc_tcp, const gchar *proc_tcp6);

                                                                             
gchar *pcv_vpc_mgmt_warning_text(const gchar *hit, guint timeout_sec, const gchar *subnet_id);

                                                                        
                                                                     
                                                                                       
                                                                            
                                                                       
                       
typedef struct {
    gchar *subnet_id;
    gchar *vpc_id;
    gchar *tenant;
    gchar *cidr;
    gchar *requested_by;
    gboolean revert_vpc;
    gint64 deadline_unix;
    guint attempts;
} PcvVpcMgmtPending;

                        
void pcv_vpc_mgmt_pending_free(PcvVpcMgmtPending *p);

                                                  
PcvVpcMgmtPending *pcv_vpc_mgmt_pending_copy(const PcvVpcMgmtPending *p);

                                                              
                                                                             
                                        
gboolean pcv_vpc_mgmt_pending_load(GError **error);

                                                                
                                                                           
                                                                         
                                                                                
gboolean pcv_vpc_mgmt_pending_add(const PcvVpcMgmtPending *p, GError **error);

                                                                     
                                                                                          
                                                                                                 
                                                                                 
PcvVpcMgmtPending *pcv_vpc_mgmt_pending_claim(const gchar *subnet_id, GError **error);

                                                               
                                                                
                                                                 
                                                                      
PcvVpcMgmtPending *pcv_vpc_mgmt_pending_claim_for_revert(const gchar *subnet_id);

                                                                              
                                                                       
gboolean pcv_vpc_mgmt_pending_lookup(const gchar *subnet_id, gint64 *deadline_out);

                                                                      
                                                                                       
                                                                    
gboolean pcv_vpc_mgmt_expiry_due(const gchar *subnet_id, gint64 now_unix, gint64 *remaining_out);

                                                                    
guint pcv_vpc_mgmt_pending_count(const gchar *tenant);

                                                                              
GPtrArray *pcv_vpc_mgmt_pending_snapshot(void);

                                                                                 
                                                                    
gboolean pcv_vpc_mgmt_confirm(const gchar *subnet_id, GError **error);

                                                             
typedef struct {
    gboolean (*subnet_delete)(const gchar *id, const gchar *tenant, GError **error);
    gboolean (*vpc_delete)(const gchar *id, const gchar *tenant, GError **error);
    gboolean (*reconcile)(GError **error);
    void (*audit)(const gchar *actor, const gchar *method, const gchar *target,
                  const gchar *result, gint code);
    void (*arm)(const PcvVpcMgmtPending *p);
} PcvVpcMgmtOps;

                                                                                  
                                                                           
                                                                     
                                                                           
                                                                         
                                                                                                 
                         
gboolean pcv_vpc_mgmt_revert(const PcvVpcMgmtOps *ops, PcvVpcMgmtPending *claimed,
                             gboolean at_boot, gint64 now_unix);

                                                                    
                                                                
                                                                                           
                                                                         
                                         
                                                                 
                                                 
                                                                            
                                                          
                                                                              
                                                                             
               
gboolean pcv_vpc_mgmt_boot_with(const PcvVpcMgmtOps *ops, gint64 now_unix, GError **error);

                                                                      
                       
gboolean pcv_vpc_mgmt_boot_reconcile_skipped(void);

                                                                        
                                                                                    
const PcvVpcMgmtOps *pcv_vpc_mgmt_runtime_ops(void);

                                                              
                                                                                       
                                                      
void pcv_vpc_mgmt_arm_pending(const PcvVpcMgmtPending *p);

                                                                        
gboolean pcv_vpc_mgmt_boot_reconcile(GError **error);

G_END_DECLS
