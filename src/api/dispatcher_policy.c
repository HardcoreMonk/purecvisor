   
                            
                                                             
  
                           
                                                                           
                                                                      
                                                                
                                                   
  
                                                                    
   

#include "api/dispatcher.h"
#include "api/uds_server.h"
#include "api/snapshot_verify_probe.h"
#include "api/ova_import_xml.h"
#include "api/vm_batch_policy.h"
#include "api/vm_batch_queue.h"
#include "api/daemon_config_policy.h"
#include "bootstrap/pcv_bootstrap.h"
#include "modules/virt/vm_manager.h"
#include "modules/virt/cancellable_map.h"
#include "modules/virt/vm_clone_plan.h"
#include "modules/daemons/prometheus_exporter.h"
#include "modules/daemons/pcv_webpush.h"
#include "modules/audit/pcv_audit.h"
#include "modules/plugin/pcv_plugin_manager.h"
#include "modules/core/cpu_allocator.h"
#include "utils/pcv_secure.h"
#include "purecvisor/version.h"
#include <json-glib/json-glib.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "modules/dispatcher/handler_snapshot.h"
#include "modules/dispatcher/rpc_utils.h"
#include "modules/dispatcher/rpc_completion.h"
#include "modules/dispatcher/handler_vnc.h"
#include "modules/dispatcher/handler_vm_start.h"
#include "modules/dispatcher/handler_vm_lifecycle.h"
#include "modules/dispatcher/handler_vm_inspect.h"
#include "modules/dispatcher/handler_vm_hotplug.h"
#include "modules/network/network_manager.h"
#include "modules/network/vpc/vpc_manager.h"
#include "modules/network/pcv_qos.h"
#include "modules/network/pcv_qos_chaos.h"
#include "modules/daemons/pcv_trace.h"
#include "modules/security/pcv_suricata.h"
#include "modules/security/pcv_suricata_rules.h"
#include "modules/security/pcv_suricata_ips.h"
#include "modules/security/pcv_suricata_ips_rules.h"
#include "modules/dispatcher/handler_storage.h"
#include "modules/dispatcher/handler_monitor.h"
#include "modules/dispatcher/handler_container.h"
#include "modules/dispatcher/handler_overlay.h"
#include "modules/dispatcher/handler_accel.h"
#include "modules/accel/gpu_manager.h"
#include "modules/dispatcher/handler_template.h"
#include "modules/dispatcher/handler_auth.h"
#include "modules/dispatcher/handler_backup.h"
#include "modules/dispatcher/handler_security.h"
#include "modules/dispatcher/handler_tenant_overlay.h"
#include "modules/dispatcher/handler_vpc.h"
#include "modules/daemons/alert_engine.h"
#include "modules/daemons/process_monitor.h"
#include "modules/daemons/update_check.h"
#include "modules/virt/virt_conn_pool.h"
#include "utils/pcv_spawn.h"
#include "utils/pcv_config.h"
#include "utils/pcv_jwt.h"
#include "modules/auth/pcv_rbac.h"
#include "api/drain.h"
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "modules/lxc/lxc_driver.h"
#include "modules/storage/zfs_driver.h"
#include "modules/backup/backup_scheduler.h"
#include "modules/ai/workload_predict.h"
#include "utils/pcv_worker_pool.h"
#include "utils/pcv_log.h"
#include "utils/pcv_validate.h"
#include "utils/pcv_job_queue.h"
#include "modules/core/vm_state.h"
#include "api/ws_server.h"
#include <sqlite3.h>
#include <errno.h>
#include "api/dispatcher_policy.h"


                                                             
                                                                 
                                                                   
                                                            
typedef struct {
    const char *method;
    int         min_role;                                               
} PcvMethodPolicy;

                                                                          
static const PcvMethodPolicy g_method_policies[] = {
                                                                
    { "vm.inspect",                2 },
                                  
    { "vm.delete",                 1 },                                                   
    { "vm.snapshot.delete",        2 },
    { "vm.snapshot.delete_all",    2 },
    { "vm.snapshot.rollback",      2 },
    { "network.delete",            2 },
    { "network.create",            2 },
    { "storage.zvol.delete",       2 },
    { "storage.pool.destroy",      2 },
    { "container.destroy",         2 },
    { "auth.role.set",             2 },
    { "auth.user.create",          2 },
    { "auth.user.tenant.set",      2 },                                           
    { "resource.register",         2 },                                                   
    { "resource.unregister",       2 },
    { "resource.grant",            2 },
    { "resource.revoke",           2 },
    { "resource.list",             2 },                                                              
    { "resource.unregistered.list", 2 },
    { "resource.import",           2 },
    { "auth.user.delete",          2 },
    { "auth.password.reset",       2 },
    { "vm.export.ova",             2 },
                          
    { "vm.create",                 1 },
    { "vm.start",                  1 },
    { "vm.stop",                   1 },
    { "vm.pause",                  1 },
    { "vm.resume",                 1 },
    { "vm.limit",                  1 },
    { "vm.rename",                 1 },
    { "vm.snapshot.create",        1 },
    { "vm.guest.exec",             2 },                          
    { "vm.guest.shutdown",         1 },
    { "vm.guest.agent.ensure_channel", 1 },
    { "vm.mount_iso",              1 },
    { "vm.eject",                  1 },
    { "vm.vnc",                    1 },
    { "get_vnc_info",              1 },
    { "vm.resize_disk",            1 },
    { "vm.clone",                  1 },
    { "container.create",          1 },
    { "container.start",           1 },
    { "container.stop",            1 },
    { "container.clone",           1 },                                                                        
    { "container.exec",            2 },
                                   
    { "auth.apikey.create",        2 },
    { "auth.apikey.revoke",        2 },
    { "auth.user.delete",          2 },
    { "auth.password.reset",       2 },
    { "auth.role.set",             2 },
    { "alert.config.set",          2 },
    { "alert.config.reload",       2 },
    { "agent.config.set",          2 },
    { "healing.set_mode",          2 },                                                         
    { "anomaly.reset_baseline",    2 },                                            
    { "agent.compare_manual",      2 },                                             
    { "backup.set",                2 },
    { "backup.delete",             2 },
    { "backup.restore",            2 },
    { "backup.run",                2 },
    { "backup.replicate",          2 },
    { "backup.replication.hostkey.scan",   2 },                                       
    { "backup.replication.hostkey.trust",  2 },
    { "backup.replication.hostkey.list",   2 },                                             
    { "backup.replication.hostkey.remove", 2 },
    { "config.reload",             2 },
    { "config.set",                2 },
    { "container.snapshot.create", 2 },
    { "container.snapshot.delete", 2 },
    { "container.snapshot.rollback", 2 },
    { "container.set_bandwidth",   2 },
    { "container.set_limits",      2 },
    { "container.health.set",      2 },
    { "container.health.delete",   2 },
    { "container.nic.attach",      2 },
    { "container.nic.detach",      2 },
    { "container.volume.attach",   2 },
    { "container.volume.detach",   2 },
    { "container.env.set",         2 },
    { "container.env.delete",      2 },
    { "device.disk.attach",        2 },
    { "device.disk.detach",        2 },
    { "device.nic.attach",         1 },                                          
    { "device.nic.detach",         1 },
    { "device.gpu.attach",         2 },
    { "device.gpu.detach",         2 },
    { "dpdk.set",                  2 },
    { "iscsi.target.create",       2 },
    { "iscsi.target.delete",       2 },
    { "network.bind_phys",         2 },
    { "network.dhcp_toggle",       2 },
    { "network.mode_set",          2 },
    { "network.ovs.create",        2 },
    { "network.ovs.delete",        2 },
    { "network.ovs.vxlan.add",     2 },
    { "network.ovs.vxlan.del",     2 },
    { "nfv.deploy",                2 },
    { "nfv.delete",                2 },
    { "overlay.create",            2 },
    { "overlay.delete",            2 },
    { "overlay.add_peer",          2 },
    { "overlay.remove_peer",       2 },
    { "ovn.switch.create",         2 },
    { "ovn.switch.delete",         2 },
    { "ovn.router.create",         2 },
    { "ovn.router.delete",         2 },
    { "ovn.nat.add",               2 },
    { "ovn.nat.delete",            2 },
    { "plugin.load",               2 },
    { "plugin.unload",             2 },
    { "security_group.create",     2 },
    { "security_group.delete",     2 },
    { "security_group.detach",     2 },
    { "snapshot.schedule.set",     2 },
    { "snapshot.schedule.delete",  2 },
    { "sriov.set",                 2 },
    { "storage.pool.create",       2 },
    { "storage.pool.scrub",        2 },
                                                                   
                                                         
                                                                  
                                                         
    { "storage.zvol.create",       2 },
    { "storage.zvol.delete",       2 },
    { "template.create",           2 },
    { "template.delete",           2 },
                                                         
    { "tenant_overlay.create",     2 },
    { "tenant_overlay.delete",     2 },
    { "tenant_overlay.list",       2 },
    { "tenant_overlay.get",        2 },
    { "tenant_overlay.attach_vm",  2 },
    { "tenant_overlay.detach_vm",  2 },
                                                                     
                                                                      
    { "vpc.list",                   0 },
    { "vpc.get",                    0 },
    { "vpc.subnet.list",            0 },
    { "vpc.attachment.list",        0 },
    { "vpc.service.list",           0 },
    { "vpc.status",                 0 },
    { "vpc.backend.list",           0 },
    { "vpc.create",                 1 },
    { "vpc.delete",                 2 },
    { "vpc.egress.set",             1 },
    { "vpc.subnet.create",          1 },
    { "vpc.subnet.delete",          1 },
    { "vpc.attachment.create",      1 },
    { "vpc.attachment.delete",      1 },
    { "vpc.service.publish",        1 },
    { "vpc.service.unpublish",      1 },
    { "vpc.reconcile",              2 },
    { "vpc.subnet.confirm",         2 },
    { "vpc.pool.set",               2 },
    { "vpc.pool.list",              2 },
    { "tls.reload",                2 },
                     
    { "vm.blkio.set",              1 },
    { "vm.import.ova",             1 },
    { "vm.security_group.set",     1 },
    { "vm.set_bandwidth",          1 },
    { "vm.set_memory",             1 },
    { "vm.set_vcpu",               1 },
    { "vm.pin_vcpu",               1 },
    { "vm.snapshot.schedule.set",  1 },
    { "vm.snapshot.schedule.delete", 1 },
    { "vm.usb.attach",             1 },
    { "vm.usb.detach",             1 },
    { "vm.disk.live_resize",       1 },
                        
    { "dpdk.bridge.create",        2 },
    { "dpdk.bridge.delete",        2 },
    { "network.qos.set",           2 },
                                                          
                                                                  
                                                    
    { "qos.vm.set",                2 },
    { "qos.tenant.set",             2 },
                                                          
                                                                   
                                                                    
                                                       
                                                        
                                                            
                                              
                                             
                                                 
                           
    { "qos.chaos.start",            2 },
    { "qos.chaos.stop",             2 },
    { "qos.chaos.status",           2 },
                                                                
                                                   
                                                                 
                                                      
                                                 
    { "debug.trace.start",          2 },
    { "debug.trace.stop",           2 },
    { "debug.trace.status",         2 },
    { "debug.trace.list",           2 },
    { "debug.trace.report",         2 },
                                                                   
                                                            
                                                          
                                                             
    { "suricata.policy.set",        2 },
    { "suricata.policy.get",        2 },
    { "suricata.rules.update",      2 },
    { "suricata.status",            2 },
    { "suricata.ips.status",        2 },                                             
    { "suricata.ips.enable",        2 },
    { "suricata.ips.disable",       2 },
                                                                    
                                                            
                                                       
                                                                       
                                                                 
                                                                          
                                                      
                                                         
    { "suricata.ips.drop.list",     2 },
    { "suricata.ips.drop.add",      2 },
    { "suricata.ips.drop.remove",   2 },
    { "node.drain",                2 },
    { "ovn.tenant.create",         2 },
    { "sriov.attach",              2 },
    { "sriov.detach",              2 },
                                                                   
    { "vm.delete.status",          0 },
    { "vm.snapshot.list",          0 },
    { "vm.snapshot.schedule.list", 0 },
    { "vm.guest.agent.status",     0 },
    { "vm.guest.fsinfo",           0 },
    { "vm.numa.info",              0 },                                                          
    { "network.host.info",         0 },                                                      
    { "daemon.update_check",       0 },                                                                                       
                                                           
                                                              
    { "alert.history",             0 },
    { "healing.pending",           0 },
    { "healing.history",           0 },
    { "healing.status",            0 },                                                           
    { "agent.history",             0 },
    { "config.history",            0 },
    { "storage.pool.forecast",     0 },
    { "vm.batch",                  1 },
               
    { "auth.apikey.list",          2 },                          
    { "auth.user.list",            2 },
    { "auth.session.revoke",       2 },
    { "auth.user.sessions.revoke", 2 },                                                    
    { "backup.policy.set",         2 },
    { "backup.policy.delete",      2 },
    { "security.event.list",       PCV_ROLE_VIEWER },
    { "security.event.get",        PCV_ROLE_VIEWER },
    { "security.event.close",      PCV_ROLE_ADMIN },                                             
    { "security.action.pending",   PCV_ROLE_VIEWER },
    { "security.action.dismiss",   PCV_ROLE_OPERATOR },
    { "security.action.approve",   PCV_ROLE_ADMIN },
    { "security.baseline.status",  PCV_ROLE_VIEWER },
    { "security.baseline.refresh", PCV_ROLE_ADMIN },
    { "security.config.get",       PCV_ROLE_VIEWER },
    { "security.config.set",       PCV_ROLE_ADMIN },
    { "daemon.config.set",         2 },
                                                  
                                                                 
                                                              
                                                          
                                                            
                                          
    { "dpdk.bind",                 2 },
    { "dpdk.unbind",               2 },
    { "sriov.enable",              2 },
    { "sriov.disable",             2 },
    { "ovn.acl.add",               2 },
    { "ovn.dhcp.enable",           2 },
    { "ovn.port.add",              2 },
    { "ovn.port.remove",           2 },
    { "ovn.router.add_port",       2 },
    { "iscsi.connect",             2 },
    { "iscsi.disconnect",          2 },
    { "network.qos.remove",        2 },
    { "backup.incremental",        2 },
    { "backup.verify",             2 },
    { "backup.snapshot.verify",    2 },
    { "alert.silence",             2 },
    { "alert.dlq.list",            2 },
                                                                 
                                                               
                                                                 
                                                        
                                              
    { "alert.ack",                 1 },

                                              
                                                                     
                                                 
                                                   
                                             
                                                                               
                                             
                                                                  
                                    
                                                                       
                                                                           
                                                                        
    { "push.vapid.get",            0 },
    { "push.subscribe",            0 },
    { "push.unsubscribe",          0 },
                                                       
                                                               
                                                          
                                             
    { "push.mine",                 0 },
    { "push.list",                 2 },
    { "push.vapid.rotate",         2 },
    { "push.test",                 2 },

                                                                      
                                                                        
                                                   
                                                       
                                                     
                                                           
                                                             
                                                              
                                                          
               
                                                           
                                                            
                                                                
                                                                        
                                         
    { "auth.totp.status",               0 },
    { "auth.totp.disable",              0 },
    { "auth.totp.recovery.regenerate",  0 },
    { "auth.totp.reset",                2 },

                                                                              
                                                                    
                                                               
                                           
    { "ai.healing.approve",        2 },                               
    { "ai.healing.reject",         2 },                   
    { "node.resume",               2 },                                     
    { "security_group.rule.add",   2 },                                           
    { "security_group.rule.remove",2 },                  
    { "webhook.dlq.list",          2 },                                                               
    { "webhook.dlq.retry",         2 },                                               
    { "config.backup",             2 },                                                      
    { "jobs.cancel",               2 },                                                                                     

                                                           
    { "vm.numa.info",              0 },                       
    { "vm.sla.report",             0 },                   
    { "vm.schedule.list",          0 },                  
    { "capacity.forecast",         0 },                 
    { "vm.billing.report",         0 },                  
    { "vm.autostart",              1 },                               
    { "vm.schedule.set",           1 },                              

                                                    
                                                                  
                                                              
                                                     
                                                             
                                                           
                              
    { "agent.config.get",          0 },
    { "alert.action.list",         0 },
    { "alert.config.get",          0 },
    { "alert.silence.list",        0 },
    { "backup.policy.list",        0 },
    { "container.env.list",        0 },
    { "container.health.get",      0 },
    { "container.list",            0 },
    { "container.metrics",         0 },
    { "container.nic.list",        0 },
    { "container.snapshot.list",   0 },
    { "container.volume.list",     0 },
    { "daemon.config.get",         0 },                                           
    { "device.nic.list",           0 },
    { "dpdk.hugepage.info",        0 },                                         
    { "dpdk.list",                 0 },
    { "dpdk.status",               0 },
    { "gpu.list",                  0 },
    { "gpu.metrics",               0 },
    { "host.device.list",          0 },
    { "iscsi.target.list",         0 },
    { "iso.list",                  0 },
    { "jobs.get",                  0 },
    { "jobs.list",                 0 },
    { "monitor.fleet",             0 },
    { "monitor.metrics",           0 },
    { "monitor.processes",         0 },
    { "network.info",              0 },
    { "network.list",              0 },
    { "network.qos.get",           0 },
    { "overlay.info",              0 },
    { "overlay.list",              0 },
    { "ovn.acl.list",              0 },
    { "ovn.nat.list",              0 },
    { "ovn.router.list",           0 },
    { "ovn.status",                0 },
    { "ovn.switch.list",           0 },
    { "pool.conninfo",             0 },
    { "qos.tenant.get",            0 },
    { "qos.vm.get",                0 },
    { "quota.get",                 0 },
    { "security_group.list",       0 },
    { "sriov.list",                0 },
    { "sriov.status",              0 },
    { "storage.pool.list",         0 },
    { "storage.zvol.list",         0 },
    { "template.get",              0 },
    { "template.list",             0 },
    { "vm.blkio.get",              0 },
    { "vm.event.webhook.list",     0 },
    { "vm.list",                   0 },
    { "vm.metrics",                0 },
    { "vm.usb.list",               0 },
                                                                              
                                                                  
    { "backup.history",            1 },
    { "container.health.check",    1 },
    { "container.logs",            1 },
    { "container.memory.stats",    1 },
    { "ovn.router.detail",         1 },
    { "ovn.switch.detail",         1 },
    { "storage.pool.health",       1 },
    { "template.history",          1 },
    { "vm.cpu.stats",              1 },                                         
    { "vm.guest.ping",             1 },
    { "vm.memory.stats",           1 },
                                                            
    { "audit.search",              2 },                    
    { "daemon.version",            2 },                                                 
    { "db.migration.status",       2 },
    { "health.deep",               2 },
    { "jobs.status",               2 },
    { "prometheus.sd",             2 },
    { "qos.stats",                 2 },
    { "snapshot.schedule.status",  2 },
    { NULL, 0 }
};

                                                                             
                                                                           
                                                                        
                                                                             
                                                                               
static GHashTable *g_method_policy_map = NULL;

                                                                          
                                                       
                                                            
                                
                                                                   
                                                               
                                                  
void pcv_dispatcher_init_policy_map(void);
void pcv_dispatcher_init_policy_map(void)
{
    if (g_method_policy_map) return;                       
    g_method_policy_map = g_hash_table_new(g_str_hash, g_str_equal);
    for (int i = 0; g_method_policies[i].method; i++) {
        g_hash_table_insert(g_method_policy_map,
                            (gpointer)g_method_policies[i].method,
                            GINT_TO_POINTER(g_method_policies[i].min_role));
    }
}

                                                                            
                                                                       
                                                                      
                                                                                                
const gchar *pcv_dispatcher_policy_method_at(guint index);
const gchar *pcv_dispatcher_policy_method_at(guint index)
{
    return index < G_N_ELEMENTS(g_method_policies) ? g_method_policies[index].method : NULL;
}

                                                 
                                                 
                                                              
                                                      
                                                                   
                                                       
                                                           
                                                                      
                                                   
static int
_method_min_role(const gchar *method)
{
    if (!method) return 2;                      
                                                                            
    if (!g_method_policy_map) return 2;                                      
    gpointer val = NULL;
    if (!g_hash_table_lookup_extended(g_method_policy_map, method, NULL, &val))
        return 2;                                                   
    return GPOINTER_TO_INT(val);
}

                                                                    
                                       
                                         
                                                  
                                                           
                                                                  
gboolean pcv_dispatcher_check_rbac(const gchar *method, gint caller_role);
gboolean pcv_dispatcher_check_rbac(const gchar *method, gint caller_role) {
    int min = _method_min_role(method);
    return caller_role >= min;
}

                                                                        
                                                       
                                                            
                                                   
                                                                    
gboolean
pcv_disp_vm_method_requires_owner_scope(const gchar *method)
{
    if (!method)
        return FALSE;

    if (g_strcmp0(method, "get_vnc_info") == 0)                                  
        return TRUE;

    if (g_strcmp0(method, "device.nic.attach") == 0 ||
        g_strcmp0(method, "device.nic.detach") == 0)
        return TRUE;

    if (!g_str_has_prefix(method, "vm."))
        return FALSE;

    if (g_strcmp0(method, "vm.create") == 0 ||
        g_strcmp0(method, "vm.import.ova") == 0)
        return FALSE;

               
                                                
                                               
                                                       
                                              
    if (g_strcmp0(method, "vm.list") == 0 ||
        g_strcmp0(method, "vm.event.webhook.list") == 0 ||
        g_strcmp0(method, "vm.delete.status") == 0 ||
        g_strcmp0(method, "vm.snapshot.schedule.list") == 0)
        return FALSE;

    return TRUE;
}

                                                                  
static const gchar *const k_vm_target_alias_keys[] = {
    "name", "vm_id", "vm_name", "vm", NULL
};

                                                        
                                                    
                                                               
                                                         
                                                     
static gboolean
_vm_target_alias_merge(JsonObject *params, const gchar *key,
                       const gchar **chosen, const gchar **reason_out)
{
    if (!json_object_has_member(params, key))
        return TRUE;
    JsonNode *node = json_object_get_member(params, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING) {
        *reason_out = "Invalid params: VM target aliases (name/vm_id/vm_name/vm/source) "
                      "must be strings";
        return FALSE;
    }
    const gchar *value = json_node_get_string(node);
    if (!value || !*value)
        return TRUE;                                        
    if (*chosen && g_strcmp0(*chosen, value) != 0) {
        *reason_out = "Invalid params: VM target aliases (name/vm_id/vm_name/vm/source) "
                      "must name the same VM";
        return FALSE;
    }
    if (!*chosen)
        *chosen = value;
    return TRUE;
}

   
                                                                          
                        
  
                                                                
                                                         
                                                    
                                                 
                                                        
                                                        
  
                                                                                  
                                                              
                                                          
                                              
  
                                                    
                                                    
                  
  
                                                         
                 
                              
                                                          
                                       
                                                
                                       
   
gboolean pcv_dispatcher_vm_target_aliases_valid(const gchar *method, JsonObject *params,
                                                const gchar **reason_out);
gboolean pcv_dispatcher_vm_target_aliases_valid(const gchar *method, JsonObject *params,
                                                const gchar **reason_out)
{
    const gchar *reason = NULL;
    if (reason_out) *reason_out = NULL;
    if (!params || !pcv_disp_vm_method_requires_owner_scope(method) ||
        g_strcmp0(method, "vm.inspect") == 0 ||
        g_strcmp0(method, "vm.batch") == 0)
        return TRUE;

    const gchar *chosen = NULL;
    gboolean valid = TRUE;
    if (g_strcmp0(method, "vm.clone") == 0)
        valid = _vm_target_alias_merge(params, "source", &chosen, &reason);
    for (guint i = 0; valid && k_vm_target_alias_keys[i]; i++)
        valid = _vm_target_alias_merge(params, k_vm_target_alias_keys[i], &chosen, &reason);
    if (!valid && reason_out)
        *reason_out = reason;
    return valid;
}
