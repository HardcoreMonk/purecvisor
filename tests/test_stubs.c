                                                                                   
                                                                                  
                                                                     
                                                           
                          
                     
  
                                                                               
                                   
                                                                               
                                                 
                                                          
                       
  
                                
                                                                 
                                                                 
                                                                        
  
                                            
                                     
  
              
                                                    
                                                        
                                                      
                                               
                                                  
                                               
  
                                                                 
                       
                                                                               
   

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "api/uds_server.h"
#include "modules/audit/pcv_audit_chain.h"
#include "modules/daemons/telemetry.h"

                   
                                            
                                 
void pcv_cluster_sync_vm_xml(const gchar *name __attribute__((unused)),
                              const gchar *xml __attribute__((unused))) { }
void pcv_cluster_remove_vm_xml(const gchar *name __attribute__((unused))) { }
void pcv_cluster_notify_config_reload(void) { }
gboolean pcv_cluster_check_zvol_fence(void) { return TRUE; }

                                   
                                                                             
                                                           
                                                
                                                                
                                                                             
                                                                
                                                            
                                                                         
                                                         

                                                                      
                                                                  
                                                                 
                                                                    
                                                    
gboolean
pcv_vpc_bridge_is_managed(const gchar *bridge_name __attribute__((unused)))
{
    return FALSE;
}

                                                    
                                                         
                
void pcv_prom_gauge_set_labels(const gchar *n __attribute__((unused)),
    const gchar *l __attribute__((unused)),
    double v __attribute__((unused))) { }

                                                                     
                                                            
                                                   
VmMetrics *get_vm_metrics(const gchar *vm_id __attribute__((unused))) { return NULL; }

                                                               
                                                                    
                                     
  
                                                                  
                                                            
                                                                
                                            
gchar *pcv_prom_render(void) { return NULL; }

                                                
                                                      
                                                             
                                                      
static gint g_test_audit_call_count = 0;
static gchar g_test_audit_last_method[128] = {0};
static gchar g_test_audit_last_target[256] = {0};
static gint g_test_alert_call_count = 0;
static gchar g_test_alert_last_event_id[128] = {0};
static GMutex g_test_telemetry_mu;
static gboolean g_test_telemetry_enabled = FALSE;
static gdouble g_test_telemetry_cpu_percent = 0.0;
static gdouble g_test_telemetry_mem_percent = 0.0;
static gint g_test_telemetry_call_count = 0;

void pcv_test_audit_reset(void)
{
    g_test_audit_call_count = 0;
    g_test_audit_last_method[0] = '\0';
    g_test_audit_last_target[0] = '\0';
}

gint pcv_test_audit_call_count(void)
{
    return g_test_audit_call_count;
}

const gchar *pcv_test_audit_last_method(void)
{
    return g_test_audit_last_method;
}

const gchar *pcv_test_audit_last_target(void)
{
    return g_test_audit_last_target;
}

void pcv_test_alert_reset(void)
{
    g_test_alert_call_count = 0;
    g_test_alert_last_event_id[0] = '\0';
}

gint pcv_test_alert_call_count(void)
{
    return g_test_alert_call_count;
}

const gchar *pcv_test_alert_last_event_id(void)
{
    return g_test_alert_last_event_id;
}

                                                         
                                                  
                                                 
void pcv_test_alert_record_security_event_hook(
    const gchar *event_id,
    const gchar *severity __attribute__((unused)),
    const gchar *summary __attribute__((unused)))
{
    g_test_alert_call_count++;
    g_strlcpy(g_test_alert_last_event_id, event_id ? event_id : "",
              sizeof g_test_alert_last_event_id);
}

void pcv_audit_log(const gchar *username __attribute__((unused)),
                   const gchar *method,
                   const gchar *target,
                   const gchar *result __attribute__((unused)),
                   gint error_code __attribute__((unused)),
                   gint64 duration_ms __attribute__((unused)),
                   const gchar *src_ip __attribute__((unused)))
{
    g_test_audit_call_count++;
    g_strlcpy(g_test_audit_last_method, method ? method : "",
              sizeof g_test_audit_last_method);
    g_strlcpy(g_test_audit_last_target, target ? target : "",
              sizeof g_test_audit_last_target);
}

PcvAuditChainHealth pcv_audit_get_chain_health(void)
{
    PcvAuditChainHealth health = {.current_ok = TRUE};
    g_strlcpy(health.reason, "test_stub", sizeof(health.reason));
    return health;
}

void pcv_ws_broadcast_job_complete(const gchar *job_id __attribute__((unused)),
                                   const gchar *method __attribute__((unused)),
                                   const gchar *status __attribute__((unused)),
                                   const gchar *error_msg __attribute__((unused))) { }

                                                                 
                                                              
                                               
                     
void pcv_ws_broadcast_job_complete_mt(const gchar *job_id __attribute__((unused)),
                                      const gchar *method __attribute__((unused)),
                                      const gchar *status __attribute__((unused)),
                                      const gchar *error_msg __attribute__((unused))) { }

                                                                            
                                                            
                                                
void pcv_agent_compare_async(const gchar *metrics_json __attribute__((unused)),
                             const gchar *anomaly_context __attribute__((unused))) { }
                                                            
                                                     
                                                     
void
pcv_test_alert_telemetry_set(gboolean enabled, gdouble cpu_percent,
                             gdouble mem_percent)
{
    g_mutex_lock(&g_test_telemetry_mu);
    g_test_telemetry_enabled = enabled;
    g_test_telemetry_cpu_percent = cpu_percent;
    g_test_telemetry_mem_percent = mem_percent;
    g_test_telemetry_call_count = 0;
    g_mutex_unlock(&g_test_telemetry_mu);
}

gint
pcv_test_alert_telemetry_call_count(void)
{
    g_mutex_lock(&g_test_telemetry_mu);
    gint count = g_test_telemetry_call_count;
    g_mutex_unlock(&g_test_telemetry_mu);
    return count;
}

JsonObject *
pcv_ebpf_telemetry_get_host(void)
{
    g_mutex_lock(&g_test_telemetry_mu);
    if (!g_test_telemetry_enabled) {
        g_mutex_unlock(&g_test_telemetry_mu);
        return NULL;
    }
    g_test_telemetry_call_count++;
    gdouble cpu = g_test_telemetry_cpu_percent;
    gdouble mem = g_test_telemetry_mem_percent;
    g_mutex_unlock(&g_test_telemetry_mu);

    JsonObject *host = json_object_new();
    json_object_set_double_member(host, "cpu_percent", cpu);
    json_object_set_double_member(host, "mem_percent", mem);
    return host;
}
void pcv_ws_broadcast(const gchar *type __attribute__((unused)),
                      const gchar *payload_json __attribute__((unused))) { }
gint pcv_ws_client_count(void) { return 0; }
                                                                    
typedef struct _virDomain  *virDomainPtr;                                
typedef struct _virConnect *virConnectPtr;
virDomainPtr pure_virt_get_domain(virConnectPtr conn __attribute__((unused)),
                                  const gchar *identifier __attribute__((unused))) { return NULL; }

                                             
                                                               
                               
typedef struct PcvEtcdClient PcvEtcdClient;
PcvEtcdClient *pcv_cluster_get_etcd(void) { return NULL; }
gboolean pcv_etcd_acquire_inflight_lock(PcvEtcdClient *c __attribute__((unused)),
    const gchar *p __attribute__((unused)),
    const gchar *n __attribute__((unused)),
    const gchar *o __attribute__((unused)),
    gint t __attribute__((unused)),
    GError **e __attribute__((unused))) { return TRUE; }
gboolean pcv_etcd_release_inflight_lock(PcvEtcdClient *c __attribute__((unused)),
    const gchar *p __attribute__((unused)),
    GError **e __attribute__((unused))) { return TRUE; }
gint pcv_etcd_compute_inflight_ttl(const gchar *op __attribute__((unused)),
    gint size_gb __attribute__((unused))) { return 60; }

                                                      
                                                                         
                                                           
                                                                 
                                                                     
                                         
void pure_uds_server_send_response(UdsServer *self __attribute__((unused)),
                                   GSocketConnection *connection __attribute__((unused)),
                                   const gchar *response __attribute__((unused))) { }
