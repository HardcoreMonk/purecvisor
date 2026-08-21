   
                  
                                                               
  
                           
                                                   
                                                    
                                        
  
                                                    
                                                
                                                 
                                                 
                                                 
                                                
                                                            
             
  
                         
                                                        
                                                            
                                                                    
                                                                 
                                    
  
                                                      
                                                
                                                   
                                                         
                                                                    
                                           
                                           
  
           
                                                          
                                                      
                                                 
                                                       
                                                  
                                                                
                                                
  
                
                                                                
                                                      
                                    
   
#ifndef PURECVISOR_PCV_QOS_H
#define PURECVISOR_PCV_QOS_H
#include <glib.h>
#include <json-glib/json-glib.h>
G_BEGIN_DECLS

#define PCV_QOS_METADATA_URI "urn:purecvisor:qos:1"
#define PCV_QOS_IFB_DEV      "pcvqos0"
#define PCV_QOS_IDS_PATH     "/var/run/purecvisor/qos_ids.json"
                                                                
                                                       
                                               
                                                         
#define PCV_QOS_TENANT_SLA_PATH "/var/lib/purecvisor/qos_tenants.json"

typedef struct {
    gchar   tenant[64];
    gchar   vm[64];
    guint32 min_mbps;                          
    guint32 max_mbps;                    
    guint32 burst_kb;                
} PcvQosSla;

                                              
                                                           
                                                       
                                              
guint16  pcv_qos_tenant_minor(const char *tenant);                                       
guint16  pcv_qos_vm_minor(const char *tenant, const char *vm);                           

                                                     
                                   
gchar   *pcv_qos_tenant_classid(const char *tenant);                                      
gchar   *pcv_qos_classid(const char *tenant, const char *vm);                             

                                                        
                                                        
gboolean pcv_qos_sla_from_json(JsonObject *o, PcvQosSla *out, GError **error);

   
                           
                                                
                                              
                                                 
                                                                  
                                                                          
                                                                 
  
                                                      
                                                      
                                                   
                                                               
                                                             
                                                 
                                                                
                                                   
                                 
  
                
                                                                       
                                                       
                                                               
                                     
                                                              
                                                                 
                                                              
                                                             
                                                          
                                                    
  
                                                         
                                                        
                                
  
                      
   
gboolean pcv_qos_vm_requires_sla(const gchar *network_mode, const gchar *nic_type);

                                           
                                                           
                                                                    
                                                          
                                                        
                                                  
                                                
                                                        
                                                 
                                                     
                                               
                                        
gboolean pcv_qos_ids_load(const char *path);
gboolean pcv_qos_ids_save(const char *path, GError **error);
void     pcv_qos_ids_clear(void);

                                                                             
                                                              
                                                                             
                                                  
                                                    
                                           
                                                   
                                                                  
                                                                    
                                                               
                                 
                                                 
                                                              
                                                              
                                                 
                   
  
       
                                                     
                                                           
                                                      
  
                                                          
                                                     
                                                               
                                                                
                        
  
                                                     
                                             
                                             
                                                     
                                                    
                                                     
                                     
  
                                                   
                                                
                                               
                                    
  
                                                                          
                                                 
                                                   
                                                   
                                                                  
                                                                   
                                                      
                                                               
                                                    
                                            
                                                                     
                                                         
                                               
                        
                                                              
                                                           
                
                                               
                                                       
                                                                   
                                                             
                                                      
                                                              
                                                           
                                                  
                                                               
                                                         
                                                 
                                                
                                                                                

   
                       
                                                   
                                                   
                                                                         
                     
  
                                                         
                                                          
                                             
  
                      
   
gboolean pcv_qos_ensure_root(guint32 uplink_mbps, GError **error);

                                             
                              
                                                                 
                                                                  
                                               
                                                           
                                                                        
                                            
                                                  
               
void pcv_qos_uplink_clear(void);

   
                    
                                                      
                                                     
                                                              
                                                                 
                                                                     
                                                             
                     
  
                                                   
                                                                      
                                                         
                                                         
  
                                                                     
                                               
                 
  
                                                     
   
gboolean pcv_qos_apply_vm(const char *vm_iface, const PcvQosSla *sla, GError **error);

   
                     
                                                     
                                                
                                                
                                              
              
                                                           
  
                                                                 
                                                   
                                                   
           
  
                                                    
                                                   
                                          
                                                          
                        
                                                        
                               
                                                      
                                                        
                                
                                                   
                                             
                          
  
                                                         
                                   
   
gboolean pcv_qos_remove_vm(const char *vm_iface, const char *tenant, const char *vm, GError **error);

                                                  
                                               
                                                              
                                                                 
                                                           
gboolean pcv_qos_lookup_applied(const char *vm, gchar **tenant_out, gchar **iface_out);

   
                            
                                                 
                                                  
                                                 
  
                                                            
                                                           
                                                             
                                                    
                                                  
                              
  
                                                                 
                                                    
                                                            
                      
  
                                                        
   
gboolean pcv_qos_iface_is_managed(const char *iface);

                                                                             
                                     
                                                                             
                                               
                                                     
                                                      
                                                   
                                                      
                                              
                                                   
                                                
                                                                 
                                                             
                                                                          
                                        
  
                                                     
                                                              
                                   
                                                                                

                                                                    
typedef struct {
    guint32 min_mbps;
    guint32 max_mbps;
} PcvQosTenantSla;

   
                          
                                                 
                                          
                                                             
                               
                                                            
                                          
                                      
                                                             
                                                           
                                                       
                                    
                                                           
                                                           
                                             
                                                            
  
                                                                    
                                                     
                                                  
                                                         
                                                                 
                
  
                                                  
                                                
                                                         
                                                                 
                                                
                        
  
                                                           
                          
   
gboolean pcv_qos_tenant_sla_set(const char *tenant, guint32 min_mbps, guint32 max_mbps,
                                 gboolean *live_applied_out, gchar **live_error_out,
                                 GError **error);

   
                          
                                              
                  
                                
  
                                                            
   
gboolean pcv_qos_tenant_sla_get(const char *tenant, PcvQosTenantSla *out);

                                                
                                                            
                                                           
                                                  
gboolean pcv_qos_tenant_sla_load(const char *path);
gboolean pcv_qos_tenant_sla_save(const char *path, GError **error);
void     pcv_qos_tenant_sla_clear(void);

   
                             
                                                   
                                         
                                                                  
                                              
                                          
  
                                                                           
                                                     
                                                            
                                                                     
                                                        
                                                          
                               
  
                                                          
                                                          
   
gboolean pcv_qos_reverse_lookup_vm(guint16 minor, gchar **tenant_out, gchar **vm_out);

                                                                             
                                                            
                                                                                

   
                                                                           
                                                  
                                            
                                                                       
                                                                   
                                                                
                         
                                                     
                                           
   
typedef struct {
    gchar   *classid;
    gchar   *tenant;
    gchar   *vm;
    guint64  bytes;
    guint64  drops;
} PcvQosClassStat;

   
                             
                                                    
                                                  
                                                                     
                    
  
                                                
                                                                    
                                                           
                                                                   
                                                             
                                                    
                                                     
  
                                                                 
                                                              
   
GPtrArray *pcv_qos_parse_class_stats(const gchar *output);

   
                         
                                                    
                                  
  
                                                                          
                                                             
                                                     
                                           
  
                                                                 
                                                   
                                                             
   
GPtrArray *pcv_qos_collect_stats(void);

   
                        
                                                       
                                     
  
                                                       
                                                                          
                                                               
                                                            
                                                   
  
                                                                          
                                                  
   
void pcv_qos_metrics_tick(void);

   
                               
                                                  
                                           
  
                                                             
                                              
                                                             
                                                              
                                                          
                                                     
                                       
  
                                
  
                                                            
   
guint pcv_qos_metrics_timer_start(void);

                                                                             
                                                      
                                                                             
                                                   
                                                     
                                                        
                                                          
                                                 
                                                   
  
                                                               
                                                 
                                                  
                                                   
                                                         
                                                 
  
                                                    
                                                            

   
                                                                  
                                                  
                                                     
                                               
                                                         
                                                     
   
typedef enum {
    PCV_QOS_RECON_OK,
    PCV_QOS_RECON_ORPHAN,
    PCV_QOS_RECON_MISSING
} PcvQosReconItem;

   
                                                        
                                            
                                                                       
                                                  
                                 
   
typedef struct {
    gchar          *classid;
    PcvQosReconItem action;
} PcvQosReconEntry;

   
                          
                                                
                                             
                                                                    
                                                      
                          
                                                                        
                          
  
                                               
                                                           
                  
  
                                                                   
                            
   
GPtrArray *pcv_qos_reconcile_diff(GPtrArray *expected_ids, GPtrArray *actual_ids);

   
                            
                                               
                                      
                                                                      
                                                                
  
                                                  
                                                             
                                                          
                                                         
                                                       
                                
  
                                                                       
                                                     
   
GPtrArray *pcv_qos_parse_class_show(const gchar *output);

   
                                                                    
                                                     
                                                
                                                 
  
                                                                 
                                                         
                                                     
                                                                
                                                            
                                                       
                                                
                                                              
   
typedef struct {
    gchar     tenant[64];
    gchar     vm[64];
    gchar     iface[16];                                                   
    PcvQosSla sla;                                                                    
} PcvQosExpectedEntry;

   
                                                           
                                                     
                                              
  
                                                                   
                                                            
                                                            
                                                             
                                                 
           
   
typedef GPtrArray *(*PcvQosExpectedProvider)(void);

   
                                 
                                            
                                                         
                   
  
                                                   
                                     
   
void pcv_qos_set_expected_provider(PcvQosExpectedProvider fn);

   
                     
                                                
                                                  
                                                
                                                    
  
                                                            
                                                        
                                                            
                                                                      
                                              
                                                               
                                                                    
                                                  
                                            
                                                   
                                                 
                                                       
                                                       
                                               
                                                      
                                                       
                       
                                                                  
                 
                                                
                                                    
                                 
  
                                                       
                                                              
                                                                  
                                                                      
                                                     
                                               
                                                  
                                                 
                                                 
                                                                  
                                       
  
                                               
                                                          
                                             
                         
   
gboolean pcv_qos_reconcile(GError **error);

   
                                 
                                                       
                                            
  
                                                                        
                                                                     
                                                              
                                                     
                                                    
  
                                                          
                                  
  
                                                                  
                       
   
guint pcv_qos_reconcile_timer_start(void);

G_END_DECLS
#endif                           
